// Engine unit tests. Headless by design (D-003) — no window, no display server.
//
// These cover the five things USER-STORIES.md names as carrying tests: dab
// spacing, tile compositing, the premultiply round-trip, undo/redo symmetry,
// and (once M2 lands) pressure normalisation.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <new>
#ifdef _WIN32
#include <malloc.h>   // _aligned_malloc / _aligned_free
#endif
#include <vector>

#include "sbl/backend.hpp"
#include "sbl/canvas.hpp"
#include "sbl/format.hpp"
#include "sbl/io.hpp"
#include "sbl/paint.hpp"
#include "sbl/project.hpp"
#include "sbl/select.hpp"

#include "lodepng.h"
// App-side, but deliberately SDL-free so the view transform is testable here.
#include "view_transform.hpp"
#include "miniz.h"
#include "miniz_zip.h"
#include "nlohmann/json.hpp"

using namespace sbl;

// US-02.9 asks for a counter or a profiler, not an eye. Replacing global
// operator new is the counter: it sees every allocation the engine makes,
// including ones hidden inside a std::vector growing by one.
namespace {
std::size_t g_allocations = 0;
}

void* operator new(std::size_t n) {
    ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    ++g_allocations;
    const std::size_t align = static_cast<std::size_t>(a);
    // No std::aligned_alloc on Windows with ANY toolchain — the CRT does not
    // provide C11 aligned_alloc, so libstdc++ does not expose it under MinGW
    // either. Its aligned allocations must be freed with _aligned_free, so the
    // matching deletes below branch as well.
#ifdef _WIN32
    if (void* p = _aligned_malloc(((n + align - 1) / align) * align, align)) return p;
#else
    if (void* p = std::aligned_alloc(align, ((n + align - 1) / align) * align)) return p;
#endif
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }

void operator delete(void* p) noexcept                    { std::free(p); }
void operator delete[](void* p) noexcept                  { std::free(p); }
void operator delete(void* p, std::size_t) noexcept       { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept     { std::free(p); }
#ifdef _WIN32
void operator delete(void* p, std::align_val_t) noexcept  { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept   { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
#else
void operator delete(void* p, std::align_val_t) noexcept  { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept   { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

namespace {

std::uint64_t hashCanvas(const Document& doc) {
    const std::vector<StraightRgba8> px = flatten(doc);
    std::uint64_t h = 1469598103934665603ull;               // FNV-1a
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(px.data());
    for (std::size_t i = 0; i < px.size() * 4; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

InputSample at(double x, double y, float pressure = 1.0f) {
    InputSample s;
    s.x = x;
    s.y = y;
    s.pressure = pressure;
    s.fromMouse = true;
    return s;
}

double gap(const Dab& a, const Dab& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

std::filesystem::path scratchFile(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

}  // namespace

// ---------------------------------------------------------------- colour

TEST_CASE("premultiply round-trips exactly at full alpha") {
    // US-13.6: off by one here means every colour the artist picks is wrong.
    for (int r = 0; r < 256; ++r) {
        const StraightRgba8 c{static_cast<std::uint8_t>(r), 17, 200, 255};
        CHECK(c.premultiply().unpremultiply() == c);
    }
}

TEST_CASE("premultiply round-trips within a step at partial alpha") {
    for (int a = 1; a < 256; a += 7) {
        for (int v = 0; v < 256; v += 11) {
            const StraightRgba8 c{static_cast<std::uint8_t>(v), 0, 0,
                                  static_cast<std::uint8_t>(a)};
            const StraightRgba8 back = c.premultiply().unpremultiply();
            CHECK(back.a == c.a);
            // The 8-bit round trip is lossy at low alpha (D-004). Bound it, so
            // a save/load/save cycle cannot drift without the test noticing.
            const int tolerance = 1 + 255 / a;
            CHECK(std::abs(static_cast<int>(back.r) - v) <= tolerance);
        }
    }
}

TEST_CASE("a fully transparent premultiplied pixel carries no colour") {
    CHECK(PremulRgba8{0, 0, 0, 0}.unpremultiply() == StraightRgba8{0, 0, 0, 0});
}

TEST_CASE("over() is source-over on premultiplied values") {
    const PremulRgba8 opaque{255, 0, 0, 255};
    const PremulRgba8 white{255, 255, 255, 255};
    CHECK(over(opaque, white) == opaque);
    CHECK(over(PremulRgba8{0, 0, 0, 0}, white) == white);

    // 50% red over white: 128 + 255*(1-0.5) ~= 255 red, ~127 green and blue.
    const PremulRgba8 half{128, 0, 0, 128};
    const PremulRgba8 r = over(half, white);
    CHECK(r.a == 255);
    CHECK(r.r > 240);
    CHECK(r.g > 120);
    CHECK(r.g < 135);
}

// ------------------------------------------------------------------ tiles

TEST_CASE("tileIndex floors toward negative infinity") {
    // C++ integer division truncates toward zero, which is wrong to the left
    // of the origin — and tile coordinates go negative once the canvas can be
    // resized from the top or left.
    CHECK(tileIndex(0) == 0);
    CHECK(tileIndex(255) == 0);
    CHECK(tileIndex(256) == 1);
    CHECK(tileIndex(-1) == -1);
    CHECK(tileIndex(-256) == -1);
    CHECK(tileIndex(-257) == -2);
}

TEST_CASE("a cloned tile is an independent copy") {
    Tile a;
    a.fill(PremulRgba8{10, 20, 30, 40});
    Tile b = a.clone();
    CHECK(b.pixel(5, 5) == PremulRgba8{10, 20, 30, 40});
    b.setPixel(5, 5, PremulRgba8{});
    CHECK(a.pixel(5, 5) == PremulRgba8{10, 20, 30, 40});
}

// ------------------------------------------------------------ dab spacing

TEST_CASE("a click without movement paints exactly one dab") {
    // US-02.3: a click never does nothing.
    Stroke s;
    std::vector<Dab> dabs;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, 1);
    appendSample(s, at(10.0, 10.0), dabs);
    CHECK(dabs.size() == 1);
    CHECK(dabs[0].x == doctest::Approx(10.0));
}

TEST_CASE("dabs are evenly spaced across segment boundaries") {
    // US-03.4, and the single most likely thing to be wrong in the engine.
    // Large jumps, deliberately not multiples of the spacing, so a reset
    // leftoverDistance shows up as a short gap right after each sample.
    BrushPreset p = defaultPencil();
    p.size = 20.0f;
    p.spacingFactor = 0.25f;
    p.pressure = PressureMapping{};      // fixed radius, so spacing is constant
    p.pressure.toSize = false;

    const double spacing = spacingFor(p.size * 0.5f, p.spacingFactor);
    REQUIRE(spacing == doctest::Approx(5.0));

    Stroke s;
    std::vector<Dab> dabs;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, 1);

    const double xs[] = {0.0, 37.3, 51.9, 133.7, 134.1, 400.0};
    for (double x : xs) appendSample(s, at(x, 0.0), dabs);

    REQUIRE(dabs.size() > 60);
    for (std::size_t i = 1; i < dabs.size(); ++i) {
        // Never further apart than the spacing — that is the no-gap guarantee.
        CHECK(gap(dabs[i - 1], dabs[i]) <= spacing + 1e-6);
        // And never bunched, which is the same bug seen from the other side.
        CHECK(gap(dabs[i - 1], dabs[i]) >= spacing - 1e-6);
    }
    CHECK(dabs.back().x <= 400.0);
}

TEST_CASE("a fast flick across the canvas leaves no gap") {
    // US-03.1: two samples, 1024 px apart, is what a 200 ms flick looks like.
    BrushPreset p = defaultPencil();
    p.pressure.toSize = false;
    const double spacing = spacingFor(p.size * 0.5f, p.spacingFactor);

    Stroke s;
    std::vector<Dab> dabs;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, 1);
    appendSample(s, at(0.0, 0.0), dabs);
    appendSample(s, at(1024.0, 768.0), dabs);

    REQUIRE(dabs.size() > 1000);
    for (std::size_t i = 1; i < dabs.size(); ++i)
        CHECK(gap(dabs[i - 1], dabs[i]) <= spacing + 1e-6);
}

TEST_CASE("spacing never collapses to zero on a tiny brush") {
    // An infinite loop in the interpolator is the worst possible failure here.
    CHECK(spacingFor(0.05f, 0.001f) >= 1.0);

    BrushPreset p = defaultPencil();
    p.size = 0.2f;
    p.spacingFactor = 0.001f;
    Stroke s;
    std::vector<Dab> dabs;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, 1);
    appendSample(s, at(0.0, 0.0), dabs);
    appendSample(s, at(50.0, 0.0), dabs);
    CHECK(dabs.size() <= 60);
}

// ------------------------------------------------------------- dab -> tiles

TEST_CASE("painting allocates only the tiles it touches") {
    Document doc = makeDocument(1024, 1024, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};

    paintSample(s, target, at(10.0, 10.0), scratch);
    CHECK(layer.tiles.size() == 1);
    CHECK(s.touched.size() == 1);

    // A 4000 px canvas is 256 tiles per layer; working in one corner must
    // allocate a handful (D-005).
    paintSample(s, target, at(300.0, 10.0), scratch);
    CHECK(layer.tiles.size() == 2);
}

TEST_CASE("a dab dragged off the canvas edge paints only the on-canvas part") {
    // US-02.4: no crash, no wrap-around.
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};

    paintSample(s, target, at(-500.0, -500.0), scratch);
    CHECK(layer.tiles.empty());

    paintSample(s, target, at(2.0, 2.0), scratch);
    REQUIRE(layer.tiles.size() == 1);
    const Tile* tile = layer.find(TileKey{0, 0});
    REQUIRE(tile != nullptr);
    CHECK(tile->pixel(2, 2).a > 0);
    // Nothing painted beyond the canvas bound.
    CHECK(tile->pixel(63, 63).a == 0);
}

TEST_CASE("a solid dab reproduces the chosen colour exactly at its centre") {
    // US-13.6. Anything else means every colour the artist picks is subtly off.
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();

    BrushPreset p = defaultPencil();
    p.size = 20.0f;
    p.hardness = 1.0f;
    p.pressure = PressureMapping{};
    p.pressure.toSize = false;

    const StraightRgba8 chosen{37, 142, 211, 255};
    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, p, chosen, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, target, at(32.0, 32.0), scratch);

    const Tile* tile = layer.find(TileKey{0, 0});
    REQUIRE(tile != nullptr);
    CHECK(tile->pixel(32, 32).unpremultiply() == chosen);
}

TEST_CASE("a soft stroke on a transparent canvas has no dark halo") {
    // US-07.3. A grey fringe here is the premultiplied/straight mix-up the
    // type split exists to prevent.
    Document doc = makeDocument(64, 64, StraightRgba8{0, 0, 0, 0});
    Layer& layer = *doc.active();

    BrushPreset p = defaultPencil();
    p.size = 24.0f;
    p.hardness = 0.0f;                  // maximum falloff, worst case for haloes
    p.pressure = PressureMapping{};
    p.pressure.toSize = false;

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, p, StraightRgba8{255, 255, 255, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, target, at(32.0, 32.0), scratch);

    const std::vector<StraightRgba8> px = flatten(doc);
    bool sawEdge = false;
    for (const StraightRgba8 c : px) {
        if (c.a == 0) continue;
        if (c.a < 250) sawEdge = true;
        // White paint must stay white at every alpha. Anything darker is the
        // halo: it means straight-alpha values were composited premultiplied.
        CHECK(c.r >= 254);
        CHECK(c.g >= 254);
        CHECK(c.b >= 254);
    }
    CHECK(sawEdge);
}

TEST_CASE("an opaque background exports opaque") {
    Document doc = makeDocument(8, 8, StraightRgba8{255, 255, 255, 255});
    for (const StraightRgba8 c : flatten(doc)) CHECK(c == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("the dab path allocates nothing once the tiles exist") {
    // US-02.9. Allocation on the hot path is what turns a smooth stroke into
    // a stuttering one on the modest hardware this is aimed at.
    //
    // Tile creation and the first-touch undo snapshot DO allocate — that is
    // D-006 working. What must not allocate is every dab after them.
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();

    Stroke s;
    std::vector<Dab> scratch;
    scratch.reserve(256);
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};

    // Warm up: allocate the tile, take its snapshot, grow the scratch vector.
    for (int i = 0; i < 8; ++i)
        paintSample(s, target, at(100.0 + i, 100.0), scratch);
    REQUIRE(layer.tiles.size() == 1);

    const std::size_t before = g_allocations;
    for (int i = 0; i < 100; ++i)
        paintSample(s, target, at(100.0 + i * 0.5, 110.0 + i * 0.3), scratch);
    const std::size_t after = g_allocations;

    CHECK(after == before);
    CHECK(layer.tiles.size() == 1);          // still inside the warmed tile
    CHECK(s.samples.size() < s.samples.capacity());   // reserved at pen-down
}

// -------------------------------------------------------------------- undo

TEST_CASE("undo then redo restores the canvas byte for byte") {
    // US-04.7: draw -> hash -> undo -> redo -> hash; hashes match.
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    const std::uint64_t blank = hashCanvas(doc);

    Stroke s;
    std::vector<Dab> scratch;
    Layer& layer = *doc.active();
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, target, at(20.0, 20.0), scratch);
    paintSample(s, target, at(200.0, 180.0), scratch);
    doc.undo.push(std::move(s.pending));

    const std::uint64_t drawn = hashCanvas(doc);
    CHECK(drawn != blank);

    doc.undo.undo(doc);
    CHECK(hashCanvas(doc) == blank);

    doc.undo.redo(doc);
    CHECK(hashCanvas(doc) == drawn);
}

TEST_CASE("undoing the first stroke on a tile erases the tile") {
    // US-04.8: it must be REMOVED, not zeroed, or the sparse map stops being
    // sparse and every empty tile still costs 256 KiB.
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();
    const std::size_t before = layer.tiles.size();
    REQUIRE(before == 0);

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, target, at(20.0, 20.0), scratch);
    doc.undo.push(std::move(s.pending));
    REQUIRE(doc.active()->tiles.size() == 1);

    const auto changed = doc.undo.undo(doc);
    CHECK(doc.active()->tiles.size() == before);
    // The caller is told which textures to release.
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].second == TileKey{0, 0});
}

TEST_CASE("one stroke is exactly one undo step however many tiles it touched") {
    // US-04.2.
    Document doc = makeDocument(1024, 1024, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
    for (double x = 10.0; x < 1000.0; x += 37.0) paintSample(s, target, at(x, x), scratch);

    CHECK(s.pending.tiles.size() > 4);          // many tiles...
    doc.undo.push(std::move(s.pending));
    CHECK(doc.undo.size() == 1);                // ...one step

    doc.undo.undo(doc);
    CHECK(doc.active()->tiles.empty());
    CHECK(!doc.undo.canUndo());
}

TEST_CASE("undo past the beginning is a no-op, not a crash") {
    // US-04.4.
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    const std::uint64_t blank = hashCanvas(doc);
    for (int i = 0; i < 5; ++i) doc.undo.undo(doc);
    CHECK(hashCanvas(doc) == blank);
    CHECK(!doc.undo.canUndo());
    CHECK(!doc.undo.canRedo());
}

TEST_CASE("drawing after undoing discards the redo stack") {
    // US-04.5.
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    std::vector<Dab> scratch;

    const auto strokeAt = [&](double x) {
        Stroke s;
        Layer& layer = *doc.active();
        beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
        PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
        paintSample(s, target, at(x, 32.0), scratch);
        doc.undo.push(std::move(s.pending));
    };

    strokeAt(10.0);
    doc.undo.undo(doc);
    CHECK(doc.undo.canRedo());
    strokeAt(40.0);
    CHECK(!doc.undo.canRedo());
}

TEST_CASE("at least 50 consecutive undo steps are available") {
    // US-04.3.
    Document doc = makeDocument(512, 512, StraightRgba8{255, 255, 255, 255});
    std::vector<Dab> scratch;
    for (int i = 0; i < 60; ++i) {
        Stroke s;
        Layer& layer = *doc.active();
        beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
        PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
        paintSample(s, target, at(10.0 + i * 3.0, 20.0), scratch);
        doc.undo.push(std::move(s.pending));
    }
    CHECK(doc.undo.size() == 60);
    for (int i = 0; i < 60; ++i) doc.undo.undo(doc);
    CHECK(doc.active()->tiles.empty());
}

TEST_CASE("a stroke that painted nothing costs no undo step") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    Stroke s;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, doc.activeLayer);
    doc.undo.push(std::move(s.pending));
    CHECK(!doc.undo.canUndo());
}

TEST_CASE("clear is a single undoable action") {
    // US-06.2, US-06.3.
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    const std::uint64_t blank = hashCanvas(doc);

    Stroke s;
    std::vector<Dab> scratch;
    Layer& layer = *doc.active();
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, target, at(30.0, 30.0), scratch);
    doc.undo.push(std::move(s.pending));
    const std::uint64_t drawn = hashCanvas(doc);

    doc.undo.push(clearLayer(*doc.active()));
    CHECK(hashCanvas(doc) == blank);
    doc.undo.undo(doc);
    CHECK(hashCanvas(doc) == drawn);

    // Clearing an already empty canvas is harmless.
    doc.undo.push(clearLayer(*doc.active()));
    doc.undo.push(clearLayer(*doc.active()));
    CHECK(hashCanvas(doc) == blank);
}

// -------------------------------------------------- blend modes (M3)

namespace {

/// Blend two opaque colours and read the result back as straight alpha.
StraightRgba8 blendOpaque(BlendMode mode, StraightRgba8 src, StraightRgba8 dst) {
    return blendOver(mode, src.premultiply(), dst.premultiply()).unpremultiply();
}

}  // namespace

TEST_CASE("blend mode names round-trip, and unknown names degrade to normal") {
    for (const BlendMode mode : ALL_BLEND_MODES)
        CHECK(blendModeFromName(blendModeName(mode)) == mode);

    // Distinct names, or two modes collapse into one on load.
    std::vector<std::string_view> names;
    for (const BlendMode mode : ALL_BLEND_MODES) names.push_back(blendModeName(mode));
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());

    // A file written by a later version must open, not fail (D-011). "hue" is
    // one of the non-separable HSL modes Sable does not have.
    CHECK(blendModeFromName("hue") == BlendMode::Normal);
    CHECK(blendModeFromName("luminosity") == BlendMode::Normal);
    CHECK(blendModeFromName("") == BlendMode::Normal);
}

TEST_CASE("normal blending is exactly source-over") {
    // The fast path must not disagree with the general one.
    const PremulRgba8 src{60, 20, 10, 128};
    const PremulRgba8 dst{200, 200, 200, 255};
    CHECK(blendOver(BlendMode::Normal, src, dst) == over(src, dst));
}

TEST_CASE("multiply darkens and screen lightens") {
    const StraightRgba8 grey{128, 128, 128, 255};
    const StraightRgba8 mid{128, 128, 128, 255};

    const StraightRgba8 multiplied = blendOpaque(BlendMode::Multiply, grey, mid);
    const StraightRgba8 screened   = blendOpaque(BlendMode::Screen, grey, mid);
    CHECK(multiplied.r < mid.r);
    CHECK(screened.r > mid.r);
    // 0.5 * 0.5 = 0.25, and 0.5 + 0.5 - 0.25 = 0.75.
    CHECK(multiplied.r == doctest::Approx(64).epsilon(0.03));
    CHECK(screened.r   == doctest::Approx(191).epsilon(0.03));
}

TEST_CASE("multiplying by white and screening by black change nothing") {
    // The identity elements. Getting these wrong is the usual sign the
    // unpremultiply step is in the wrong place.
    const StraightRgba8 base{37, 142, 211, 255};
    CHECK(blendOpaque(BlendMode::Multiply, StraightRgba8{255, 255, 255, 255}, base) == base);
    CHECK(blendOpaque(BlendMode::Screen, StraightRgba8{0, 0, 0, 255}, base) == base);
    CHECK(blendOpaque(BlendMode::Add, StraightRgba8{0, 0, 0, 255}, base) == base);
}

TEST_CASE("add saturates rather than wrapping") {
    // Wrapping here turns a bright highlight into a black hole.
    const StraightRgba8 bright{200, 200, 200, 255};
    const StraightRgba8 result = blendOpaque(BlendMode::Add, bright, bright);
    CHECK(result.r == 255);
    CHECK(result.g == 255);
    CHECK(result.b == 255);
}

TEST_CASE("overlay keeps black black and white white") {
    const StraightRgba8 mid{128, 128, 128, 255};
    const StraightRgba8 onBlack =
        blendOpaque(BlendMode::Overlay, mid, StraightRgba8{0, 0, 0, 255});
    const StraightRgba8 onWhite =
        blendOpaque(BlendMode::Overlay, mid, StraightRgba8{255, 255, 255, 255});
    CHECK(onBlack.r == 0);
    CHECK(onWhite.r == 255);
}

TEST_CASE("darken and lighten pick a channel, and are idempotent") {
    const StraightRgba8 src{200, 50, 128, 255};
    const StraightRgba8 dst{100, 150, 128, 255};
    const StraightRgba8 darker  = blendOpaque(BlendMode::Darken, src, dst);
    const StraightRgba8 lighter = blendOpaque(BlendMode::Lighten, src, dst);
    CHECK(darker  == StraightRgba8{100, 50, 128, 255});
    CHECK(lighter == StraightRgba8{200, 150, 128, 255});

    // Identity elements: darkening with white and lightening with black.
    CHECK(blendOpaque(BlendMode::Darken, StraightRgba8{255, 255, 255, 255}, dst) == dst);
    CHECK(blendOpaque(BlendMode::Lighten, StraightRgba8{0, 0, 0, 255}, dst) == dst);
}

TEST_CASE("dodge brightens and burn darkens, without dividing by zero") {
    const StraightRgba8 base{128, 128, 128, 255};
    CHECK(blendOpaque(BlendMode::ColourDodge, StraightRgba8{64, 64, 64, 255}, base).r > base.r);
    CHECK(blendOpaque(BlendMode::ColourBurn, StraightRgba8{192, 192, 192, 255}, base).r < base.r);

    // Identity elements: dodging by black and burning by white.
    CHECK(blendOpaque(BlendMode::ColourDodge, StraightRgba8{0, 0, 0, 255}, base) == base);
    CHECK(blendOpaque(BlendMode::ColourBurn, StraightRgba8{255, 255, 255, 255}, base) == base);

    // The degenerate cases. White dodge over anything but black saturates;
    // black burn over anything but white goes to black; and a black backdrop
    // stays black under any dodge, which is where a naive cb/(1-cs) produces
    // a NaN and then a garbage pixel.
    const StraightRgba8 white{255, 255, 255, 255};
    const StraightRgba8 black{0, 0, 0, 255};
    CHECK(blendOpaque(BlendMode::ColourDodge, white, base).r == 255);
    CHECK(blendOpaque(BlendMode::ColourDodge, white, black).r == 0);
    CHECK(blendOpaque(BlendMode::ColourBurn, black, base).r == 0);
    CHECK(blendOpaque(BlendMode::ColourBurn, black, white).r == 255);
}

TEST_CASE("hard light is overlay with the operands swapped") {
    // Overlay is defined as hard light applied the other way round, so this
    // catches the two drifting apart.
    const StraightRgba8 a{37, 142, 211, 255};
    const StraightRgba8 b{200, 90, 15, 255};
    CHECK(blendOpaque(BlendMode::HardLight, a, b) == blendOpaque(BlendMode::Overlay, b, a));

    // Mid grey is hard light's identity, as it is soft light's.
    const StraightRgba8 mid{128, 128, 128, 255};
    const StraightRgba8 unchanged = blendOpaque(BlendMode::HardLight, mid, b);
    CHECK(unchanged.r == doctest::Approx(b.r).epsilon(0.02));
}

TEST_CASE("soft light nudges towards the source without clipping") {
    const StraightRgba8 base{128, 128, 128, 255};
    const StraightRgba8 lit  = blendOpaque(BlendMode::SoftLight, StraightRgba8{255, 255, 255, 255}, base);
    const StraightRgba8 dark = blendOpaque(BlendMode::SoftLight, StraightRgba8{0, 0, 0, 255}, base);
    CHECK(lit.r  > base.r);
    CHECK(dark.r < base.r);
    // Softer than hard light: pure white must not blow mid grey to white.
    CHECK(lit.r < 255);
    CHECK(dark.r > 0);

    // Mid grey changes nothing, and the shadow branch below cb = 0.25 stays
    // continuous — a kink there is a visible band on a gradient.
    CHECK(blendOpaque(BlendMode::SoftLight, StraightRgba8{128, 128, 128, 255}, base) == base);
    const StraightRgba8 shadow{60, 60, 60, 255};
    const StraightRgba8 justAbove{66, 66, 66, 255};
    const int below = blendOpaque(BlendMode::SoftLight, StraightRgba8{255, 255, 255, 255}, shadow).r;
    const int above = blendOpaque(BlendMode::SoftLight, StraightRgba8{255, 255, 255, 255}, justAbove).r;
    CHECK(std::abs(above - below) < 16);
}

TEST_CASE("difference and exclusion are symmetric, and black is their identity") {
    const StraightRgba8 a{200, 50, 128, 255};
    const StraightRgba8 b{100, 150, 128, 255};
    CHECK(blendOpaque(BlendMode::Difference, a, b) == blendOpaque(BlendMode::Difference, b, a));
    CHECK(blendOpaque(BlendMode::Exclusion, a, b) == blendOpaque(BlendMode::Exclusion, b, a));

    const StraightRgba8 black{0, 0, 0, 255};
    CHECK(blendOpaque(BlendMode::Difference, black, a) == a);
    CHECK(blendOpaque(BlendMode::Exclusion, black, a) == a);

    // Difference with itself is black; exclusion of mid grey with itself is
    // mid grey, which is the one place the two visibly disagree.
    CHECK(blendOpaque(BlendMode::Difference, a, a) == black);
    const StraightRgba8 mid{128, 128, 128, 255};
    CHECK(blendOpaque(BlendMode::Exclusion, mid, mid).r == doctest::Approx(128).epsilon(0.02));
}

TEST_CASE("a transparent source leaves the backdrop untouched in every mode") {
    const PremulRgba8 clear{0, 0, 0, 0};
    const PremulRgba8 base{100, 50, 25, 255};
    for (const BlendMode mode : ALL_BLEND_MODES)
        CHECK(blendOver(mode, clear, base) == base);
}

TEST_CASE("blending never produces colour brighter than its own alpha") {
    // A premultiplied pixel with a channel above its alpha is invalid, and it
    // renders as a bright fringe. Sweep every mode for it — this is the check
    // that has to grow when a mode is added, so it reads ALL_BLEND_MODES.
    for (const BlendMode mode : ALL_BLEND_MODES) {
        for (int sa = 0; sa <= 255; sa += 17) {
            for (int da = 0; da <= 255; da += 17) {
                const auto src = StraightRgba8{255, 128, 0,
                                               static_cast<std::uint8_t>(sa)}.premultiply();
                const auto dst = StraightRgba8{0, 200, 255,
                                               static_cast<std::uint8_t>(da)}.premultiply();
                const PremulRgba8 out = blendOver(mode, src, dst);
                CHECK(out.r <= out.a);
                CHECK(out.g <= out.a);
                CHECK(out.b <= out.a);
            }
        }
    }
}

// ------------------------------------------------ layer operations (M3)

namespace {

/// Paints a solid square so a layer has something identifiable in it.
void paintSquare(Document& doc, LayerId id, StraightRgba8 colour, double x, double y) {
    Layer* layer = doc.layerById(id);
    REQUIRE(layer != nullptr);
    BrushPreset p = defaultPencil();
    p.size = 40.0f;
    p.hardness = 1.0f;
    p.pressure = PressureMapping{};
    p.pressure.toSize = false;

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, p, colour, id);
    PaintTarget target{*layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, target, at(x, y), scratch);
    doc.undo.push(std::move(s.pending));
}

}  // namespace

TEST_CASE("adding a layer is undoable") {
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    REQUIRE(doc.layers.size() == 1);

    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Ink"));
    CHECK(doc.layers.size() == 2);
    CHECK(doc.layers[1].name == "Ink");
    CHECK(doc.activeLayer == doc.layers[1].id);

    doc.undo.undo(doc);
    CHECK(doc.layers.size() == 1);
    doc.undo.redo(doc);
    CHECK(doc.layers.size() == 2);
    CHECK(doc.layers[1].name == "Ink");
}

TEST_CASE("deleting a layer restores its pixels on undo") {
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Ink"));
    const LayerId ink = doc.activeLayer;
    paintSquare(doc, ink, StraightRgba8{255, 0, 0, 255}, 60.0, 60.0);

    const std::uint64_t withInk = hashCanvas(doc);
    doc.undo.push(deleteLayer(doc, ink));
    CHECK(doc.layers.size() == 1);
    CHECK(hashCanvas(doc) != withInk);

    doc.undo.undo(doc);
    CHECK(doc.layers.size() == 2);
    CHECK(hashCanvas(doc) == withInk);      // the tiles came back, not just the layer
}

TEST_CASE("the last layer cannot be deleted") {
    // A document with nothing to paint on is a dead end with no obvious way out.
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    const UndoRecord rec = deleteLayer(doc, doc.activeLayer);
    CHECK(rec.empty());
    CHECK(doc.layers.size() == 1);
}

TEST_CASE("duplicating a layer copies its pixels, not its identity") {
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 255, 255}, 60.0, 60.0);
    const LayerId original = doc.activeLayer;

    doc.undo.push(duplicateLayer(doc, original));
    REQUIRE(doc.layers.size() == 2);
    const LayerId copy = doc.activeLayer;
    CHECK(copy != original);
    CHECK(doc.layerById(copy)->tiles.size() == doc.layerById(original)->tiles.size());

    // Painting the copy must not disturb the original. Tile counts cannot show
    // this — both squares land in the same tile — so compare the pixels.
    paintSquare(doc, copy, StraightRgba8{0, 255, 0, 255}, 200.0, 200.0);
    const Tile* originalTile = doc.layerById(original)->find(TileKey{0, 0});
    const Tile* copyTile     = doc.layerById(copy)->find(TileKey{0, 0});
    REQUIRE(originalTile != nullptr);
    REQUIRE(copyTile != nullptr);
    CHECK(copyTile->pixel(200, 200).a > 0);
    CHECK(originalTile->pixel(200, 200).a == 0);
    CHECK(originalTile->pixel(60, 60).a > 0);      // and it kept its own paint
}

TEST_CASE("moving a layer changes draw order and undoes cleanly") {
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
    const LayerId bottom = doc.activeLayer;
    doc.undo.push(addLayerAbove(doc, bottom, "Top"));
    const LayerId top = doc.activeLayer;

    // Overlapping squares, so which one is on top is visible in the flatten.
    paintSquare(doc, bottom, StraightRgba8{255, 0, 0, 255}, 64.0, 64.0);
    paintSquare(doc, top, StraightRgba8{0, 0, 255, 255}, 64.0, 64.0);

    CHECK(pickColour(doc, 64, 64) == StraightRgba8{0, 0, 255, 255});   // blue wins

    doc.undo.push(moveLayer(doc, top, -1));
    CHECK(doc.layers[0].id == top);
    CHECK(pickColour(doc, 64, 64) == StraightRgba8{255, 0, 0, 255});   // now red

    doc.undo.undo(doc);
    CHECK(doc.layers[1].id == top);
    CHECK(pickColour(doc, 64, 64) == StraightRgba8{0, 0, 255, 255});
}

TEST_CASE("moving past either end is a no-op") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Top"));
    CHECK(moveLayer(doc, doc.layers[0].id, -1).empty());
    CHECK(moveLayer(doc, doc.layers[1].id, +1).empty());
    CHECK(doc.layers.size() == 2);
}

TEST_CASE("merging down combines pixels and removes the upper layer") {
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    const LayerId bottom = doc.activeLayer;
    doc.undo.push(addLayerAbove(doc, bottom, "Top"));
    const LayerId top = doc.activeLayer;

    paintSquare(doc, bottom, StraightRgba8{255, 0, 0, 255}, 60.0, 60.0);
    paintSquare(doc, top, StraightRgba8{0, 0, 255, 255}, 180.0, 180.0);
    const std::uint64_t before = hashCanvas(doc);

    doc.undo.push(mergeLayerDown(doc, top));
    CHECK(doc.layers.size() == 1);
    // The visible result is unchanged — that is what "merge" has to mean.
    CHECK(hashCanvas(doc) == before);
    CHECK(pickColour(doc, 60, 60) == StraightRgba8{255, 0, 0, 255});
    CHECK(pickColour(doc, 180, 180) == StraightRgba8{0, 0, 255, 255});

    doc.undo.undo(doc);
    CHECK(doc.layers.size() == 2);
    CHECK(hashCanvas(doc) == before);
}

TEST_CASE("merging honours the upper layer's blend mode and opacity") {
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
    const LayerId bottom = doc.activeLayer;
    doc.undo.push(addLayerAbove(doc, bottom, "Shade"));
    const LayerId top = doc.activeLayer;

    paintSquare(doc, bottom, StraightRgba8{200, 200, 200, 255}, 64.0, 64.0);
    paintSquare(doc, top, StraightRgba8{128, 128, 128, 255}, 64.0, 64.0);
    doc.layerById(top)->blend = BlendMode::Multiply;

    const StraightRgba8 composited = pickColour(doc, 64, 64);
    doc.undo.push(mergeLayerDown(doc, top));
    // Merging must not change what the artist sees. If it does, the blend maths
    // used at merge time disagrees with the one used at composite time.
    CHECK(pickColour(doc, 64, 64) == composited);
}

TEST_CASE("merging the bottom layer is a no-op") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    CHECK(mergeLayerDown(doc, doc.activeLayer).empty());
    CHECK(doc.layers.size() == 1);
}

TEST_CASE("a property edit is undoable without snapshotting pixels") {
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}, 60.0, 60.0);
    const LayerId id = doc.activeLayer;

    LayerProps props = propsOf(*doc.layerById(id));
    props.opacity = 0.25f;
    props.blend   = BlendMode::Multiply;
    props.name    = "Shading";

    UndoRecord rec = setLayerProps(doc, id, props);
    // The whole point of the separate props delta: an opacity slider must not
    // cost 256 KiB per touched tile of history.
    CHECK(rec.memoryBytes() < static_cast<std::size_t>(TILE_BYTES));
    doc.undo.push(std::move(rec));

    CHECK(doc.layerById(id)->opacity == doctest::Approx(0.25f));
    CHECK(doc.layerById(id)->name == "Shading");

    doc.undo.undo(doc);
    CHECK(doc.layerById(id)->opacity == doctest::Approx(1.0f));
    CHECK(doc.layerById(id)->name == "Layer 1");
    CHECK(doc.layerById(id)->blend == BlendMode::Normal);
    CHECK(doc.layerById(id)->tiles.size() == 1);      // pixels never moved
}

TEST_CASE("a clipped layer shows only where its base has paint") {
    Document doc = makeDocument(200, 100, StraightRgba8{255, 255, 255, 255});
    const LayerId base = doc.activeLayer;
    paintSquare(doc, base, StraightRgba8{255, 255, 255, 255}, 50.0, 50.0);

    doc.undo.push(addLayerAbove(doc, base, "Shade"));
    const LayerId shade = doc.activeLayer;
    doc.layerById(shade)->clipToBelow = true;

    // Paint the clipped layer right across the canvas, well past the base.
    {
        Layer* layer = doc.layerById(shade);
        BrushPreset p = defaultPencil();
        p.size = 60.0f;
        p.hardness = 1.0f;
        p.pressure = PressureMapping{};
        p.pressure.toSize = false;
        Stroke s;
        std::vector<Dab> scratch;
        beginStroke(s, p, StraightRgba8{255, 0, 0, 255}, shade);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
        for (double x = 0.0; x <= 200.0; x += 4.0) paintSample(s, t, at(x, 50.0), scratch);
        doc.undo.push(std::move(s.pending));
    }

    // Red only where the base square is...
    CHECK(pickColour(doc, 50, 50) == StraightRgba8{255, 0, 0, 255});
    // ...and nowhere else, even though paint exists there on the clipped layer.
    CHECK(pickColour(doc, 160, 50) == StraightRgba8{255, 255, 255, 255});
    REQUIRE(doc.layerById(shade)->find(TileKey{0, 0}) != nullptr);

    // Turning clipping off reveals the rest of it — proving the paint was
    // there all along and only the compositing was masking it.
    doc.layerById(shade)->clipToBelow = false;
    CHECK(pickColour(doc, 160, 50) == StraightRgba8{255, 0, 0, 255});
}

TEST_CASE("pickColour and flatten agree pixel for pixel") {
    // Alt+click reads through pickColour; the canvas is drawn through flatten.
    // If they diverge the artist samples a colour that is not on screen.
    Document doc = makeDocument(80, 60, StraightRgba8{200, 210, 220, 255});
    const LayerId base = doc.activeLayer;
    paintSquare(doc, base, StraightRgba8{255, 0, 0, 255}, 30.0, 30.0);

    doc.undo.push(addLayerAbove(doc, base, "Multiply"));
    const LayerId top = doc.activeLayer;
    paintSquare(doc, top, StraightRgba8{0, 0, 255, 255}, 45.0, 30.0);
    doc.layerById(top)->blend   = BlendMode::Multiply;
    doc.layerById(top)->opacity = 0.55f;

    doc.undo.push(addLayerAbove(doc, top, "Clipped"));
    const LayerId clip = doc.activeLayer;
    paintSquare(doc, clip, StraightRgba8{0, 255, 0, 255}, 40.0, 30.0);
    doc.layerById(clip)->clipToBelow = true;

    const std::vector<StraightRgba8> full = flatten(doc);
    for (std::int32_t y = 0; y < doc.height; ++y)
        for (std::int32_t x = 0; x < doc.width; ++x)
            REQUIRE(pickColour(doc, x, y) ==
                    full[static_cast<std::size_t>(y) * doc.width + x]);
}

TEST_CASE("the canvas display path agrees with flatten pixel for pixel") {
    // The canvas view composites one tile at a time through compositeRect and
    // blits the result; the export goes through flatten. v1.0.0 shipped with
    // the renderer applying nothing but layer opacity, so a Multiply layer
    // drew as Normal and exported as Multiply (#1). Any inequality below is
    // that bug returning — including an offset bug that only shows up on the
    // tiles that are not the first one.
    Document doc = makeDocument(300, 300, StraightRgba8{200, 210, 220, 160});
    const LayerId base = doc.activeLayer;
    paintSquare(doc, base, StraightRgba8{255, 0, 0, 255}, 40.0, 40.0);
    paintSquare(doc, base, StraightRgba8{255, 0, 0, 255}, 256.0, 256.0);   // over a tile seam

    doc.undo.push(addLayerAbove(doc, base, "Multiply"));
    const LayerId mul = doc.activeLayer;
    paintSquare(doc, mul, StraightRgba8{0, 0, 255, 255}, 60.0, 40.0);
    paintSquare(doc, mul, StraightRgba8{0, 0, 255, 255}, 250.0, 256.0);
    doc.layerById(mul)->blend   = BlendMode::Multiply;
    doc.layerById(mul)->opacity = 0.55f;

    doc.undo.push(addLayerAbove(doc, mul, "Clipped"));
    const LayerId clip = doc.activeLayer;
    paintSquare(doc, clip, StraightRgba8{0, 255, 0, 255}, 70.0, 40.0);
    paintSquare(doc, clip, StraightRgba8{0, 255, 0, 255}, 256.0, 250.0);
    doc.layerById(clip)->clipToBelow = true;

    doc.undo.push(addLayerAbove(doc, clip, "Group"));
    const LayerId group = doc.activeLayer;
    doc.layerById(group)->kind    = LayerKind::Folder;
    doc.layerById(group)->opacity = 0.6f;
    doc.undo.push(addLayerAbove(doc, group, "In group, lower"));
    doc.layerById(doc.activeLayer)->parent = group;
    paintSquare(doc, doc.activeLayer, StraightRgba8{255, 200, 0, 255}, 140.0, 140.0);
    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "In group, upper"));
    doc.layerById(doc.activeLayer)->parent = group;
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 200, 255, 255}, 150.0, 150.0);

    doc.undo.push(addLayerAbove(doc, group, "Hidden"));
    const LayerId hidden = doc.activeLayer;
    doc.layerById(hidden)->visible = false;
    for (double y = 20.0; y < 300.0; y += 20.0)
        paintSquare(doc, hidden, StraightRgba8{0, 0, 0, 255}, 150.0, y);

    const std::vector<StraightRgba8> exported = flatten(doc);
    REQUIRE(exported.size() == static_cast<std::size_t>(doc.width) * doc.height);

    // The document has to actually exercise the rules, or equality is vacuous.
    doc.layerById(mul)->blend = BlendMode::Normal;
    REQUIRE(flatten(doc) != exported);
    doc.layerById(mul)->blend = BlendMode::Multiply;

    // Rasterise the display path the way CanvasView does it: one composited
    // tile per texture, assembled into the canvas.
    std::vector<StraightRgba8> screen(exported.size());
    for (std::int32_t ty = 0; ty <= tileIndex(doc.height - 1); ++ty) {
        for (std::int32_t tx = 0; tx <= tileIndex(doc.width - 1); ++tx) {
            const std::vector<PremulRgba8> tile =
                compositeRect(doc, tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE);
            REQUIRE(tile.size() == static_cast<std::size_t>(TILE_PIXELS));
            for (std::int32_t y = 0; y < TILE_SIZE; ++y) {
                const std::int32_t cy = ty * TILE_SIZE + y;
                if (cy >= doc.height) break;
                for (std::int32_t x = 0; x < TILE_SIZE; ++x) {
                    const std::int32_t cx = tx * TILE_SIZE + x;
                    if (cx >= doc.width) break;
                    screen[static_cast<std::size_t>(cy) * doc.width + cx] =
                        tile[static_cast<std::size_t>(y) * TILE_SIZE + x].unpremultiply();
                }
            }
        }
    }

    for (std::int32_t y = 0; y < doc.height; ++y) {
        for (std::int32_t x = 0; x < doc.width; ++x) {
            const auto at = static_cast<std::size_t>(y) * doc.width + x;
            REQUIRE(screen[at] == exported[at]);      // screen == export (#1)
            REQUIRE(pickColour(doc, x, y) == exported[at]);   // and Alt+click == both
        }
    }
}

TEST_CASE("a folder's opacity applies to the group, not to each child") {
    // The whole reason groups exist. Two overlapping opaque children at 50%
    // group opacity must show the group half-transparent — NOT each child
    // half-transparent, which would let the lower one show through the upper.
    Document doc = makeDocument(120, 120, StraightRgba8{255, 255, 255, 255});
    const LayerId base = doc.activeLayer;

    doc.undo.push(addLayerAbove(doc, base, "Group"));
    const LayerId group = doc.activeLayer;
    doc.layerById(group)->kind = LayerKind::Folder;

    doc.undo.push(addLayerAbove(doc, group, "Lower"));
    const LayerId lower = doc.activeLayer;
    doc.layerById(lower)->parent = group;
    paintSquare(doc, lower, StraightRgba8{255, 0, 0, 255}, 60.0, 60.0);

    doc.undo.push(addLayerAbove(doc, lower, "Upper"));
    const LayerId upper = doc.activeLayer;
    doc.layerById(upper)->parent = group;
    paintSquare(doc, upper, StraightRgba8{0, 0, 255, 255}, 60.0, 60.0);

    // Fully opaque group: the upper child wins outright.
    CHECK(pickColour(doc, 60, 60) == StraightRgba8{0, 0, 255, 255});

    doc.layerById(group)->opacity = 0.5f;
    const StraightRgba8 half = pickColour(doc, 60, 60);
    // Blue blended with the white background, with NO red leaking through.
    CHECK(half.b > 200);
    CHECK(half.r == half.g);            // a clean blue-over-white, not blue-over-red
    CHECK(half.r > 100);
    CHECK(half.r < 160);
}

TEST_CASE("hiding a folder hides everything inside it") {
    Document doc = makeDocument(80, 80, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Group"));
    const LayerId group = doc.activeLayer;
    doc.layerById(group)->kind = LayerKind::Folder;

    doc.undo.push(addLayerAbove(doc, group, "Child"));
    doc.layerById(doc.activeLayer)->parent = group;
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);
    REQUIRE(pickColour(doc, 40, 40) == StraightRgba8{0, 0, 0, 255});

    doc.layerById(group)->visible = false;
    CHECK(pickColour(doc, 40, 40) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("a folder round-trips through .sable with its children") {
    Document doc = makeDocument(100, 100, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Group"));
    const LayerId group = doc.activeLayer;
    doc.layerById(group)->kind    = LayerKind::Folder;
    doc.layerById(group)->opacity = 0.7f;

    doc.undo.push(addLayerAbove(doc, group, "Child"));
    doc.layerById(doc.activeLayer)->parent = group;
    paintSquare(doc, doc.activeLayer, StraightRgba8{30, 60, 90, 255}, 50.0, 50.0);

    const std::uint64_t before = hashCanvas(doc);
    const auto path = scratchFile("sable_folders.sable");
    REQUIRE(saveProject(doc, path).has_value());

    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());
    const Layer* reloadedGroup = loaded->layerById(group);
    REQUIRE(reloadedGroup != nullptr);
    CHECK(reloadedGroup->kind == LayerKind::Folder);
    CHECK(hashCanvas(*loaded) == before);
    std::filesystem::remove(path);
}

TEST_CASE("a hidden layer contributes nothing") {
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}, 64.0, 64.0);
    const std::uint64_t visible = hashCanvas(doc);

    doc.layerById(doc.activeLayer)->visible = false;
    const std::uint64_t hidden = hashCanvas(doc);
    CHECK(hidden != visible);
    CHECK(pickColour(doc, 64, 64) == StraightRgba8{255, 255, 255, 255});
}

// ------------------------------------------------------- pressure (M2)

TEST_CASE("pressure normalisation runs deadzone, then rescale, then curve") {
    // US-09.7. Reversing the first and last steps produces a device-specific
    // bug that is close to undiagnosable from a user's description, so the
    // order gets an explicit test rather than a comment.
    TabletProfile profile;
    profile.rawMin = 0.5f;             // a hefty deadzone...
    profile.rawMax = 1.0f;
    profile.curve  = curveHard();      // ...and a distinctly non-linear curve

    PressureFilter filter;
    const float got = filter.apply(profile, 0.75f);

    // Correct order: 0.75 sits halfway through the live range, so the curve is
    // asked about 0.5.
    CHECK(filter.lastRescaled() == doctest::Approx(0.5f));
    CHECK(got == doctest::Approx(curveHard().eval(0.5f)));

    // Curve-then-deadzone would ask the curve about 0.75 and rescale after,
    // giving a visibly different answer. Prove the two differ, so the test
    // cannot pass by coincidence.
    const float wrongOrder = (curveHard().eval(0.75f) - 0.5f) / 0.5f;
    CHECK(std::abs(got - wrongOrder) > 0.05f);
}

TEST_CASE("the deadzone ignores faint contact and the clamp reaches full early") {
    // US-09.2 and US-09.3.
    TabletProfile profile;
    profile.rawMin = 0.1f;
    profile.rawMax = 0.8f;
    PressureFilter filter;

    CHECK(filter.apply(profile, 0.0f) == doctest::Approx(0.0f));
    CHECK(filter.apply(profile, 0.05f) == doctest::Approx(0.0f));   // below deadzone
    CHECK(filter.apply(profile, 0.8f) == doctest::Approx(1.0f));    // at the clamp
    CHECK(filter.apply(profile, 1.0f) == doctest::Approx(1.0f));    // beyond it
}

TEST_CASE("pressure curves are monotonic and span the full range") {
    // A curve that dips means pressing harder makes a lighter mark. Monotone
    // interpolation exists to prevent exactly that, so verify it holds rather
    // than trusting the algorithm's name.
    for (const PressureCurve& curve : {curveLinear(), curveSoft(), curveHard()}) {
        CHECK(curve.eval(0.0f) == doctest::Approx(0.0f));
        CHECK(curve.eval(1.0f) == doctest::Approx(1.0f));

        float previous = -1.0f;
        for (int i = 0; i <= 200; ++i) {
            const float value = curve.eval(static_cast<float>(i) / 200.0f);
            CHECK(value >= previous - 1e-5f);      // never dips
            CHECK(value >= 0.0f);
            CHECK(value <= 1.0f);                  // never overshoots
            previous = value;
        }
    }
}

TEST_CASE("soft and hard curves sit either side of linear") {
    CHECK(curveSoft().eval(0.3f) > curveLinear().eval(0.3f));
    CHECK(curveHard().eval(0.3f) < curveLinear().eval(0.3f));
}

TEST_CASE("smoothing lags a noisy signal without freezing it") {
    // US-09.4. Smoothing must settle on the true value, not stall short of it.
    TabletProfile profile;
    profile.smoothing = 0.9f;
    PressureFilter filter;

    (void)filter.apply(profile, 0.0f);
    const float firstStep = filter.apply(profile, 1.0f);
    CHECK(firstStep < 0.9f);                       // it lags...

    float value = firstStep;
    for (int i = 0; i < 400; ++i) value = filter.apply(profile, 1.0f);
    CHECK(value == doctest::Approx(1.0f).epsilon(0.01));   // ...but it arrives
}

TEST_CASE("pressure drives size and density independently") {
    // US-08.4: each mapping switches off on its own.
    BrushPreset p = defaultPencil();
    p.size = 20.0f;
    p.minSizeRatio = 0.0f;
    p.minDensityRatio = 0.0f;

    Stroke s;
    std::vector<Dab> dabs;

    p.pressure = PressureMapping{.toSize = true, .toDensity = false};
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, 1);
    appendSample(s, at(0.0, 0.0, 0.5f), dabs);
    CHECK(dabs[0].radius == doctest::Approx(5.0f));       // half of half of 20
    CHECK(dabs[0].density == doctest::Approx(1.0f));      // untouched

    dabs.clear();
    p.pressure = PressureMapping{.toSize = false, .toDensity = true};
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, 1);
    appendSample(s, at(0.0, 0.0, 0.5f), dabs);
    CHECK(dabs[0].radius == doctest::Approx(10.0f));      // untouched
    CHECK(dabs[0].density == doctest::Approx(0.5f));
}

TEST_CASE("a stroke that presses and lifts produces a tapered mark") {
    // US-08.5.
    BrushPreset p = defaultPencil();
    p.size = 30.0f;
    p.minSizeRatio = 0.1f;

    Stroke s;
    std::vector<Dab> dabs;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, 1);
    appendSample(s, at(0.0, 0.0, 0.05f), dabs);
    const float startRadius = dabs.back().radius;
    appendSample(s, at(100.0, 0.0, 1.0f), dabs);
    const float peakRadius = dabs.back().radius;
    appendSample(s, at(200.0, 0.0, 0.05f), dabs);
    const float endRadius = dabs.back().radius;

    CHECK(peakRadius > startRadius * 3.0f);
    CHECK(endRadius < peakRadius * 0.5f);
}

TEST_CASE("mouse input still paints at full pressure") {
    // US-08.7: the application never requires a tablet, and the mouse path is
    // never a special case that can rot.
    const InputSample mouse = at(10.0, 10.0);
    CHECK(mouse.fromMouse);
    CHECK(mouse.pressure == doctest::Approx(1.0f));

    BrushPreset p = defaultPencil();
    Stroke s;
    std::vector<Dab> dabs;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, 1);
    appendSample(s, mouse, dabs);
    CHECK(dabs[0].radius == doctest::Approx(p.size * 0.5f));
}

// ----------------------------------------------------------- stabilizer (M2)

TEST_CASE("stabilizer off is a pure pass-through") {
    // US-11.2: no smoothing, and nothing that could add latency.
    Stabilizer stabilizer;
    stabilizer.setLevel(0);
    for (int i = 0; i < 10; ++i) {
        const InputSample in = at(i * 7.0, i * 3.0, 0.5f);
        const InputSample out = stabilizer.apply(in);
        CHECK(out.x == doctest::Approx(in.x));
        CHECK(out.y == doctest::Approx(in.y));
    }
}

TEST_CASE("higher stabilizer levels reduce wobble more") {
    // US-11.3. A zig-zag stands in for a shaky hand; measure how far the
    // output still deviates from the straight line the artist meant to draw.
    const auto wobbleFor = [](std::uint8_t level) {
        Stabilizer stabilizer;
        stabilizer.setLevel(level);
        double worst = 0.0;
        for (int i = 0; i < 2000; ++i) {
            const double x = i * 2.0;
            const double y = (i % 2 == 0) ? 6.0 : -6.0;    // +/- 6 px of shake
            const InputSample out = stabilizer.apply(at(x, y));
            // Steady state only. A longer string takes longer to converge from
            // the stroke's first sample, and measuring during that transient
            // says nothing about how much shake it removes.
            if (i > 1000) worst = std::max(worst, std::abs(out.y));
        }
        return worst;
    };

    const double off    = wobbleFor(0);
    const double low    = wobbleFor(1);
    const double medium = wobbleFor(2);
    const double high   = wobbleFor(3);

    CHECK(off == doctest::Approx(6.0));
    CHECK(low < off);
    CHECK(medium < low);
    CHECK(high < medium);
}

TEST_CASE("a stabilized stroke still ends where the pen lifted") {
    // US-11.4. A smoothed line that stops short of the release point is the
    // classic pulled-string bug, and it is very visible on short strokes.
    Stabilizer stabilizer;
    stabilizer.setLevel(3);

    InputSample last{};
    for (int i = 0; i < 20; ++i) {
        last = at(i * 10.0, 0.0);
        (void)stabilizer.apply(last);
    }
    const InputSample end = stabilizer.finish(last);
    CHECK(end.x == doctest::Approx(last.x));
    CHECK(end.y == doctest::Approx(last.y));
}

TEST_CASE("the stabilizer leaves pressure alone") {
    // US-11.6: positions only.
    Stabilizer stabilizer;
    stabilizer.setLevel(3);
    (void)stabilizer.apply(at(0.0, 0.0, 0.2f));
    const InputSample out = stabilizer.apply(at(100.0, 0.0, 0.77f));
    CHECK(out.pressure == doctest::Approx(0.77f));
    CHECK(out.x < 100.0);                      // position did lag
}

TEST_CASE("the stabilizer holds a sharp corner") {
    // US-03.3 read through the stabilizer: a moving average rounds a V off,
    // which is why D-103 picked pulled-string instead.
    Stabilizer stabilizer;
    stabilizer.setLevel(2);
    for (int i = 0; i <= 30; ++i) (void)stabilizer.apply(at(i * 5.0, 0.0));
    InputSample out{};
    for (int i = 1; i <= 30; ++i) out = stabilizer.apply(at(150.0, i * 5.0));
    // After travelling far down the second leg, the brush is on that leg —
    // not still cutting the corner diagonally.
    CHECK(out.x == doctest::Approx(150.0).epsilon(0.01));
    CHECK(out.y > 100.0);
}

// --------------------------------------------------------------- rulers (M5)

TEST_CASE("a symmetry ruler that is off produces the sample and nothing else") {
    SymmetryRuler ruler;
    ruler.vertical = true;                 // configured, but not switched on
    std::vector<SymmetryImage> images;
    ruler.map(30.0, 40.0, 0.0f, images);
    REQUIRE(images.size() == 1);
    CHECK(images[0].x == doctest::Approx(30.0));
    CHECK(images[0].y == doctest::Approx(40.0));
}

TEST_CASE("a vertical axis mirrors x about the centre and leaves y alone") {
    SymmetryRuler ruler;
    ruler.enabled = true;
    ruler.vertical = true;
    ruler.centreX = 100.0;
    ruler.centreY = 50.0;

    std::vector<SymmetryImage> images;
    ruler.map(130.0, 20.0, 0.0f, images);
    REQUIRE(images.size() == 2);
    // The original comes first, so a caller can skip what it has already painted.
    CHECK(images[0].x == doctest::Approx(130.0));
    CHECK(images[0].y == doctest::Approx(20.0));
    CHECK(images[1].x == doctest::Approx(70.0));
    CHECK(images[1].y == doctest::Approx(20.0));

    ruler.vertical = false;
    ruler.horizontal = true;
    ruler.map(130.0, 20.0, 0.0f, images);
    REQUIRE(images.size() == 2);
    CHECK(images[1].x == doctest::Approx(130.0));
    CHECK(images[1].y == doctest::Approx(80.0));

    ruler.vertical = true;                 // both axes: four quadrants
    ruler.map(130.0, 20.0, 0.0f, images);
    CHECK(images.size() == 4);
}

TEST_CASE("radial symmetry spaces its copies evenly at one radius") {
    SymmetryRuler ruler;
    ruler.enabled = true;
    ruler.radial  = 6;
    ruler.centreX = 200.0;
    ruler.centreY = 200.0;

    std::vector<SymmetryImage> images;
    ruler.map(240.0, 200.0, 0.0f, images);        // 40 px out along +x
    REQUIRE(images.size() == 6);
    for (const SymmetryImage& image : images) {
        const double dx = image.x - 200.0, dy = image.y - 200.0;
        CHECK(std::sqrt(dx * dx + dy * dy) == doctest::Approx(40.0));
    }

    // No two copies land on the same spot. A doubled dab is not a subtle
    // failure — it paints twice and shows as a dark blotch on the seam.
    for (std::size_t i = 0; i < images.size(); ++i)
        for (std::size_t j = i + 1; j < images.size(); ++j)
            CHECK(std::hypot(images[i].x - images[j].x,
                             images[i].y - images[j].y) > 1.0);
}

TEST_CASE("radial symmetry with an axis is dihedral, and still has no duplicates") {
    SymmetryRuler ruler;
    ruler.enabled  = true;
    ruler.radial   = 4;            // even: the case where a naive transform
    ruler.vertical = true;         // list produces the half turn twice
    ruler.centreX = 0.0;
    ruler.centreY = 0.0;

    std::vector<SymmetryImage> images;
    ruler.map(10.0, 3.0, 0.0f, images);
    REQUIRE(images.size() == 8);
    for (std::size_t i = 0; i < images.size(); ++i)
        for (std::size_t j = i + 1; j < images.size(); ++j)
            CHECK(std::hypot(images[i].x - images[j].x,
                             images[i].y - images[j].y) > 1e-6);
}

TEST_CASE("radial copies are capped rather than trusted") {
    SymmetryRuler ruler;
    ruler.enabled = true;
    ruler.radial  = 100000;        // a slider dragged, or a settings file edited
    std::vector<SymmetryImage> images;
    ruler.map(10.0, 0.0, 0.0f, images);
    CHECK(images.size() == static_cast<std::size_t>(SymmetryRuler::kMaxRadial));
}

TEST_CASE("a perspective ruler with no usable point changes nothing") {
    PerspectiveRuler ruler;
    ruler.points.push_back(VanishingPoint{500.0, 100.0, false});   // disabled
    ruler.enabled = true;
    CHECK(!ruler.usable());
    for (int i = 0; i < 20; ++i) {
        const InputSample in = at(i * 9.0, i * 4.0);
        const InputSample out = ruler.apply(in);
        CHECK(out.x == doctest::Approx(in.x));
        CHECK(out.y == doctest::Approx(in.y));
    }
}

TEST_CASE("a perspective stroke lands on the line through its start and the point") {
    PerspectiveRuler ruler;
    ruler.enabled = true;
    ruler.points.push_back(VanishingPoint{400.0, 0.0, true});

    const double startX = 0.0, startY = 100.0;
    (void)ruler.apply(at(startX, startY));

    // A drifting hand: the y wanders 30 px off the guide.
    double worst = 0.0;
    for (int i = 1; i <= 60; ++i) {
        const double x = i * 5.0;
        const InputSample out = ruler.apply(at(x, startY + (i % 2 == 0 ? 30.0 : -30.0)));
        // The guide runs from (0, 100) to (400, 0): y = 100 - x / 4.
        const double onGuide = startY - out.x * 0.25;
        worst = std::max(worst, std::abs(out.y - onGuide));
    }
    CHECK(worst < 1e-9);
    CHECK(ruler.chosen() == 0);

    // Pressure is the artist's, not the ruler's (the same rule as US-11.6).
    InputSample sample = at(200.0, 50.0);
    sample.pressure = 0.42f;
    CHECK(ruler.apply(sample).pressure == doctest::Approx(0.42f));
}

TEST_CASE("perspective picks the point the stroke is heading towards") {
    // Two-point perspective: which guide is meant is in the opening direction,
    // so the artist never has to say which one out loud.
    PerspectiveRuler ruler;
    ruler.enabled = true;
    ruler.points.push_back(VanishingPoint{-1000.0, 0.0, true});    // away left
    ruler.points.push_back(VanishingPoint{1000.0, 0.0, true});     // away right

    ruler.reset();
    (void)ruler.apply(at(500.0, 500.0));
    (void)ruler.apply(at(520.0, 490.0));       // heading up and to the right
    CHECK(ruler.chosen() == 1);

    ruler.reset();
    (void)ruler.apply(at(500.0, 500.0));
    (void)ruler.apply(at(480.0, 490.0));       // heading up and to the left
    CHECK(ruler.chosen() == 0);

    // A single sample commits to nothing: the first pixel of a stroke is
    // tremor, and a guide chosen from tremor cannot be corrected mid-stroke.
    ruler.reset();
    (void)ruler.apply(at(500.0, 500.0));
    (void)ruler.apply(at(500.5, 500.2));
    CHECK(ruler.chosen() == -1);
}

TEST_CASE("symmetry composes with the stabilizer instead of fighting it") {
    // The mirror must be of the SMOOTHED stroke. Mirroring the raw input and
    // smoothing only the original gives two strokes of different shapes, which
    // is what "the rulers fight the stabilizer" would look like on screen.
    Document doc = makeDocument(256, 256, StraightRgba8{0, 0, 0, 0});
    Layer& layer = doc.layers.front();

    SymmetryRuler ruler;
    ruler.enabled  = true;
    ruler.vertical = true;
    ruler.centreX  = 128.0;
    ruler.centreY  = 128.0;

    Stabilizer stabilizer;
    stabilizer.setLevel(3);

    BrushPreset brush = defaultPencil();
    brush.size = 6.0f;
    Stroke stroke;
    beginStroke(stroke, brush, StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, stroke.pending, stroke.touched, doc.width, doc.height};

    std::vector<Dab> scratch;
    std::vector<SymmetryImage> images;
    double worstRaw = 0.0, worstPainted = 0.0;
    double lastX = 0.0, lastMirrorX = 0.0, lastY = 0.0;

    for (int i = 0; i < 160; ++i) {
        const double x = 20.0 + i * 0.5;
        const double y = 40.0 + (i % 2 == 0 ? 5.0 : -5.0);      // a shaky line
        // Past the first samples, where the string is still being taken up:
        // that is latency, not wobble, and D-103 says not to confuse the two.
        const bool settled = i >= 10;
        if (settled) worstRaw = std::max(worstRaw, std::abs(y - 40.0));

        const InputSample smoothed = stabilizer.apply(at(x, y));
        paintSample(stroke, target, smoothed, scratch);
        for (const Dab& dab : scratch) {
            if (settled) worstPainted = std::max(worstPainted, std::abs(dab.y - 40.0));
            ruler.map(dab.x, dab.y, dab.angle, images);
            REQUIRE(images.size() == 2);
            // The image is the mirror of what the STABILIZER produced, not of
            // the raw sample: the ruler sits downstream of the smoothing.
            CHECK(images[1].x == doctest::Approx(256.0 - dab.x));
            CHECK(images[1].y == doctest::Approx(dab.y));

            Dab mirrored = dab;
            mirrored.x = images[1].x;
            mirrored.y = images[1].y;
            applyDab(target, mirrored);

            lastX = dab.x;
            lastMirrorX = mirrored.x;
            lastY = dab.y;
        }
    }

    CHECK(worstPainted < worstRaw);          // the smoothing survived the mirror

    const auto opaque = [&](double x, double y) {
        return pickColour(doc, static_cast<std::int32_t>(std::lround(x)),
                          static_cast<std::int32_t>(std::lround(y))).a > 0;
    };
    CHECK(opaque(lastX, lastY));
    CHECK(opaque(lastMirrorX, lastY));

    // One stroke is ONE undo step however many dabs symmetry multiplied it
    // into — and undoing it has to clear both sides, which it only does if the
    // images went into the same record.
    doc.undo.push(std::move(stroke.pending));
    CHECK(doc.undo.size() == 1);
    (void)doc.undo.undo(doc);
    CHECK(!opaque(lastX, lastY));
    CHECK(!opaque(lastMirrorX, lastY));
}

// ----------------------------------------------------- brushes (M3/M4)

TEST_CASE("every built-in brush has a distinct id and a sane configuration") {
    const std::vector<BrushPreset> brushes = defaultBrushes();
    CHECK(brushes.size() >= 6);

    std::vector<std::string> ids;
    for (const BrushPreset& b : brushes) {
        CHECK(!b.id.empty());
        CHECK(!b.name.empty());
        CHECK(b.size > 0.0f);
        CHECK(b.density > 0.0f);
        CHECK(b.density <= 1.0f);
        CHECK(b.spacingFactor > 0.0f);
        // Spacing above half a diameter leaves visible beads at speed.
        CHECK(b.spacingFactor <= 0.5f);
        CHECK(b.hardness >= 0.0f);
        CHECK(b.hardness <= 1.0f);
        ids.push_back(b.id);
    }
    std::ranges::sort(ids);
    CHECK(std::ranges::adjacent_find(ids) == ids.end());   // no duplicates
}

TEST_CASE("the airbrush builds up rather than covering in one pass") {
    // A single airbrush dab must be faint; repeated passes make it solid.
    //
    // Each pass is a separate stroke, because dabs are emitted by MOVEMENT:
    // holding the brush still at one point produces no further paint. That is
    // a deliberate consequence of the position-driven dab pipeline — a
    // time-driven spray would need the loop to keep waking up, which D-008
    // rules out. Repeated passes is how an artist gets there anyway.
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
    std::vector<Dab> scratch;

    const auto pass = [&] {
        Layer* layer = doc.active();
        Stroke s;
        beginStroke(s, defaultAirbrush(), StraightRgba8{0, 0, 0, 255}, layer->id);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
        paintSample(s, t, at(64.0, 64.0), scratch);
    };

    pass();
    const StraightRgba8 onePass = pickColour(doc, 64, 64);
    CHECK(onePass.r > 200);                       // barely there

    for (int i = 0; i < 80; ++i) pass();
    const StraightRgba8 manyPasses = pickColour(doc, 64, 64);
    CHECK(manyPasses.r < onePass.r);              // and it accumulates
    CHECK(manyPasses.r < 80);
}

TEST_CASE("smudge carries colour out of a filled area") {
    // The whole point of the loaded-colour fields. Fill the left half, then
    // drag right across the boundary and check paint arrives where there was
    // none — carried, not generated.
    Document doc = makeDocument(200, 60, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{255, 255, 255, 0}));
    doc.active()->tiles.clear();

    doc.selection = Selection{0, 0, 100, 60};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{200, 20, 20, 255}));
    doc.selection.reset();
    REQUIRE(pickColour(doc, 50, 30) == StraightRgba8{200, 20, 20, 255});
    REQUIRE(pickColour(doc, 150, 30) == StraightRgba8{255, 255, 255, 255});

    Layer* layer = doc.active();
    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultSmudge(), StraightRgba8{0, 255, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
    for (double x = 40.0; x <= 130.0; x += 2.0) paintSample(s, t, at(x, 30.0), scratch);

    const StraightRgba8 smeared = pickColour(doc, 115, 30);
    CHECK(smeared.a > 0);
    // Reddish, carried from the fill — NOT the green the brush was handed,
    // because a smudge deposits what it picked up, not the current colour.
    CHECK(smeared.r > smeared.g);
    CHECK(smeared.r > smeared.b);
    CHECK(smeared.g < 200);
}

TEST_CASE("a brush with no blending ignores the smudge path entirely") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    Layer* layer = doc.active();
    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultOpaque(), StraightRgba8{10, 20, 30, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, t, at(32.0, 32.0), scratch);

    // The colour the artist chose, unmodified.
    CHECK(pickColour(doc, 32, 32) == StraightRgba8{10, 20, 30, 255});
    CHECK(s.loadedAmount == doctest::Approx(0.0f));
}

// ------------------------------------------- fill and selection (M3)

TEST_CASE("bucket fill spreads across a uniform region and stops at a line") {
    Document doc = makeDocument(200, 200, StraightRgba8{255, 255, 255, 255});
    const LayerId base = doc.activeLayer;

    // A black vertical wall down the middle, on its own layer — filling either
    // side must respect it even though it is not on the target layer.
    doc.undo.push(addLayerAbove(doc, base, "Line art"));
    const LayerId lineArt = doc.activeLayer;
    {
        Layer* layer = doc.layerById(lineArt);
        BrushPreset p = defaultPencil();
        p.size = 10.0f;
        p.hardness = 1.0f;
        p.pressure = PressureMapping{};
        p.pressure.toSize = false;
        Stroke s;
        std::vector<Dab> scratch;
        beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, lineArt);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
        for (double y = -5.0; y <= 205.0; y += 2.0) paintSample(s, t, at(100.0, y), scratch);
        doc.undo.push(std::move(s.pending));
    }

    UndoRecord rec = bucketFill(doc, base, 20, 100, StraightRgba8{255, 0, 0, 255}, 8);
    CHECK(!rec.empty());
    doc.undo.push(std::move(rec));

    // Filled to the left of the wall...
    CHECK(pickColour(doc, 20, 100) == StraightRgba8{255, 0, 0, 255});
    CHECK(pickColour(doc, 90, 100) == StraightRgba8{255, 0, 0, 255});
    // ...and not past it.
    CHECK(pickColour(doc, 150, 100) == StraightRgba8{255, 255, 255, 255});

    doc.undo.undo(doc);
    CHECK(pickColour(doc, 20, 100) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("bucket fill paints the target layer, not the layer it sampled") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    const LayerId base = doc.activeLayer;
    doc.undo.push(addLayerAbove(doc, base, "Colour"));
    const LayerId colour = doc.activeLayer;

    doc.undo.push(bucketFill(doc, colour, 32, 32, StraightRgba8{0, 128, 255, 255}, 0));
    CHECK(doc.layerById(colour)->tiles.size() == 1);
    CHECK(doc.layerById(base)->tiles.empty());       // untouched
}

TEST_CASE("filling an already-matching region is not an undo step") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    // The canvas is already white; filling it white changes nothing.
    const UndoRecord rec =
        bucketFill(doc, doc.activeLayer, 32, 32, StraightRgba8{255, 255, 255, 255}, 0);
    CHECK(rec.empty());
}

TEST_CASE("bucket fill is bounded by the selection") {
    Document doc = makeDocument(100, 100, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{10, 10, 30, 30};

    doc.undo.push(bucketFill(doc, doc.activeLayer, 20, 20,
                             StraightRgba8{0, 200, 0, 255}, 0));
    CHECK(pickColour(doc, 20, 20) == StraightRgba8{0, 200, 0, 255});
    CHECK(pickColour(doc, 39, 39) == StraightRgba8{0, 200, 0, 255});
    CHECK(pickColour(doc, 41, 41) == StraightRgba8{255, 255, 255, 255});   // outside
    CHECK(pickColour(doc, 5, 5)   == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("clicking outside the selection fills nothing") {
    Document doc = makeDocument(100, 100, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{10, 10, 20, 20};
    CHECK(bucketFill(doc, doc.activeLayer, 80, 80,
                     StraightRgba8{255, 0, 0, 255}, 0).empty());
}

TEST_CASE("painting is clipped to the selection") {
    Document doc = makeDocument(100, 100, StraightRgba8{255, 255, 255, 255});
    const Selection selection{40, 40, 20, 20};

    Layer* layer = doc.active();
    BrushPreset p = defaultPencil();
    p.size = 60.0f;                     // deliberately larger than the selection
    p.hardness = 1.0f;
    p.pressure = PressureMapping{};
    p.pressure.toSize = false;

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height, &selection};
    paintSample(s, t, at(50.0, 50.0), scratch);

    CHECK(pickColour(doc, 50, 50) == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 45, 45) == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 30, 50) == StraightRgba8{255, 255, 255, 255});   // outside
    CHECK(pickColour(doc, 50, 70) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("filling the selection covers exactly it") {
    Document doc = makeDocument(100, 100, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{25, 25, 50, 50};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{10, 20, 30, 255}));

    CHECK(pickColour(doc, 25, 25) == StraightRgba8{10, 20, 30, 255});
    CHECK(pickColour(doc, 74, 74) == StraightRgba8{10, 20, 30, 255});
    CHECK(pickColour(doc, 24, 24) == StraightRgba8{255, 255, 255, 255});
    CHECK(pickColour(doc, 75, 75) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("with no selection, fill covers the whole canvas") {
    Document doc = makeDocument(80, 60, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}));
    CHECK(pickColour(doc, 0, 0)   == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 79, 59) == StraightRgba8{0, 0, 0, 255});
}

TEST_CASE("a locked layer refuses paint and fill") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    doc.active()->locked = true;
    CHECK(bucketFill(doc, doc.activeLayer, 32, 32,
                     StraightRgba8{255, 0, 0, 255}, 0).empty());
    CHECK(fillSelection(doc, doc.activeLayer, StraightRgba8{255, 0, 0, 255}).empty());
    CHECK(doc.active()->tiles.empty());
}

// ----------------------------------------------------- transform (M3)

TEST_CASE("moving a selection carries the pixels and leaves the source empty") {
    Document doc = makeDocument(200, 200, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{20, 20, 40, 40};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{200, 30, 40, 255}));
    REQUIRE(pickColour(doc, 30, 30) == StraightRgba8{200, 30, 40, 255});

    Transform move;
    move.dx = 100.0;
    move.dy = 60.0;
    doc.undo.push(transformRegion(doc, doc.activeLayer, *doc.selection, move));

    CHECK(pickColour(doc, 130, 90) == StraightRgba8{200, 30, 40, 255});   // arrived
    CHECK(pickColour(doc, 30, 30)  == StraightRgba8{255, 255, 255, 255}); // and left
}

TEST_CASE("a transform is one undo step and restores exactly") {
    Document doc = makeDocument(160, 160, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{10, 10, 50, 50};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 90, 200, 255}));
    const std::uint64_t before = hashCanvas(doc);

    Transform t;
    t.dx = 40.0;
    t.dy = 25.0;
    t.scaleX = 1.4;
    t.scaleY = 1.4;
    doc.undo.push(transformRegion(doc, doc.activeLayer, *doc.selection, t));
    CHECK(doc.undo.size() == 2);
    const std::uint64_t after = hashCanvas(doc);
    CHECK(after != before);

    doc.undo.undo(doc);
    CHECK(hashCanvas(doc) == before);
    doc.undo.redo(doc);
    CHECK(hashCanvas(doc) == after);
}

TEST_CASE("scaling a region up makes it bigger") {
    Document doc = makeDocument(300, 300, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{100, 100, 40, 40};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}));

    Transform grow;
    grow.scaleX = 2.0;
    grow.scaleY = 2.0;
    doc.undo.push(transformRegion(doc, doc.activeLayer, *doc.selection, grow));

    // The 40 px square about its centre (120,120) becomes 80 px: 80..160.
    CHECK(pickColour(doc, 120, 120) == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 85, 120)  == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 155, 120) == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 70, 120)  == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("rotating a quarter turn swaps a rectangle's proportions") {
    Document doc = makeDocument(300, 300, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{100, 130, 80, 20};      // wide and short
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}));

    Transform turn;
    turn.angle = 3.14159265358979323846 / 2.0;
    doc.undo.push(transformRegion(doc, doc.activeLayer, *doc.selection, turn));

    // Centre (140,140): the bar should now be tall and narrow.
    CHECK(pickColour(doc, 140, 140).r == 0);
    CHECK(pickColour(doc, 140, 110).r == 0);          // extends vertically now
    CHECK(pickColour(doc, 140, 170).r == 0);
    CHECK(pickColour(doc, 175, 140) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("a transform that overlaps its own source is not corrupted") {
    // The usual case — a small nudge. Reading into a buffer first is what
    // stops the copy sampling pixels it has already written.
    Document doc = makeDocument(120, 120, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{20, 20, 60, 60};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{10, 200, 90, 255}));

    Transform nudge;
    nudge.dx = 5.0;
    doc.undo.push(transformRegion(doc, doc.activeLayer, *doc.selection, nudge));

    // A solid block moved by 5 px stays solid everywhere it still covers.
    for (std::int32_t x = 27; x < 83; ++x)
        CHECK(pickColour(doc, x, 50) == StraightRgba8{10, 200, 90, 255});
    CHECK(pickColour(doc, 22, 50) == StraightRgba8{255, 255, 255, 255});   // vacated
}

TEST_CASE("transforming an empty or locked region does nothing") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    CHECK(transformRegion(doc, doc.activeLayer, Selection{}, Transform{}).empty());

    doc.active()->locked = true;
    CHECK(transformRegion(doc, doc.activeLayer, Selection{0, 0, 10, 10},
                          Transform{}).empty());
}

// ------------------------------------------- lasso and magic wand (#18)

namespace {

/// A square as a lasso path, corners on pixel boundaries so the expected
/// coverage is exactly 0 or 255 and a test can be strict about it.
std::vector<Point> squarePath(double x, double y, double side) {
    return {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
}

}  // namespace

TEST_CASE("a lasso selects the inside of its loop and nothing else") {
    const Selection sel = lassoSelection(squarePath(20.0, 20.0, 40.0), 100, 100);
    REQUIRE(!sel.empty());
    CHECK(sel.x == 20);
    CHECK(sel.y == 20);
    CHECK(sel.w == 40);
    CHECK(sel.h == 40);
    CHECK(sel.contains(20, 20));
    CHECK(sel.contains(59, 59));
    CHECK(!sel.contains(19, 40));
    CHECK(!sel.contains(60, 40));
    // Axis- and pixel-aligned, so there is no partial coverage anywhere: the
    // rectangle it happens to be is recognised and the mask dropped.
    CHECK(sel.mask.empty());
}

TEST_CASE("a lasso edge is anti-aliased rather than stepped") {
    // A triangle, so the long edge crosses pixels at an angle. A hard-edged
    // rasteriser answers only 0 and 255; this must answer in between.
    const std::vector<Point> triangle{{10.0, 10.0}, {90.0, 10.0}, {10.0, 90.0}};
    const Selection sel = lassoSelection(triangle, 100, 100);
    REQUIRE(!sel.mask.empty());

    int partial = 0;
    for (const std::uint8_t v : sel.mask)
        if (v != 0 && v != 255) ++partial;
    CHECK(partial > 20);

    CHECK(sel.contains(15, 15));                     // well inside
    CHECK(!sel.contains(80, 80));                    // beyond the diagonal
    // The hypotenuse runs along x + y = 100, so this pixel straddles it.
    CHECK(sel.coverage(45, 54) > 0);
    CHECK(sel.coverage(45, 54) < 255);
}

TEST_CASE("a self-crossing lasso encloses everything it went round") {
    // A path that doubles back over itself. Even-odd winding punches the
    // overlap out; non-zero keeps it, which is what an artist scribbling a
    // loop means by it.
    const std::vector<Point> eight{{10.0, 10.0}, {50.0, 10.0}, {50.0, 50.0},
                                   {10.0, 50.0}, {10.0, 30.0}, {50.0, 30.0},
                                   {50.0, 40.0}, {10.0, 40.0}};
    const Selection sel = lassoSelection(eight, 100, 100);
    CHECK(sel.contains(30, 35));
}

TEST_CASE("a lasso with too few points, or off the canvas, selects nothing") {
    CHECK(lassoSelection(squarePath(10.0, 10.0, 20.0), 0, 0).empty());
    const std::vector<Point> two{{1.0, 1.0}, {5.0, 5.0}};
    CHECK(lassoSelection(two, 50, 50).empty());
    CHECK(lassoSelection(squarePath(-90.0, -90.0, 40.0), 50, 50).empty());
}

TEST_CASE("painting is clipped to a lasso, not to its bounding box") {
    Document doc = makeDocument(100, 100, StraightRgba8{255, 255, 255, 255});
    const std::vector<Point> triangle{{10.0, 10.0}, {90.0, 10.0}, {10.0, 90.0}};
    const Selection selection = lassoSelection(triangle, 100, 100);
    REQUIRE(!selection.mask.empty());

    Layer* layer = doc.active();
    BrushPreset p = defaultOpaque();
    p.size     = 200.0f;                // covers the whole bounding box
    p.hardness = 1.0f;
    p.pressure = PressureMapping{};

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height, &selection};
    paintSample(s, t, at(50.0, 50.0), scratch);

    CHECK(pickColour(doc, 15, 15) == StraightRgba8{0, 0, 0, 255});
    // Inside the bounding box but past the diagonal: a rectangle-only
    // selection would have painted here.
    CHECK(pickColour(doc, 80, 80) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("filling a lasso selection fades at the edge instead of stepping") {
    Document doc = makeDocument(100, 100, StraightRgba8{255, 255, 255, 255});
    const std::vector<Point> triangle{{10.0, 10.0}, {90.0, 10.0}, {10.0, 90.0}};
    doc.selection = lassoSelection(triangle, 100, 100);
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}));

    CHECK(pickColour(doc, 15, 15) == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 85, 85) == StraightRgba8{255, 255, 255, 255});

    // Somewhere along the diagonal there are grey pixels. Without coverage
    // there would be only black and white, and the edge would be a staircase.
    int greys = 0;
    for (std::int32_t i = 12; i < 88; ++i) {
        const StraightRgba8 c = pickColour(doc, i, 99 - i);
        if (c.r > 20 && c.r < 235) ++greys;
    }
    CHECK(greys > 5);
}

TEST_CASE("the magic wand finds the same region the bucket fill would") {
    Document doc = makeDocument(120, 120, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{30, 30, 50, 40};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 90, 200, 255}));
    doc.selection.reset();

    const Selection wand = magicWandSelection(doc, 50, 50, 0);
    REQUIRE(!wand.empty());
    CHECK(wand.contains(30, 30));
    CHECK(wand.contains(79, 69));
    CHECK(!wand.contains(29, 50));
    CHECK(!wand.contains(80, 50));

    // The same boundary the bucket's own flood reports — they run the same
    // code, and this is the test that says so out loud.
    const std::vector<StraightRgba8> composite = flatten(doc);
    const std::vector<bool> region =
        floodRegion(composite, doc.width, doc.height, 50, 50, 0, nullptr);
    for (std::int32_t y = 0; y < doc.height; ++y)
        for (std::int32_t x = 0; x < doc.width; ++x)
            REQUIRE(region[static_cast<std::size_t>(y) * 120 + x] == wand.contains(x, y));
}

TEST_CASE("the magic wand is bounded by tolerance and by the canvas") {
    Document doc = makeDocument(60, 60, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{0, 0, 30, 60};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{250, 250, 250, 255}));
    doc.selection.reset();

    // Five apart: outside tolerance 2, inside tolerance 16. Asked with
    // `contains` rather than by width, because the bounding box also holds the
    // one-pixel soft rim.
    CHECK(!magicWandSelection(doc, 10, 10, 2).contains(30, 10));
    CHECK(magicWandSelection(doc, 10, 10, 16).contains(30, 10));
    CHECK(magicWandSelection(doc, 10, 10, 16).contains(59, 59));
    // A click off the canvas selects nothing rather than clamping to an edge
    // the artist did not click on.
    CHECK(magicWandSelection(doc, -1, 10, 0).empty());
    CHECK(magicWandSelection(doc, 60, 10, 0).empty());
}

TEST_CASE("a magic wand selection constrains painting and bucket fill") {
    Document doc = makeDocument(120, 120, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{20, 20, 40, 40};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 200, 0, 255}));
    doc.selection = magicWandSelection(doc, 40, 40, 0);
    REQUIRE(!doc.selection->empty());

    doc.undo.push(bucketFill(doc, doc.activeLayer, 40, 40,
                             StraightRgba8{200, 0, 0, 255}, 0));
    CHECK(pickColour(doc, 40, 40) == StraightRgba8{200, 0, 0, 255});
    CHECK(pickColour(doc, 100, 100) == StraightRgba8{255, 255, 255, 255});
    // Clicking outside the wand's region still fills nothing.
    CHECK(bucketFill(doc, doc.activeLayer, 100, 100,
                     StraightRgba8{0, 0, 255, 255}, 0).empty());
}

TEST_CASE("transforming a lasso region moves the shape, not its bounding box") {
    Document doc = makeDocument(120, 120, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}));

    const std::vector<Point> triangle{{20.0, 20.0}, {80.0, 20.0}, {20.0, 80.0}};
    const Selection region = lassoSelection(triangle, 120, 120);
    doc.undo.push(transformRegion(doc, doc.activeLayer, region, Transform{}));

    // A pixel inside the loop was lifted and put straight back.
    CHECK(pickColour(doc, 30, 30) == StraightRgba8{0, 0, 0, 255});
    // A pixel in the bounding box but outside the loop was never lifted, so it
    // is still black rather than cleared.
    CHECK(pickColour(doc, 75, 75) == StraightRgba8{0, 0, 0, 255});
}

TEST_CASE("add, subtract and intersect combine coverage") {
    const Selection a{0, 0, 40, 40};
    const Selection b{20, 20, 40, 40};

    const Selection sum = combineSelections(a, b, SelectMode::Add);
    CHECK(sum.x == 0);
    CHECK(sum.w == 60);
    CHECK(sum.contains(5, 5));
    CHECK(sum.contains(55, 55));
    CHECK(!sum.contains(55, 5));       // the corner neither rectangle covers

    const Selection cut = combineSelections(a, b, SelectMode::Subtract);
    CHECK(cut.contains(5, 5));
    CHECK(!cut.contains(30, 30));

    const Selection both = combineSelections(a, b, SelectMode::Intersect);
    CHECK(both.x == 20);
    CHECK(both.y == 20);
    CHECK(both.w == 20);
    CHECK(both.h == 20);
    // Two rectangles intersected are a rectangle: the mask must be dropped, or
    // every selection stays a mask for the rest of the session.
    CHECK(both.mask.empty());

    CHECK(combineSelections(a, b, SelectMode::Replace) == b);
}

TEST_CASE("combining with nothing is not a way to lose a selection") {
    const Selection a{10, 10, 20, 20};
    CHECK(combineSelections(a, Selection{}, SelectMode::Add) == a);
    CHECK(combineSelections(a, Selection{}, SelectMode::Subtract) == a);
    CHECK(combineSelections(a, Selection{}, SelectMode::Intersect).empty());
    CHECK(combineSelections(Selection{}, a, SelectMode::Add) == a);
    // Nothing to subtract from, and nothing to intersect with.
    CHECK(combineSelections(Selection{}, a, SelectMode::Subtract).empty());
    CHECK(combineSelections(Selection{}, a, SelectMode::Intersect).empty());

    // Subtracting a region from itself leaves nothing at all, not an empty box
    // that still blocks painting.
    CHECK(combineSelections(a, a, SelectMode::Subtract).empty());
}

TEST_CASE("subtracting keeps the anti-aliased edge of what remains") {
    const std::vector<Point> triangle{{10.0, 10.0}, {90.0, 10.0}, {10.0, 90.0}};
    const Selection lasso = lassoSelection(triangle, 100, 100);
    const Selection cut =
        combineSelections(lasso, Selection{10, 10, 20, 20}, SelectMode::Subtract);
    REQUIRE(!cut.mask.empty());
    CHECK(!cut.contains(15, 15));                    // taken out
    CHECK(cut.contains(60, 15));                     // still in
    int partial = 0;
    for (const std::uint8_t v : cut.mask)
        if (v != 0 && v != 255) ++partial;
    CHECK(partial > 20);                             // the diagonal survived
}

TEST_CASE("a rectangular selection carries no mask") {
    // The fast path, stated as a test: the common case must not start paying
    // for the feature that made the uncommon one possible.
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{8, 8, 16, 16};
    CHECK(doc.selection->mask.empty());
    CHECK(doc.selection->coverage(8, 8) == 255);
    CHECK(doc.selection->coverage(7, 8) == 0);
}

TEST_CASE("a selection with a mask survives a save, a load and a clone") {
    Document doc = makeDocument(120, 120, StraightRgba8{255, 255, 255, 255});
    const std::vector<Point> triangle{{10.0, 10.0}, {100.0, 10.0}, {10.0, 100.0}};
    doc.selection = lassoSelection(triangle, 120, 120);
    REQUIRE(!doc.selection->mask.empty());

    const auto path = scratchFile("sable_selection.sable");
    REQUIRE(saveProject(doc, path).has_value());
    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->selection.has_value());
    CHECK(*loaded->selection == *doc.selection);

    // The clone the background save hands its worker must carry it too, or a
    // recovered file comes back without the selection (D-013).
    const Document copy = cloneDocument(*loaded);
    REQUIRE(copy.selection.has_value());
    CHECK(*copy.selection == *doc.selection);
    std::filesystem::remove(path);
}

TEST_CASE("a rectangular selection round-trips without a mask file") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{5, 6, 20, 21};

    const auto path = scratchFile("sable_selection_rect.sable");
    REQUIRE(saveProject(doc, path).has_value());

    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_file(&zip, path.string().c_str(), 0));
    CHECK(mz_zip_reader_locate_file(&zip, "selection.png", nullptr, 0) < 0);
    mz_zip_reader_end(&zip);

    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->selection.has_value());
    CHECK(*loaded->selection == Selection{5, 6, 20, 21});
    std::filesystem::remove(path);
}

TEST_CASE("a document with nothing selected saves no selection at all") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    const auto path = scratchFile("sable_selection_none.sable");
    REQUIRE(saveProject(doc, path).has_value());
    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());
    CHECK(!loaded->selection.has_value());
    std::filesystem::remove(path);
}

// ------------------------------------------------- project format (M3)

namespace {

/// A document with enough variety that a lazy writer cannot round-trip it by
/// accident: two layers, non-default properties, negative-ish coordinates.
Document sampleDocument() {
    Document doc = makeDocument(600, 400, StraightRgba8{240, 230, 220, 255});
    doc.dpi = 144;
    paintSquare(doc, doc.activeLayer, StraightRgba8{200, 30, 40, 255}, 100.0, 100.0);

    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Shading"));
    const LayerId top = doc.activeLayer;
    paintSquare(doc, top, StraightRgba8{20, 60, 200, 255}, 400.0, 300.0);

    Layer* layer = doc.layerById(top);
    layer->opacity         = 0.6f;
    layer->blend           = BlendMode::Multiply;
    layer->locked          = true;
    layer->preserveOpacity = true;
    return doc;
}

}  // namespace

TEST_CASE("a project round-trips through .sable") {
    const Document original = sampleDocument();
    const std::uint64_t before = hashCanvas(original);
    const auto path = scratchFile("sable_roundtrip.sable");

    REQUIRE(saveProject(original, path).has_value());
    REQUIRE(std::filesystem::exists(path));

    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());

    CHECK(loaded->width  == original.width);
    CHECK(loaded->height == original.height);
    CHECK(loaded->dpi    == original.dpi);
    CHECK(loaded->background == original.background);
    REQUIRE(loaded->layers.size() == original.layers.size());

    for (std::size_t i = 0; i < loaded->layers.size(); ++i) {
        const Layer& a = original.layers[i];
        const Layer& b = loaded->layers[i];
        CHECK(b.id   == a.id);
        CHECK(b.name == a.name);
        CHECK(b.blend == a.blend);
        CHECK(b.visible == a.visible);
        CHECK(b.locked  == a.locked);
        CHECK(b.preserveOpacity == a.preserveOpacity);
        CHECK(b.opacity == doctest::Approx(a.opacity).epsilon(0.005));
        CHECK(b.tiles.size() == a.tiles.size());
    }

    // The pixels are what actually matter. Tiles are stored as straight alpha,
    // so this also covers the premultiply round-trip across a save (D-004).
    CHECK(hashCanvas(*loaded) == before);
    CHECK(!loaded->dirty);
    CHECK(loaded->path == path);
    std::filesystem::remove(path);
}

TEST_CASE("saving twice produces the same bytes") {
    // Sorted tile lists and no timestamps in the manifest, so a project under
    // version control does not churn.
    const Document doc = sampleDocument();
    const auto a = scratchFile("sable_stable_a.sable");
    const auto b = scratchFile("sable_stable_b.sable");
    REQUIRE(saveProject(doc, a).has_value());
    REQUIRE(saveProject(doc, b).has_value());
    CHECK(std::filesystem::file_size(a) == std::filesystem::file_size(b));
    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

TEST_CASE("a saved project is a real ZIP holding real PNGs") {
    // D-011's whole argument for this format is that it stays inspectable with
    // unzip and any image viewer. Check the local file header magic.
    const Document doc = sampleDocument();
    const auto path = scratchFile("sable_iszip.sable");
    REQUIRE(saveProject(doc, path).has_value());

    std::vector<unsigned char> bytes(4);
    FILE* in = std::fopen(path.string().c_str(), "rb");
    REQUIRE(in != nullptr);
    REQUIRE(std::fread(bytes.data(), 1, 4, in) == 4);
    std::fclose(in);
    CHECK(bytes[0] == 'P');
    CHECK(bytes[1] == 'K');
    CHECK(bytes[2] == 3);
    CHECK(bytes[3] == 4);
    std::filesystem::remove(path);
}

TEST_CASE("undo history is not saved") {
    Document doc = sampleDocument();
    REQUIRE(doc.undo.canUndo());
    const auto path = scratchFile("sable_noundo.sable");
    REQUIRE(saveProject(doc, path).has_value());

    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());
    CHECK(!loaded->undo.canUndo());
    CHECK(!loaded->undo.canRedo());
    std::filesystem::remove(path);
}

/// Rewrites the manifest's format_version in place, leaving everything else.
/// Used to fake both a future file and an older one.
namespace {
void rewriteFormatVersion(const std::filesystem::path& path, int version) {
    mz_zip_archive in{};
    REQUIRE(mz_zip_reader_init_file(&in, path.string().c_str(), 0));
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&in, "document.json", &size, 0);
    REQUIRE(data != nullptr);
    std::string text(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_reader_end(&in);

    const std::string key = "\"format_version\": ";
    const auto at = text.find(key);
    REQUIRE(at != std::string::npos);
    const auto end = text.find_first_not_of("0123456789", at + key.size());
    REQUIRE(end != std::string::npos);
    text.replace(at, end - at, key + std::to_string(version));

    mz_zip_archive out{};
    REQUIRE(mz_zip_writer_init_file(&out, path.string().c_str(), 0));
    mz_zip_writer_add_mem(&out, "document.json", text.data(), text.size(),
                          MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_finalize_archive(&out);
    mz_zip_writer_end(&out);
}
}  // namespace

TEST_CASE("a newer format version is refused rather than read wrong") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    const auto path = scratchFile("sable_future.sable");
    REQUIRE(saveProject(doc, path).has_value());
    rewriteFormatVersion(path, 999);

    const auto loaded = loadProject(path);
    REQUIRE(!loaded.has_value());
    CHECK(loaded.error().kind == ErrorKind::UnsupportedVersion);
    CHECK(loaded.error().detail.find("newer version") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("vanishing points survive a save and load") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    doc.vanishingPoints.push_back(VanishingPoint{-320.5, 40.25, true});
    doc.vanishingPoints.push_back(VanishingPoint{900.0, 40.25, false});

    const auto path = scratchFile("sable_vanishing.sable");
    REQUIRE(saveProject(doc, path).has_value());
    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->vanishingPoints.size() == 2);
    CHECK(loaded->vanishingPoints[0].x == doctest::Approx(-320.5));
    CHECK(loaded->vanishingPoints[0].y == doctest::Approx(40.25));
    CHECK(loaded->vanishingPoints[0].enabled);
    CHECK(loaded->vanishingPoints[1].x == doctest::Approx(900.0));
    // Off is a position worth keeping, not a reason to drop the point.
    CHECK(!loaded->vanishingPoints[1].enabled);

    // The clone the background save hands to its worker has to carry them too,
    // or a recovered file comes back without its perspective (D-013).
    CHECK(cloneDocument(*loaded).vanishingPoints.size() == 2);
    std::filesystem::remove(path);
}

TEST_CASE("files from before the version bumps still open") {
    // A v1 document is a v2 one with no vanishing_points, and a v2 one is a v3
    // with no selection. Everything each bump added is optional, so no bump may
    // cost an existing file its contents.
    for (const int version : {1, 2}) {
        Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
        const auto path = scratchFile("sable_old.sable");
        REQUIRE(saveProject(doc, path).has_value());
        rewriteFormatVersion(path, version);

        const auto loaded = loadProject(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->width == 64);
        CHECK(loaded->layers.size() == 1);
        CHECK(loaded->vanishingPoints.empty());
        CHECK(!loaded->selection.has_value());
        std::filesystem::remove(path);
    }
}

TEST_CASE("malformed and missing files fail with a message, never a crash") {
    // The cross-cutting input-trust rule: never a crash, never a silent
    // partial load.
    CHECK(!loadProject("/nowhere/at/all.sable").has_value());
    CHECK(loadProject("/nowhere/at/all.sable").error().kind == ErrorKind::NotFound);

    const auto junk = scratchFile("sable_junk.sable");
    {
        FILE* out = std::fopen(junk.string().c_str(), "wb");
        REQUIRE(out != nullptr);
        const char* rubbish = "this is definitely not a zip archive";
        std::fwrite(rubbish, 1, std::strlen(rubbish), out);
        std::fclose(out);
    }
    const auto loaded = loadProject(junk);
    REQUIRE(!loaded.has_value());
    CHECK(loaded.error().kind == ErrorKind::Malformed);
    CHECK(!loaded.error().detail.empty());
    std::filesystem::remove(junk);
}

TEST_CASE("a truncated manifest tile list still loads the surviving tiles") {
    // "If the manifest's tiles list disagrees with the ZIP entries, trust the
    // ZIP and repair the manifest" (D-011). Tiles are loaded from the ZIP
    // directory, so an empty list in the manifest must not lose pixels.
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}, 100.0, 100.0);
    const std::uint64_t before = hashCanvas(doc);

    const auto path = scratchFile("sable_repair.sable");
    REQUIRE(saveProject(doc, path).has_value());

    {
        mz_zip_archive in{};
        REQUIRE(mz_zip_reader_init_file(&in, path.string().c_str(), 0));
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&in, "document.json", &size, 0);
        std::string text(static_cast<const char*>(data), size);
        mz_free(data);

        // Keep every tile PNG, but empty the manifest's list of them.
        std::vector<std::pair<std::string, std::vector<unsigned char>>> kept;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i) {
            mz_zip_archive_file_stat stat{};
            mz_zip_reader_file_stat(&in, i, &stat);
            const std::string name = stat.m_filename;
            if (!name.starts_with("layers/")) continue;
            std::size_t n = 0;
            void* d = mz_zip_reader_extract_to_heap(&in, i, &n, 0);
            kept.emplace_back(name, std::vector<unsigned char>(
                static_cast<unsigned char*>(d), static_cast<unsigned char*>(d) + n));
            mz_free(d);
        }
        mz_zip_reader_end(&in);
        REQUIRE(!kept.empty());

        auto json = nlohmann::json::parse(text);
        json["layers"][0]["tiles"] = nlohmann::json::array();
        text = json.dump(2);

        mz_zip_archive out{};
        REQUIRE(mz_zip_writer_init_file(&out, path.string().c_str(), 0));
        mz_zip_writer_add_mem(&out, "document.json", text.data(), text.size(),
                              MZ_DEFAULT_COMPRESSION);
        for (const auto& [name, bytes] : kept)
            mz_zip_writer_add_mem(&out, name.c_str(), bytes.data(), bytes.size(),
                                  MZ_NO_COMPRESSION);
        mz_zip_writer_finalize_archive(&out);
        mz_zip_writer_end(&out);
    }

    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());
    CHECK(hashCanvas(*loaded) == before);      // the pixels survived the bad manifest
    std::filesystem::remove(path);
}

// -------------------------------------------------------- the format registry

namespace {

/// Stands in for the importers still to come. It does what a careless one would
/// do — sets Document::path — so the tests can prove the registry undoes it.
std::expected<Document, Error> readPretend(const std::filesystem::path& path) {
    Document doc = makeDocument(16, 16, StraightRgba8{255, 255, 255, 255});
    doc.path = path;
    return doc;
}

bool looksLikePretend(const std::filesystem::path& path) {
    return readMagic(path, 7) == "PRETEND";
}

/// Idempotent: test cases must not depend on which of them ran first.
void ensurePretendFormat() {
    for (const Format& format : formats())
        if (format.id == "pretend") return;
    registerFormat(Format{.id = "pretend", .label = "Pretend image",
                          .extensions = {"pretend"}, .nativeProject = false,
                          .read = &readPretend, .write = nullptr,
                          .sniff = &looksLikePretend});
}

std::filesystem::path writePretendFile(const char* name) {
    const auto path = scratchFile(name);
    FILE* out = std::fopen(path.string().c_str(), "wb");
    REQUIRE(out != nullptr);
    std::fwrite("PRETENDnot really an image", 1, 26, out);
    std::fclose(out);
    return path;
}

}  // namespace

TEST_CASE("the registry opens .sable through the same call as any other format") {
    const Document original = sampleDocument();
    const auto path = scratchFile("registry_native.sable");
    REQUIRE(exportDocument(original, path).has_value());

    const auto loaded = importDocument(path);
    REQUIRE(loaded.has_value());
    CHECK(hashCanvas(*loaded) == hashCanvas(original));
    // The native project format is the only one that owns its path.
    CHECK(loaded->path == path);
    std::filesystem::remove(path);
}

TEST_CASE("an imported document has no path, so Ctrl+S cannot overwrite it") {
    // The trap this registry exists to close: Ctrl+S writes a .sable archive
    // straight to Document::path. If an import left the artist's own file there,
    // saving would destroy it.
    ensurePretendFormat();
    const auto path = writePretendFile("registry_import.pretend");

    const auto imported = importDocument(path);
    REQUIRE(imported.has_value());
    CHECK(imported->path.empty());
    CHECK(imported->width == 16);
    std::filesystem::remove(path);
}

TEST_CASE("content decides when the extension lies") {
    // .sable, .ora and .kra are all ZIPs, so a wrong extension is not exotic.
    ensurePretendFormat();
    const auto path = writePretendFile("registry_misnamed.sable");

    const auto imported = importDocument(path);
    REQUIRE(imported.has_value());
    CHECK(imported->width == 16);        // read as a pretend file, not as .sable
    CHECK(imported->path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("an unrecognised file fails with a message naming what would work") {
    const auto path = scratchFile("registry_mystery.xyz");
    { FILE* out = std::fopen(path.string().c_str(), "wb");
      REQUIRE(out != nullptr);
      std::fwrite("nothing recognisable", 1, 20, out);
      std::fclose(out); }

    const auto imported = importDocument(path);
    REQUIRE(!imported.has_value());
    CHECK(imported.error().kind == ErrorKind::Malformed);
    CHECK(imported.error().detail.find(".sable") != std::string::npos);

    CHECK(!importDocument("/nowhere/at/all.sable").has_value());
    CHECK(importDocument("/nowhere/at/all.sable").error().kind == ErrorKind::NotFound);

    // Writing is extension-only: there is nothing to sniff in a file that does
    // not exist yet, so an unknown one must fail rather than guess.
    const auto out = scratchFile("registry_mystery_out.xyz");
    const auto written = exportDocument(sampleDocument(), out);
    REQUIRE(!written.has_value());
    CHECK(!written.error().detail.empty());
    CHECK(!std::filesystem::exists(out));
    std::filesystem::remove(path);
}

TEST_CASE("the dialog filters come from the registry") {
    ensurePretendFormat();
    const auto open = dialogFilters(FormatUse::Read, true);
    REQUIRE(open.size() == 1);
    CHECK(open[0].pattern == "sable");

    const auto import = dialogFilters(FormatUse::Read, false);
    CHECK(std::ranges::any_of(import, [](const DialogFilter& f) {
        return f.pattern == "pretend";
    }));

    // Export offers what can be written and is not the project format itself.
    const auto exportable = dialogFilters(FormatUse::Write, false);
    CHECK(std::ranges::any_of(exportable, [](const DialogFilter& f) {
        return f.pattern == "png";
    }));
    CHECK(std::ranges::none_of(exportable, [](const DialogFilter& f) {
        return f.pattern == "sable";
    }));
}

// ------------------------------------------------------------------ PSD (#6)
//
// The fixtures come from tests/data/make_psd_fixtures.py, which composites the
// same artwork independently in floating point. flat.psd is therefore ground
// truth for what Photoshop, Krita and GIMP show, and comparing Sable's
// flatten() of layered.psd against it is a check against another
// implementation rather than against Sable itself.

namespace {

std::filesystem::path testData(const char* name) {
    return std::filesystem::path(SABLE_TEST_DATA_DIR) / name;
}

const Layer* layerNamed(const Document& doc, std::string_view name) {
    for (const Layer& layer : doc.layers)
        if (layer.name == name) return &layer;
    return nullptr;
}

/// A 26-byte PSD header and three empty sections. Enough to reach — and be
/// rejected by — every check readPsd() makes before it looks at any pixels.
std::filesystem::path writePsdHeader(const char* file, std::uint16_t depth,
                                     std::uint16_t colourMode) {
    const auto path = scratchFile(file);
    const auto be16 = [](std::vector<unsigned char>& out, std::uint16_t v) {
        out.push_back(static_cast<unsigned char>(v >> 8));
        out.push_back(static_cast<unsigned char>(v & 0xFF));
    };
    const auto be32 = [&](std::vector<unsigned char>& out, std::uint32_t v) {
        be16(out, static_cast<std::uint16_t>(v >> 16));
        be16(out, static_cast<std::uint16_t>(v & 0xFFFF));
    };

    std::vector<unsigned char> bytes{'8', 'B', 'P', 'S'};
    be16(bytes, 1);
    bytes.insert(bytes.end(), 6, 0);
    be16(bytes, 4);
    be32(bytes, 8);            // height
    be32(bytes, 8);            // width
    be16(bytes, depth);
    be16(bytes, colourMode);
    be32(bytes, 0);            // colour mode data
    be32(bytes, 0);            // image resources
    be32(bytes, 0);            // layer and mask information

    FILE* out = std::fopen(path.string().c_str(), "wb");
    REQUIRE(out != nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);
    return path;
}

}  // namespace

TEST_CASE("a layered PSD imports its stack, groups, names and modes") {
    const auto doc = importDocument(testData("layered.psd"));
    REQUIRE(doc.has_value());

    CHECK(doc->width == 64);
    CHECK(doc->height == 48);
    // PSD has no document background, so nothing may be composited underneath
    // or a file with transparency stops matching what Photoshop shows.
    CHECK(doc->background.a == 0);
    CHECK(doc->path.empty());
    CHECK(!doc->dirty);

    // Bottom to top, a folder ahead of its own children — Sable's order, which
    // PSD writes the other way round.
    REQUIRE(doc->layers.size() == 6);
    CHECK(doc->layers[0].name == "Base");
    CHECK(doc->layers[1].name == "Sky");
    CHECK(doc->layers[2].name == "Fill");
    CHECK(doc->layers[3].name == "Tint");
    CHECK(doc->layers[4].name == "Shadow");
    CHECK(doc->layers[5].name == "Ink");

    const Layer* group = layerNamed(*doc, "Sky");
    REQUIRE(group != nullptr);
    CHECK(group->kind == LayerKind::Folder);
    CHECK(group->tiles.empty());
    CHECK(group->opacity == doctest::Approx(200.0f / 255.0f).epsilon(0.005));
    for (const char* child : {"Fill", "Tint", "Shadow"}) {
        const Layer* layer = layerNamed(*doc, child);
        REQUIRE(layer != nullptr);
        CHECK(layer->kind == LayerKind::Raster);
        CHECK(layer->parent == group->id);
    }
    for (const char* top : {"Base", "Ink"})
        CHECK(!layerNamed(*doc, top)->parent.has_value());

    CHECK(layerNamed(*doc, "Ink")->blend == BlendMode::Multiply);
    CHECK(layerNamed(*doc, "Ink")->opacity == doctest::Approx(128.0f / 255.0f).epsilon(0.005));
    CHECK(!layerNamed(*doc, "Shadow")->visible);
    CHECK(layerNamed(*doc, "Tint")->clipToBelow);
    CHECK(!layerNamed(*doc, "Fill")->clipToBelow);

    // Both compression schemes: "Fill" is stored raw, the rest PackBits.
    CHECK(!layerNamed(*doc, "Fill")->tiles.empty());
    CHECK(!layerNamed(*doc, "Base")->tiles.empty());
}

TEST_CASE("a PSD flattens to the composite the file itself carries") {
    // The acceptance criterion for #6: what Sable draws matches what every
    // other application shows for the same file, within 8-bit tolerance.
    const auto layered = importDocument(testData("layered.psd"));
    const auto flat    = importDocument(testData("flat.psd"));
    REQUIRE(layered.has_value());
    REQUIRE(flat.has_value());

    // A PSD with no layer section is a real case, not a corner: it is what
    // most exporters produce.
    REQUIRE(flat->layers.size() == 1);

    const std::vector<StraightRgba8> ours   = flatten(*layered);
    const std::vector<StraightRgba8> theirs = flatten(*flat);
    REQUIRE(ours.size() == theirs.size());

    int worst = 0;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        worst = std::max({worst,
            std::abs(ours[i].r - theirs[i].r), std::abs(ours[i].g - theirs[i].g),
            std::abs(ours[i].b - theirs[i].b), std::abs(ours[i].a - theirs[i].a)});
    }
    CHECK(worst <= 2);
}

TEST_CASE("an imported PSD round-trips through .sable") {
    const auto imported = importDocument(testData("layered.psd"));
    REQUIRE(imported.has_value());
    const std::uint64_t before = hashCanvas(*imported);

    const auto path = scratchFile("psd_import_roundtrip.sable");
    REQUIRE(saveProject(*imported, path).has_value());
    const auto reloaded = loadProject(path);
    REQUIRE(reloaded.has_value());

    REQUIRE(reloaded->layers.size() == imported->layers.size());
    for (std::size_t i = 0; i < reloaded->layers.size(); ++i) {
        CHECK(reloaded->layers[i].name   == imported->layers[i].name);
        CHECK(reloaded->layers[i].blend  == imported->layers[i].blend);
        CHECK(reloaded->layers[i].parent == imported->layers[i].parent);
    }
    CHECK(hashCanvas(*reloaded) == before);
    std::filesystem::remove(path);
}

TEST_CASE("a PSD is recognised by content even when the extension lies") {
    const auto path = scratchFile("psd_misnamed.sable");
    std::filesystem::copy_file(testData("layered.psd"), path);

    const auto doc = importDocument(path);
    REQUIRE(doc.has_value());
    CHECK(doc->layers.size() == 6);
    CHECK(doc->path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("PSDs Sable cannot read are refused, not read wrong") {
    // Until 16-bit lands (D-023), the honest answer is a message naming what
    // the file actually is — never a canvas full of garbage.
    const auto deep = writePsdHeader("psd_16bit.psd", 16, 3);
    const auto wide = importDocument(deep);
    REQUIRE(!wide.has_value());
    CHECK(wide.error().detail.find("16 bits") != std::string::npos);

    const auto cmyk = writePsdHeader("psd_cmyk.psd", 8, 4);
    const auto inky = importDocument(cmyk);
    REQUIRE(!inky.has_value());
    CHECK(inky.error().detail.find("CMYK") != std::string::npos);

    // Truncation is the common form of corruption, and must not crash.
    const auto stub = scratchFile("psd_truncated.psd");
    { FILE* out = std::fopen(stub.string().c_str(), "wb");
      REQUIRE(out != nullptr);
      std::fwrite("8BPS\0\1", 1, 6, out);
      std::fclose(out); }
    CHECK(!importDocument(stub).has_value());

    std::filesystem::remove(deep);
    std::filesystem::remove(cmyk);
    std::filesystem::remove(stub);
}

// ------------------------------------------------------------ PSD export (#7)

namespace {

/// Everything the writer has to carry: a group with children, a clipped layer,
/// a hidden layer, non-Normal blend modes and partial opacities.
///
/// The background is transparent because PSD has no document background — one
/// that is not becomes an extra layer, which has its own test below.
Document psdSampleDocument() {
    Document doc = makeDocument(300, 200, StraightRgba8{0, 0, 0, 0});
    doc.dpi = 144;
    doc.layers[0].name = "Base";
    paintSquare(doc, doc.activeLayer, StraightRgba8{200, 30, 40, 255}, 80.0, 80.0);

    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Group"));
    const LayerId group = doc.activeLayer;
    doc.layerById(group)->kind    = LayerKind::Folder;
    doc.layerById(group)->opacity = 0.6f;

    doc.undo.push(addLayerAbove(doc, group, "Fill"));
    const LayerId fill = doc.activeLayer;
    doc.layerById(fill)->parent = group;
    paintSquare(doc, fill, StraightRgba8{20, 60, 200, 255}, 150.0, 100.0);

    doc.undo.push(addLayerAbove(doc, fill, "Tint"));
    const LayerId tint = doc.activeLayer;
    doc.layerById(tint)->parent      = group;
    doc.layerById(tint)->clipToBelow = true;
    doc.layerById(tint)->blend       = BlendMode::Screen;
    paintSquare(doc, tint, StraightRgba8{240, 220, 60, 200}, 160.0, 110.0);

    doc.undo.push(addLayerAbove(doc, tint, "Hidden"));
    doc.layerById(doc.activeLayer)->visible         = false;
    doc.layerById(doc.activeLayer)->preserveOpacity = true;
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}, 40.0, 160.0);

    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Ink"));
    doc.layerById(doc.activeLayer)->blend   = BlendMode::Multiply;
    doc.layerById(doc.activeLayer)->opacity = 0.4f;
    paintSquare(doc, doc.activeLayer, StraightRgba8{40, 200, 90, 220}, 200.0, 150.0);
    return doc;
}

/// The same PSD with its layer section removed, so importing it yields only the
/// merged composite the file carries — which is exactly what a viewer that does
/// not parse layers shows.
std::filesystem::path flattenedCopyOfPsd(const std::filesystem::path& src,
                                         const char* name) {
    std::vector<unsigned char> bytes;
    { FILE* in = std::fopen(src.string().c_str(), "rb");
      REQUIRE(in != nullptr);
      std::fseek(in, 0, SEEK_END);
      bytes.resize(static_cast<std::size_t>(std::ftell(in)));
      std::fseek(in, 0, SEEK_SET);
      bytes.resize(std::fread(bytes.data(), 1, bytes.size(), in));
      std::fclose(in); }
    REQUIRE(bytes.size() > 26);

    // Header, then three length-prefixed sections; the merged image follows.
    std::size_t at = 26;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(at + 4 <= bytes.size());
        at += 4 + ((static_cast<std::size_t>(bytes[at]) << 24) |
                   (static_cast<std::size_t>(bytes[at + 1]) << 16) |
                   (static_cast<std::size_t>(bytes[at + 2]) << 8) |
                    static_cast<std::size_t>(bytes[at + 3]));
    }
    REQUIRE(at <= bytes.size());

    const auto out = scratchFile(name);
    FILE* file = std::fopen(out.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    const std::vector<unsigned char> empty(12, 0);      // three zero lengths
    std::fwrite(bytes.data(), 1, 26, file);
    std::fwrite(empty.data(), 1, empty.size(), file);
    std::fwrite(bytes.data() + at, 1, bytes.size() - at, file);
    std::fclose(file);
    return out;
}

}  // namespace

TEST_CASE("a document round-trips through PSD") {
    const Document original = psdSampleDocument();
    const std::uint64_t before = hashCanvas(original);
    const auto path = scratchFile("psd_export_roundtrip.psd");

    REQUIRE(exportDocument(original, path).has_value());
    const auto loaded = importDocument(path);
    REQUIRE(loaded.has_value());

    CHECK(loaded->width  == original.width);
    CHECK(loaded->height == original.height);
    REQUIRE(loaded->layers.size() == original.layers.size());

    for (std::size_t i = 0; i < loaded->layers.size(); ++i) {
        const Layer& a = original.layers[i];
        const Layer& b = loaded->layers[i];
        CHECK(b.name    == a.name);
        CHECK(b.kind    == a.kind);
        CHECK(b.blend   == a.blend);
        CHECK(b.visible == a.visible);
        CHECK(b.clipToBelow     == a.clipToBelow);
        CHECK(b.preserveOpacity == a.preserveOpacity);
        CHECK(b.opacity == doctest::Approx(a.opacity).epsilon(0.005));
        // Ids are reassigned on import, so group membership is checked by
        // position rather than by number.
        CHECK(b.parent.has_value() == a.parent.has_value());
    }

    // Group membership survives, which is the part PSD's begin/end markers are
    // doing all the work for.
    const Layer* group = layerNamed(*loaded, "Group");
    REQUIRE(group != nullptr);
    CHECK(group->kind == LayerKind::Folder);
    for (const char* child : {"Fill", "Tint"})
        CHECK(layerNamed(*loaded, child)->parent == group->id);

    // Pixels: exact, not merely close. Layer data is straight alpha in a PSD
    // exactly as it is in a .sable, so this is the same premultiply round trip
    // the project format already relies on (D-004).
    CHECK(hashCanvas(*loaded) == before);
    CHECK(loaded->path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("the PSD composite section matches flatten()") {
    // Many viewers read only the merged image, so it is not decoration. The
    // copy below has its layer section stripped, which is what those viewers
    // effectively see.
    const Document original = psdSampleDocument();
    const auto path = scratchFile("psd_export_composite.psd");
    REQUIRE(exportDocument(original, path).has_value());

    const auto stripped = flattenedCopyOfPsd(path, "psd_export_merged.psd");
    const auto merged = importDocument(stripped);
    REQUIRE(merged.has_value());
    REQUIRE(merged->layers.size() == 1);

    const std::vector<StraightRgba8> expected = flatten(original);
    const std::vector<StraightRgba8> got      = flatten(*merged);
    REQUIRE(got.size() == expected.size());
    CHECK(got == expected);

    std::filesystem::remove(path);
    std::filesystem::remove(stripped);
}

TEST_CASE("an opaque document background is written as a layer") {
    // PSD has nowhere to put a document background, so it becomes the bottom
    // layer. One more layer than was exported, and identical pixels — the
    // alternative loses the background entirely.
    Document doc = psdSampleDocument();
    doc.background = StraightRgba8{240, 230, 220, 255};
    const std::uint64_t before = hashCanvas(doc);

    const auto path = scratchFile("psd_export_background.psd");
    REQUIRE(exportDocument(doc, path).has_value());
    const auto loaded = importDocument(path);
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->layers.size() == doc.layers.size() + 1);
    CHECK(loaded->layers.front().name == "Background");
    CHECK(loaded->background.a == 0);
    CHECK(hashCanvas(*loaded) == before);
    std::filesystem::remove(path);
}

TEST_CASE("exporting a PSD leaves the document alone, dirty flag included") {
    // US-07.5, the same guarantee PNG export gives.
    Document doc = psdSampleDocument();
    doc.dirty = true;
    const std::uint64_t before = hashCanvas(doc);
    const std::size_t layers = doc.layers.size();

    const auto path = scratchFile("psd_export_untouched.psd");
    REQUIRE(exportDocument(doc, path).has_value());
    CHECK(doc.dirty);
    CHECK(doc.layers.size() == layers);
    CHECK(hashCanvas(doc) == before);
    CHECK(doc.path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("PSD export refuses rather than writing a file no reader accepts") {
    Document doc = psdSampleDocument();
    doc.width = 40000;                       // past PSD's own limit; PSB territory
    const auto path = scratchFile("psd_export_huge.psd");
    const auto written = exportDocument(doc, path);
    REQUIRE(!written.has_value());
    CHECK(written.error().detail.find("30000") != std::string::npos);
    CHECK(!std::filesystem::exists(path));
}

TEST_CASE("the registry offers PSD for export as well as import") {
    const auto exportable = dialogFilters(FormatUse::Write, false);
    CHECK(std::ranges::any_of(exportable, [](const DialogFilter& f) {
        return f.pattern == "psd";
    }));
}

TEST_CASE("recovery never writes over the artist's own file") {
    // D-013, and the one failure mode that destroys work permanently.
    const auto userFile = scratchFile("sable_precious.sable");
    Document doc = sampleDocument();
    REQUIRE(saveProject(doc, userFile).has_value());
    const auto sizeBefore = std::filesystem::file_size(userFile);

    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 255, 0, 255}, 300.0, 200.0);
    const auto written = writeRecovery(doc, userFile);
    REQUIRE(written.has_value());

    CHECK(*written != userFile);
    CHECK(written->string().find(recoveryDirectory().string()) == 0);
    CHECK(std::filesystem::file_size(userFile) == sizeBefore);   // untouched

    const auto recoveries = listRecoveries();
    const auto match = std::ranges::find(recoveries, *written, &RecoveryEntry::recoveryFile);
    REQUIRE(match != recoveries.end());
    CHECK(match->originalPath == userFile);

    // And the recovery itself opens.
    const auto restored = loadProject(*written);
    REQUIRE(restored.has_value());
    CHECK(hashCanvas(*restored) == hashCanvas(doc));

    clearRecovery(*written);
    CHECK(!std::filesystem::exists(*written));
    std::filesystem::remove(userFile);
}

// ---------------------------------------------------- undo budget (D-102)

namespace {

/// One stroke big enough to be worth several tiles of history.
void bigStroke(Document& doc, double y) {
    Layer* layer = doc.active();
    BrushPreset p = defaultPencil();
    p.size = 40.0f;
    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
    for (double x = 10.0; x < doc.width - 10.0; x += 20.0)
        paintSample(s, t, at(x, y), scratch);
    doc.undo.push(std::move(s.pending));
}

}  // namespace

TEST_CASE("undo history stays inside its memory budget") {
    // PRD §12: bounded memory with a stated cap. Unbounded history is what
    // eventually swaps a laptop to death in the middle of a drawing.
    Document doc = makeDocument(1200, 900, StraightRgba8{255, 255, 255, 255});
    doc.undo.setMemoryBudget(4u * 1024u * 1024u);          // deliberately tight

    for (int i = 0; i < 40; ++i) bigStroke(doc, 20.0 + (i % 40) * 20.0);

    CHECK(doc.undo.memoryBytes() <= doc.undo.memoryBudget());
    CHECK(doc.undo.droppedRecords() > 0);        // and it says so
    CHECK(doc.undo.canUndo());                   // without becoming useless
}

TEST_CASE("the budget never evicts the last remaining step") {
    // A stack that empties itself turns Ctrl+Z into a no-op exactly when a big
    // operation has made it most valuable.
    Document doc = makeDocument(2000, 2000, StraightRgba8{255, 255, 255, 255});
    doc.undo.setMemoryBudget(1);                 // floored internally, still tiny

    doc.selection = Selection{0, 0, 2000, 2000};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}));
    const std::uint64_t filled = hashCanvas(doc);

    REQUIRE(doc.undo.canUndo());
    doc.undo.undo(doc);
    CHECK(hashCanvas(doc) != filled);            // the one step still works
}

TEST_CASE("the default budget comfortably holds the 50 steps US-04.3 requires") {
    // The cap and the acceptance criterion have to coexist, and the default is
    // chosen so ordinary work never meets the cap at all.
    Document doc = makeDocument(1024, 1024, StraightRgba8{255, 255, 255, 255});
    CHECK(doc.undo.memoryBudget() == UndoStack::kDefaultBudgetBytes);

    for (int i = 0; i < 60; ++i) bigStroke(doc, 10.0 + i * 16.0);
    CHECK(doc.undo.size() == 60);
    CHECK(doc.undo.droppedRecords() == 0);
    CHECK(doc.undo.memoryBytes() <= doc.undo.memoryBudget());
}

TEST_CASE("eviction drops the oldest history and keeps redo working") {
    Document doc = makeDocument(600, 600, StraightRgba8{255, 255, 255, 255});
    for (int i = 0; i < 6; ++i) bigStroke(doc, 20.0 + i * 40.0);
    const std::uint64_t drawn = hashCanvas(doc);

    doc.undo.undo(doc);
    REQUIRE(doc.undo.canRedo());

    // Tighten the budget to something only a couple of records fit in.
    doc.undo.setMemoryBudget(static_cast<std::size_t>(TILE_BYTES) * 6);
    CHECK(doc.undo.memoryBytes() <= doc.undo.memoryBudget());

    // Redo survives if the undo side alone could satisfy the budget.
    if (doc.undo.canRedo()) {
        doc.undo.redo(doc);
        CHECK(hashCanvas(doc) == drawn);
    }
}

TEST_CASE("clearing the stack resets the dropped count") {
    Document doc = makeDocument(800, 800, StraightRgba8{255, 255, 255, 255});
    doc.undo.setMemoryBudget(2u * 1024u * 1024u);
    for (int i = 0; i < 20; ++i) bigStroke(doc, 20.0 + i * 30.0);
    REQUIRE(doc.undo.droppedRecords() > 0);

    doc.undo.clear();
    CHECK(doc.undo.droppedRecords() == 0);
    CHECK(doc.undo.memoryBytes() == 0);
}

// -------------------------------------------------------- performance

TEST_CASE("a 4000 x 4000 canvas stays cheap while painting in one corner") {
    // PRD §12 and the v1 acceptance list. The guarantee is structural, not a
    // matter of tuning: tile allocation is proportional to PAINTED area, not
    // to canvas area (D-005), so working in one corner of a huge document
    // costs the same as working on a small one.
    Document doc = makeDocument(4000, 4000, StraightRgba8{255, 255, 255, 255});
    CHECK(doc.active()->tiles.empty());          // nothing allocated up front

    Layer* layer = doc.active();
    BrushPreset p = defaultPencil();
    p.size = 24.0f;

    Stroke s;
    std::vector<Dab> scratch;
    scratch.reserve(512);
    beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};

    const std::size_t before = g_allocations;
    for (int i = 0; i < 200; ++i)
        paintSample(s, t, at(40.0 + i * 0.7, 40.0 + std::sin(i * 0.2) * 20.0), scratch);
    const std::size_t allocations = g_allocations - before;

    // A 200-sample stroke inside one corner touches a handful of tiles, and
    // allocation happens only on first touch of each.
    CHECK(layer->tiles.size() <= 4);
    CHECK(allocations < 64);

    // 4000 x 4000 would be 62'500 tiles if it were dense; a fraction of one
    // percent of that is the whole point.
    CHECK(layer->tiles.size() * TILE_BYTES < 2u * 1024u * 1024u);
}

TEST_CASE("undo memory is proportional to what was painted, not to the canvas") {
    // D-102 is still open on the exact cap, but the shape must hold now: a
    // stroke's history costs one snapshot per touched tile, full stop.
    Document doc = makeDocument(4000, 4000, StraightRgba8{255, 255, 255, 255});
    Layer* layer = doc.active();

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
    for (int i = 0; i < 100; ++i) paintSample(s, t, at(50.0 + i, 50.0), scratch);

    const std::size_t touched = s.touched.size();
    doc.undo.push(std::move(s.pending));
    CHECK(doc.undo.memoryBytes() <= touched * TILE_BYTES + 4096);
}

// ------------------------------------------------------------------ export

TEST_CASE("exporting writes a PNG of exactly the canvas size") {
    Document doc = makeDocument(37, 19, StraightRgba8{255, 255, 255, 255});
    Stroke s;
    std::vector<Dab> scratch;
    Layer& layer = *doc.active();
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
    paintSample(s, target, at(18.0, 9.0), scratch);

    const auto path = std::filesystem::temp_directory_path() / "sable_test_export.png";
    std::filesystem::remove(path);

    const auto result = exportPng(doc, path);
    REQUIRE(result.has_value());
    REQUIRE(std::filesystem::exists(path));
    CHECK(std::filesystem::file_size(path) > 0);

    // US-07.5: exporting does not modify the document.
    CHECK(!doc.dirty);
    std::filesystem::remove(path);
}

TEST_CASE("a failed write returns an error rather than terminating") {
    // US-07.7, and the cross-cutting "never lose work" rule: no path that
    // touches a file may terminate the process.
    Document doc = makeDocument(8, 8, StraightRgba8{255, 255, 255, 255});
    const auto result = exportPng(doc, "/definitely/not/a/directory/x.png");
    REQUIRE(!result.has_value());
    CHECK(!result.error().detail.empty());
    CHECK(!describe(result.error().kind).empty());
}

// ------------------------------------------------------------ OpenRaster (#8)

namespace {

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(SABLE_FIXTURE_DIR) / name;
}

const Layer* byName(const Document& doc, std::string_view name) {
    for (const Layer& layer : doc.layers)
        if (layer.name == name) return &layer;
    return nullptr;
}

/// A stack with everything ORA has to carry: a group, a nested child, blend
/// modes on both, fractional opacity and a hidden layer.
Document oraSampleDocument() {
    Document doc = makeDocument(300, 200, StraightRgba8{250, 245, 235, 255});
    doc.layerById(doc.activeLayer)->name = "Base";
    paintSquare(doc, doc.activeLayer, StraightRgba8{200, 30, 40, 255}, 60.0, 60.0);

    doc.undo.push(addLayerAbove(doc, doc.activeLayer, "Group"));
    const LayerId group = doc.activeLayer;
    doc.layerById(group)->kind    = LayerKind::Folder;
    doc.layerById(group)->opacity = 0.7f;
    doc.layerById(group)->blend   = BlendMode::Screen;

    doc.undo.push(addLayerAbove(doc, group, "Hidden"));
    doc.layerById(doc.activeLayer)->visible = false;
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 0, 255}, 250.0, 150.0);

    doc.undo.push(addLayerAbove(doc, group, "Inside"));
    const LayerId inside = doc.activeLayer;
    doc.layerById(inside)->parent  = group;
    doc.layerById(inside)->blend   = BlendMode::ColourDodge;
    doc.layerById(inside)->opacity = 0.35f;
    paintSquare(doc, inside, StraightRgba8{20, 90, 200, 255}, 120.0, 90.0);
    return doc;
}

}  // namespace

TEST_CASE("a document round-trips through .ora") {
    const Document doc = oraSampleDocument();
    const std::uint64_t before = hashCanvas(doc);

    const auto path = scratchFile("roundtrip.ora");
    REQUIRE(exportDocument(doc, path).has_value());

    const auto loaded = importDocument(path);
    REQUIRE(loaded.has_value());
    // An import is a copy of someone else's file, never the document (D-024).
    CHECK(loaded->path.empty());
    CHECK(loaded->width == doc.width);
    CHECK(loaded->height == doc.height);
    CHECK(hashCanvas(*loaded) == before);
    REQUIRE(loaded->layers.size() == doc.layers.size());

    // Compared by name, not by index: ORA nests a folder's children while
    // Sable's vector is flat, so the two orders need not agree for the two
    // stacks to be the same stack.
    for (const Layer& original : doc.layers) {
        const Layer* same = byName(*loaded, original.name);
        REQUIRE(same != nullptr);
        CHECK(same->kind == original.kind);
        CHECK(same->visible == original.visible);
        CHECK(same->blend == original.blend);
        CHECK(same->opacity == doctest::Approx(original.opacity));
    }
    const Layer* group  = byName(*loaded, "Group");
    const Layer* inside = byName(*loaded, "Inside");
    const Layer* hidden = byName(*loaded, "Hidden");
    REQUIRE(group != nullptr);
    REQUIRE(inside != nullptr);
    REQUIRE(hidden != nullptr);
    CHECK(inside->parent == group->id);
    CHECK(!hidden->parent.has_value());

    std::filesystem::remove(path);
}

TEST_CASE("an exported .ora carries a mergedimage that is exactly flatten()") {
    const Document doc = oraSampleDocument();
    const auto path = scratchFile("merged.ora");
    REQUIRE(exportDocument(doc, path).has_value());

    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_file(&zip, path.string().c_str(), 0));
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, "mergedimage.png", &size, 0);
    REQUIRE(data != nullptr);

    std::vector<unsigned char> decoded;
    unsigned w = 0, h = 0;
    const unsigned failed =
        lodepng::decode(decoded, w, h, static_cast<const unsigned char*>(data), size);
    mz_free(data);
    mz_zip_reader_end(&zip);
    REQUIRE(failed == 0);

    CHECK(w == static_cast<unsigned>(doc.width));
    CHECK(h == static_cast<unsigned>(doc.height));
    const std::vector<StraightRgba8> expected = flatten(doc);
    REQUIRE(decoded.size() == expected.size() * 4);
    bool identical = true;
    for (std::size_t i = 0; i < expected.size(); ++i)
        identical = identical && decoded[i * 4 + 0] == expected[i].r &&
                    decoded[i * 4 + 1] == expected[i].g &&
                    decoded[i * 4 + 2] == expected[i].b &&
                    decoded[i * 4 + 3] == expected[i].a;
    CHECK(identical);

    // The spec's other required entries, which are what other applications
    // look for before they parse anything.
    CHECK(hasZipEntry(path, "mimetype"));
    CHECK(hasZipEntry(path, "stack.xml"));
    CHECK(hasZipEntry(path, "Thumbnails/thumbnail.png"));
    std::filesystem::remove(path);
}

TEST_CASE("a .ora written by Krita imports with its stack intact") {
    // tests/fixtures/krita.ora was exported by Krita 6.0.3, not by Sable.
    const auto doc = importDocument(fixture("krita.ora"));
    REQUIRE(doc.has_value());
    CHECK(doc->path.empty());
    CHECK(doc->width == 64);
    CHECK(doc->height == 48);
    // Base, Group, Inside, Top, Hidden, Fade — and the empty "Background"
    // layer Krita puts at the bottom of every new document.
    CHECK(doc->layers.size() == 7);

    const Layer* group  = byName(*doc, "Group");
    const Layer* inside = byName(*doc, "Inside");
    const Layer* top    = byName(*doc, "Top");
    const Layer* fade   = byName(*doc, "Fade");
    const Layer* hidden = byName(*doc, "Hidden");
    const Layer* base   = byName(*doc, "Base");
    REQUIRE(group != nullptr);
    REQUIRE(inside != nullptr);
    REQUIRE(top != nullptr);
    REQUIRE(fade != nullptr);
    REQUIRE(hidden != nullptr);
    REQUIRE(base != nullptr);

    CHECK(group->kind == LayerKind::Folder);
    CHECK(group->opacity == doctest::Approx(0.501961));
    CHECK(inside->parent == group->id);
    CHECK(inside->kind == LayerKind::Raster);
    CHECK(top->blend == BlendMode::Multiply);       // svg:multiply
    CHECK(top->opacity == doctest::Approx(0.6));
    CHECK(hidden->visible == false);

    // Pixels, not only metadata: "Base" is opaque red over the whole canvas,
    // "Inside" an opaque blue square at (8, 8), and "Fade" half-alpha red at
    // (0, 24) — which is where a wrong straight-to-premultiplied conversion
    // would show up.
    const Tile* baseTile = base->find(TileKey{0, 0});
    REQUIRE(baseTile != nullptr);
    CHECK(baseTile->pixel(2, 2) == PremulRgba8{255, 0, 0, 255});

    const Tile* insideTile = inside->find(TileKey{0, 0});
    REQUIRE(insideTile != nullptr);
    CHECK(insideTile->pixel(16, 16) == PremulRgba8{0, 0, 255, 255});
    CHECK(insideTile->pixel(2, 2) == PremulRgba8{0, 0, 0, 0});

    const Tile* fadeTile = fade->find(TileKey{0, 0});
    REQUIRE(fadeTile != nullptr);
    CHECK(fadeTile->pixel(5, 30) == PremulRgba8{128, 0, 0, 128});

    // ORA has no background of its own, so an imported image keeps its
    // transparency rather than gaining a white sheet under it.
    CHECK(doc->background.a == 0);
}

TEST_CASE("a misnamed .ora is still read as one") {
    // .sable, .ora and .kra are all ZIPs; the mimetype and stack.xml entries
    // are what the registry sniffs (D-024).
    const auto path = scratchFile("misnamed_ora.sable");
    std::filesystem::copy_file(fixture("krita.ora"), path);

    const auto doc = importDocument(path);
    REQUIRE(doc.has_value());
    CHECK(doc->width == 64);
    CHECK(doc->path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("a malformed .ora fails with a message rather than a crash") {
    const auto path = scratchFile("broken.ora");
    { FILE* out = std::fopen(path.string().c_str(), "wb");
      REQUIRE(out != nullptr);
      std::fwrite("PK\x03\x04 and then rubbish", 1, 22, out);
      std::fclose(out); }

    const auto doc = importDocument(path);
    REQUIRE(!doc.has_value());
    CHECK(!doc.error().detail.empty());
    std::filesystem::remove(path);
}

// -------------------------------------------------------------- paint backend
// D-021: the CPU path is the default and the reference. What these check is
// the seam itself — that every pixel writer really goes through it, that a
// backend can be swapped in without the callers knowing, and that a failure
// reaches somebody instead of vanishing into a noexcept.

namespace {

/// What #12-#15 will subclass, with a counter where the device would go. It
/// forwards to the reference implementation, which is also how a real GPU
/// backend falls back — CpuBackend never looks at PaintTarget::backend, so
/// forwarding cannot recurse.
class SpyBackend final : public PaintBackend {
public:
    int writes = 0, reads = 0, readbacks = 0;
    bool failNextDab = false;

    std::string_view name() const noexcept override { return "spy"; }

    void applyDab(PaintTarget& t, const Dab& dab) override {
        ++writes;
        if (failNextDab) {
            failNextDab = false;
            recordFailure(Error{ErrorKind::Io, "device lost"});
            return;
        }
        cpuBackend().applyDab(t, dab);
    }
    UndoRecord bucketFill(Document& doc, LayerId target, std::int32_t x, std::int32_t y,
                          StraightRgba8 colour, int tolerance) override {
        ++writes;
        return cpuBackend().bucketFill(doc, target, x, y, colour, tolerance);
    }
    UndoRecord fillSelection(Document& doc, LayerId target, StraightRgba8 c) override {
        ++writes;
        return cpuBackend().fillSelection(doc, target, c);
    }
    UndoRecord transformRegion(Document& doc, LayerId target, const Selection& source,
                               const Transform& transform) override {
        ++writes;
        return cpuBackend().transformRegion(doc, target, source, transform);
    }
    UndoRecord clearLayer(Layer& layer) override {
        ++writes;
        return cpuBackend().clearLayer(layer);
    }
    UndoRecord mergeLayerDown(Document& doc, LayerId id) override {
        ++writes;
        return cpuBackend().mergeLayerDown(doc, id);
    }
    std::vector<PremulRgba8> compositeRect(const Document& doc, std::int32_t x,
                                           std::int32_t y, std::int32_t w,
                                           std::int32_t h) override {
        ++reads;
        return cpuBackend().compositeRect(doc, x, y, w, h);
    }
    StraightRgba8 pickColour(const Document& doc, std::int32_t x,
                             std::int32_t y) override {
        ++reads;
        return cpuBackend().pickColour(doc, x, y);
    }
    std::expected<void, Error> readback(const Document& doc) override {
        ++readbacks;
        return cpuBackend().readback(doc);
    }
};

/// The default backend is process-wide state, so putting it back is not
/// optional — a leaked one would paint the rest of the suite.
struct InstalledBackend {
    explicit InstalledBackend(PaintBackend& b) noexcept { setPaintBackend(&b); }
    ~InstalledBackend() { setPaintBackend(nullptr); }
};

}  // namespace

TEST_CASE("the CPU backend is the default, and it is the one with no device") {
    CHECK(paintBackend().name() == "cpu");
    CHECK(&paintBackend() == &cpuBackend());
    // The reference implementation has nothing to fetch, so a readback on it
    // always succeeds — which is why the headless tests need no device.
    Document doc = makeDocument(8, 8, StraightRgba8{255, 255, 255, 255});
    CHECK(cpuBackend().readback(doc).has_value());
}

TEST_CASE("every pixel writer and reader goes through the backend") {
    SpyBackend spy;
    Document doc = makeDocument(300, 300, StraightRgba8{255, 255, 255, 255});
    const LayerId lower = doc.activeLayer;
    const LayerId upper = doc.addLayer("upper").id;

    std::uint64_t hash = 0;
    {
        const InstalledBackend installed{spy};

        Stroke s;
        std::vector<Dab> scratch;
        Layer& layer = *doc.layerById(upper);
        beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, upper);
        PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height};
        paintSample(s, target, at(40.0, 40.0), scratch);

        (void)bucketFill(doc, lower, 200, 200, StraightRgba8{255, 0, 0, 255}, 0);
        (void)fillSelection(doc, lower, StraightRgba8{0, 255, 0, 255});
        (void)transformRegion(doc, lower, Selection{10, 10, 20, 20}, Transform{.dx = 5.0});
        (void)mergeLayerDown(doc, upper);
        (void)clearLayer(*doc.layerById(lower));
        (void)pickColour(doc, 1, 1);
        hash = hashCanvas(doc);          // flatten -> compositeRect
    }

    CHECK(spy.writes == 6);
    CHECK(spy.reads >= 2);               // pickColour, and flatten's composite
    CHECK(spy.readbacks == 0);           // nothing asked for the pixels back

    // Delegating to the CPU backend must produce exactly the CPU answer: that
    // equality is the whole basis of D-021's "the CPU defines the right one".
    CHECK(paintBackend().name() == "cpu");
    CHECK(hashCanvas(doc) == hash);
}

TEST_CASE("a stroke can name its own backend without moving the default") {
    SpyBackend spy;
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultOpaque(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height, nullptr, &spy};
    paintSample(s, target, at(32.0, 32.0), scratch);

    CHECK(spy.writes > 0);
    CHECK(paintBackend().name() == "cpu");     // the process default never moved
    CHECK(pickColour(doc, 32, 32) == StraightRgba8{0, 0, 0, 255});
}

TEST_CASE("a backend failure reaches the caller instead of vanishing") {
    // The reason applyDab stopped being noexcept. A dab a backend could not
    // draw used to be indistinguishable from one it drew perfectly.
    SpyBackend spy;
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultOpaque(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer, s.pending, s.touched, doc.width, doc.height, nullptr, &spy};

    CHECK(!spy.takeError().has_value());       // nothing has gone wrong yet
    spy.failNextDab = true;
    paintSample(s, target, at(32.0, 32.0), scratch);

    const std::optional<Error> failure = spy.takeError();
    REQUIRE(failure.has_value());
    CHECK(failure->kind == ErrorKind::Io);
    CHECK(!failure->detail.empty());
    CHECK(!spy.takeError().has_value());       // taking it clears it

    // The stroke carries on after a failed dab rather than dying mid-line.
    paintSample(s, target, at(33.0, 33.0), scratch);
    CHECK(!spy.takeError().has_value());
}

// ---------------------------------------------------------- view transform

// The screen<->canvas mapping is the one piece of app code these tests can
// reach, and the one where a mistake is hardest to see: a dropped rotation
// term still looks almost right at ten degrees and paints in the wrong place.

namespace {
constexpr double kPi = 3.14159265358979323846;
double radians(double degrees) { return degrees * kPi / 180.0; }
}  // namespace

TEST_CASE("screen -> canvas -> screen is the identity at any angle") {
    // Negative, past a right angle, past a half turn, and past a full one.
    for (const double degrees : {0.0, 15.0, -37.5, 90.0, 179.9, 200.0, 359.0, -400.0}) {
        const View v{-123.5, 64.25, 2.5f, radians(degrees)};
        for (const double sx : {-40.0, 0.0, 17.0, 1024.0})
            for (const double sy : {-9.0, 0.0, 33.5, 768.0}) {
                const double cx = toCanvasX(v, sx, sy);
                const double cy = toCanvasY(v, sx, sy);
                CHECK(toScreenX(v, cx, cy) == doctest::Approx(sx).epsilon(1e-9));
                CHECK(toScreenY(v, cx, cy) == doctest::Approx(sy).epsilon(1e-9));

                // And the other way round, so neither direction can be the one
                // quietly carrying a sign error.
                const double bx = toScreenX(v, sx, sy);
                const double by = toScreenY(v, sx, sy);
                CHECK(toCanvasX(v, bx, by) == doctest::Approx(sx).epsilon(1e-9));
                CHECK(toCanvasY(v, bx, by) == doctest::Approx(sy).epsilon(1e-9));
            }
    }
}

TEST_CASE("an unrotated view is still a subtract and a divide") {
    // US-05.6 must not regress: the pan/zoom case has to keep landing exactly
    // where it did before rotation existed.
    const View v{100.0, -50.0, 4.0f, 0.0};
    CHECK(toCanvasX(v, 300.0, 0.0) == doctest::Approx(50.0));
    CHECK(toCanvasY(v, 0.0, 30.0) == doctest::Approx(20.0));
    CHECK(toScreenX(v, 50.0, 20.0) == doctest::Approx(300.0));
    CHECK(toScreenY(v, 50.0, 20.0) == doctest::Approx(30.0));
}

TEST_CASE("a quarter turn maps the canvas x axis onto screen y") {
    // Screen y grows downwards, so a positive angle reads as clockwise.
    const View v{0.0, 0.0, 1.0f, radians(90.0)};
    CHECK(toScreenX(v, 10.0, 0.0) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(toScreenY(v, 10.0, 0.0) == doctest::Approx(10.0));
}

TEST_CASE("rotating about a screen point leaves that point's pixel under it") {
    View v{37.0, -11.0, 1.75f, radians(12.0)};
    const double sx = 640.0, sy = 360.0;
    const double cx = toCanvasX(v, sx, sy);
    const double cy = toCanvasY(v, sx, sy);

    for (int step = 0; step < 30; ++step) {     // more than a full turn
        rotateAbout(v, sx, sy, radians(15.0));
        CHECK(toCanvasX(v, sx, sy) == doctest::Approx(cx).epsilon(1e-9));
        CHECK(toCanvasY(v, sx, sy) == doctest::Approx(cy).epsilon(1e-9));
        // Normalised, so the status readout cannot wander off to 450 degrees.
        CHECK(std::abs(v.rotation) <= kPi + 1e-12);
    }

    rotateAbout(v, sx, sy, -v.rotation);        // the reset action
    CHECK(v.rotation == doctest::Approx(0.0));
    CHECK(toCanvasX(v, sx, sy) == doctest::Approx(cx).epsilon(1e-9));
    CHECK(toCanvasY(v, sx, sy) == doctest::Approx(cy).epsilon(1e-9));
}
