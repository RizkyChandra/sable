// Engine unit tests. Headless by design (D-003) — no window, no display server.
//
// These cover the five things USER-STORIES.md names as carrying tests: dab
// spacing, tile compositing, the premultiply round-trip, undo/redo symmetry,
// and (once M2 lands) pressure normalisation.
// Not IMPLEMENT_WITH_MAIN: the GPU device has to be destroyed before
// LeakSanitizer's exit check, and a static destructor runs after it. See the
// main() at the bottom.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <memory>
#include <new>
#include <numbers>
#ifdef _WIN32
#include <malloc.h>   // _aligned_malloc / _aligned_free
#endif
#include <string>
#include <vector>

#include "sbl/backend.hpp"
#include "sbl/canvas.hpp"
#include "sbl/format.hpp"
#include "sbl/gpu.hpp"
#include "sbl/io.hpp"
#include "sbl/linework.hpp"
#include "sbl/paint.hpp"
#include "sbl/project.hpp"
#include "sbl/select.hpp"
#include "sbl/text.hpp"

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

// The nothrow forms have to be replaced too, now that the engine may link
// SDL3 and a GPU driver (D-022/D-025). Replacing only the throwing ones leaves
// a library that calls `new (std::nothrow)` allocating through the runtime's
// allocator and freeing through ours, which AddressSanitizer correctly reports
// as an alloc/dealloc mismatch — in Mesa's shader compiler, of all places.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    ++g_allocations;
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    ++g_allocations;
    const std::size_t align = static_cast<std::size_t>(a);
#ifdef _WIN32
    return _aligned_malloc(((n + align - 1) / align) * align, align);
#else
    return std::aligned_alloc(align, ((n + align - 1) / align) * align);
#endif
}
void* operator new[](std::size_t n, std::align_val_t a,
                     const std::nothrow_t& t) noexcept {
    return ::operator new(n, a, t);
}

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
    CHECK(narrow(b.pixel(5, 5)) == PremulRgba8{10, 20, 30, 40});
    b.setPixel(5, 5, PremulRgba8{});
    CHECK(narrow(a.pixel(5, 5)) == PremulRgba8{10, 20, 30, 40});
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
    CHECK(narrow(tile->pixel(2, 2)).a > 0);
    // Nothing painted beyond the canvas bound.
    CHECK(narrow(tile->pixel(63, 63)).a == 0);
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
    CHECK(narrow(tile->pixel(32, 32)).unpremultiply() == chosen);
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
    CHECK(narrow(copyTile->pixel(200, 200)).a > 0);
    CHECK(narrow(originalTile->pixel(200, 200)).a == 0);
    CHECK(narrow(originalTile->pixel(60, 60)).a > 0);      // and it kept its own paint
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
    CHECK(rec.memoryBytes() < static_cast<std::size_t>(tileBytes(ColourDepth::Bits8)));
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

// ------------------------------------------------------ gradient (#49)

namespace {

/// A horizontal linear gradient with the exact ramp — dithering off, so the
/// values a test reads are the ones the interpolation produced and not the
/// ones a threshold matrix nudged. D-030.
Gradient horizontalRamp(double x0, double x1, StraightRgba8 from, StraightRgba8 to) {
    Gradient g;
    g.x0 = x0;
    g.x1 = x1;
    g.from = from;
    g.to = to;
    g.dither = false;
    return g;
}

}  // namespace

TEST_CASE("a linear gradient ramps monotonically and holds both ends") {
    Document doc = makeDocument(256, 4, StraightRgba8{255, 255, 255, 255});
    const Gradient g = horizontalRamp(64.0, 192.0, StraightRgba8{0, 0, 0, 255},
                                      StraightRgba8{255, 255, 255, 255});
    doc.undo.push(gradientFill(doc, doc.activeLayer, g));

    // Beyond either end of the axis the ramp holds that end's colour exactly:
    // no overshoot past black or past white, which is what an unclamped
    // projection would produce off the ends of a short drag.
    CHECK(pickColour(doc, 0, 2)   == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 63, 2)  == StraightRgba8{0, 0, 0, 255});
    CHECK(pickColour(doc, 192, 2) == StraightRgba8{255, 255, 255, 255});
    CHECK(pickColour(doc, 255, 2) == StraightRgba8{255, 255, 255, 255});

    int previous = -1;
    bool monotonic = true;
    for (std::int32_t x = 0; x < 256; ++x) {
        const StraightRgba8 c = pickColour(doc, x, 2);
        if (c.r < previous) monotonic = false;
        previous = c.r;
        CHECK(c.g == c.r);          // a grey ramp stays grey
        CHECK(c.b == c.r);
    }
    CHECK(monotonic);
    CHECK(pickColour(doc, 128, 2).r == doctest::Approx(128).epsilon(0.02));
}

TEST_CASE("a gradient to transparent fades without a grey haze") {
    // THE failure this feature exists to get right. Interpolating the two ends
    // on straight alpha drags the colour channels toward the transparent end's
    // zero alongside the alpha, and every pixel of the fade comes out darker
    // than the colour the artist picked. Premultiplied, the red stays red and
    // only the alpha moves.
    //
    // A transparent BLACK far end on purpose: that is what "fade to nothing"
    // means to the app, and a `to` that carried the foreground's own RGB would
    // hide the bug rather than test it.
    Document doc = makeDocument(256, 4, StraightRgba8{0, 0, 0, 0});
    doc.undo.push(gradientFill(
        doc, doc.activeLayer,
        horizontalRamp(0.0, 256.0, StraightRgba8{255, 0, 0, 255},
                       StraightRgba8{0, 0, 0, 0})));

    int previousAlpha = 256;
    for (std::int32_t x = 0; x < 256; ++x) {
        const StraightRgba8 c = pickColour(doc, x, 2);
        if (c.a == 0) continue;              // the far end, fully faded out
        CHECK(c.r == 255);                   // still the foreground, not a dulled one
        CHECK(c.g == 0);
        CHECK(c.b == 0);
        CHECK(c.a <= previousAlpha);         // and the fade only ever thins
        previousAlpha = c.a;
    }
    CHECK(previousAlpha < 8);                // it does reach the far end
}

TEST_CASE("a gradient to transparent does not darken the art beneath it") {
    // The same failure seen from the other side: composited over white, a
    // half-faded pure red must stay at full red. Straight-alpha interpolation
    // reads as a grey wash lying over the fade.
    Document doc = makeDocument(256, 4, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{255, 255, 255, 255}));
    doc.undo.push(gradientFill(
        doc, doc.activeLayer,
        horizontalRamp(0.0, 256.0, StraightRgba8{255, 0, 0, 255},
                       StraightRgba8{0, 0, 0, 0})));

    for (std::int32_t x = 0; x < 256; ++x) {
        const StraightRgba8 c = pickColour(doc, x, 2);
        CHECK(c.r == 255);
        CHECK(c.g == c.b);                   // no colour cast either way
    }
}

TEST_CASE("a radial gradient runs outward from its centre") {
    Document doc = makeDocument(200, 200, StraightRgba8{255, 255, 255, 255});
    Gradient g;
    g.shape = GradientShape::Radial;
    // The centre of pixel (100, 100), not its corner: distances are measured
    // from pixel centres, so a centre on a corner is half a pixel nearer one
    // side than the other and the symmetry checks below would be off by one.
    g.x0 = 100.5;
    g.y0 = 100.5;
    g.x1 = 150.5;                            // radius 50
    g.y1 = 100.5;
    g.from = StraightRgba8{0, 0, 0, 255};
    g.to   = StraightRgba8{255, 255, 255, 255};
    g.dither = false;
    doc.undo.push(gradientFill(doc, doc.activeLayer, g));

    CHECK(pickColour(doc, 100, 100).r == 0);            // the centre
    // Equal distances in any direction give equal colour — the thing a linear
    // gradient with the same endpoints would get wrong.
    const int right = pickColour(doc, 125, 100).r;
    CHECK(pickColour(doc, 75, 100).r  == right);
    CHECK(pickColour(doc, 100, 125).r == right);
    CHECK(right == doctest::Approx(128).epsilon(0.03));
    CHECK(pickColour(doc, 0, 0)     == StraightRgba8{255, 255, 255, 255});  // past it
    CHECK(pickColour(doc, 100, 160) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("a gradient lands only inside the selection") {
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
    doc.selection = Selection{32, 32, 64, 64};
    doc.undo.push(gradientFill(
        doc, doc.activeLayer,
        horizontalRamp(0.0, 128.0, StraightRgba8{0, 0, 0, 255},
                       StraightRgba8{0, 0, 255, 255})));

    CHECK(pickColour(doc, 31, 64) == StraightRgba8{255, 255, 255, 255});
    CHECK(pickColour(doc, 96, 64) == StraightRgba8{255, 255, 255, 255});
    CHECK(pickColour(doc, 64, 31) == StraightRgba8{255, 255, 255, 255});
    CHECK(pickColour(doc, 64, 96) == StraightRgba8{255, 255, 255, 255});
    CHECK(pickColour(doc, 64, 64).b > 0);
    CHECK(pickColour(doc, 64, 64).r == 0);
}

TEST_CASE("a gradient is one undo step and restores exactly") {
    Document doc = makeDocument(160, 160, StraightRgba8{255, 255, 255, 255});
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{0, 90, 200, 255}));
    const std::uint64_t before = hashCanvas(doc);

    doc.undo.push(gradientFill(
        doc, doc.activeLayer,
        horizontalRamp(20.0, 140.0, StraightRgba8{255, 255, 0, 255},
                       StraightRgba8{0, 0, 0, 0})));
    CHECK(doc.undo.size() == 2);
    const std::uint64_t after = hashCanvas(doc);
    CHECK(after != before);

    doc.undo.undo(doc);
    CHECK(hashCanvas(doc) == before);
    doc.undo.redo(doc);
    CHECK(hashCanvas(doc) == after);
}

TEST_CASE("a gradient obeys locked, preserveOpacity and a zero-length axis") {
    Document doc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    const Gradient g = horizontalRamp(0.0, 64.0, StraightRgba8{255, 0, 0, 255},
                                      StraightRgba8{0, 0, 255, 255});

    doc.active()->locked = true;
    CHECK(gradientFill(doc, doc.activeLayer, g).empty());
    CHECK(doc.active()->tiles.empty());
    doc.active()->locked = false;

    // No drag, no axis, no undo step to clear up afterwards.
    CHECK(gradientFill(doc, doc.activeLayer,
                       horizontalRamp(10.0, 10.0, g.from, g.to)).empty());

    // preserveOpacity: paint only where there is already paint.
    doc.selection = Selection{0, 0, 32, 64};
    doc.undo.push(fillSelection(doc, doc.activeLayer, StraightRgba8{255, 255, 255, 255}));
    doc.selection.reset();
    doc.active()->preserveOpacity = true;
    doc.undo.push(gradientFill(doc, doc.activeLayer, g));

    CHECK(pickColour(doc, 16, 32).r < 255);                 // painted half: gradient
    const Tile* tile = doc.active()->find(TileKey{0, 0});
    REQUIRE(tile != nullptr);
    CHECK(tile->pixel(40, 32) == PremulRgba16{});           // empty half: still empty
}

TEST_CASE("dithering breaks the bands of a shallow 8-bit ramp") {
    // 256 pixels across five levels: without dither the row is five flat bands
    // with four steps in it, and on a real canvas those edges are visible.
    // Counting the steps is the blunt way to say "the bands were broken up".
    const auto stepsAlongRow = [](bool dither) {
        Document doc = makeDocument(256, 8, StraightRgba8{0, 0, 0, 0});
        Gradient g = horizontalRamp(0.0, 256.0, StraightRgba8{0, 0, 0, 255},
                                    StraightRgba8{4, 4, 4, 255});
        g.dither = dither;
        doc.undo.push(gradientFill(doc, doc.activeLayer, g));

        int steps = 0;
        for (std::int32_t x = 1; x < 256; ++x)
            if (pickColour(doc, x, 3).r != pickColour(doc, x - 1, 3).r) ++steps;
        return steps;
    };
    CHECK(stepsAlongRow(false) == 4);
    CHECK(stepsAlongRow(true) > 20);
}

TEST_CASE("dither leaves a colour that needs no dithering alone") {
    // The other half of D-030: a dither that reaches a whole 8-bit step rather
    // than half of one also wobbles values that were exactly representable,
    // speckling the flat ends of every ramp and every solid colour under one.
    Document doc = makeDocument(64, 64, StraightRgba8{0, 0, 0, 0});
    Gradient g = horizontalRamp(0.0, 64.0, StraightRgba8{200, 30, 40, 255},
                                StraightRgba8{200, 30, 40, 255});
    g.dither = true;
    doc.undo.push(gradientFill(doc, doc.activeLayer, g));

    for (std::int32_t y = 0; y < 8; ++y)
        for (std::int32_t x = 0; x < 8; ++x)
            CHECK(pickColour(doc, x, y) == StraightRgba8{200, 30, 40, 255});
}

TEST_CASE("a gradient on a 16-bit document keeps the levels between 8-bit ones") {
    Document doc = makeDocument(256, 4, StraightRgba8{0, 0, 0, 0}, ColourDepth::Bits16);
    doc.undo.push(gradientFill(
        doc, doc.activeLayer,
        horizontalRamp(0.0, 256.0, StraightRgba8{0, 0, 0, 255},
                       StraightRgba8{4, 4, 4, 255})));

    // 4 levels at 8 bits, 1028 at 16: the ramp must not have been quantised on
    // its way through, which is what makes the extra storage worth its memory.
    const Tile* tile = doc.active()->find(TileKey{0, 0});
    REQUIRE(tile != nullptr);
    int distinct = 1;
    for (std::int32_t x = 1; x < 256; ++x)
        if (tile->pixel(x, 2).r != tile->pixel(x - 1, 2).r) ++distinct;
    CHECK(distinct > 200);
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
    for (const int version : {1, 2, 3, 4}) {
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
    // No layer in this file has a mask, so the mask warning (#40) must stay
    // quiet — a channel that fires on every PSD is one nobody reads.
    CHECK(doc->warnings.empty());

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

TEST_CASE("a PSD's layer masks reach the canvas (#35) and stay masks (#48)") {
    // The bug was silence: the pixels arrived and the mask that hid half of
    // them did not, so the import showed content the file does not. #35 fixed
    // the picture by multiplying the mask into the alpha; #48 gives the mask
    // somewhere to live, so the import is now faithful in BOTH — the drawing
    // matches the file and the mask is still a mask afterwards.
    //
    // masked_flat.psd carries the same artwork composited in floating point by
    // the fixture script, so this compares against another implementation of
    // the masking rules rather than against Sable's.
    const auto masked = importDocument(testData("masked.psd"));
    const auto truth  = importDocument(testData("masked_flat.psd"));
    REQUIRE(masked.has_value());
    REQUIRE(truth.has_value());
    REQUIRE(masked->layers.size() == 4);

    const std::vector<StraightRgba8> ours   = flatten(*masked);
    const std::vector<StraightRgba8> theirs = flatten(*truth);
    REQUIRE(ours.size() == theirs.size());

    int worst = 0;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        worst = std::max({worst,
            std::abs(ours[i].r - theirs[i].r), std::abs(ours[i].g - theirs[i].g),
            std::abs(ours[i].b - theirs[i].b), std::abs(ours[i].a - theirs[i].a)});
    }
    CHECK(worst <= 2);

    // The layer whose mask rectangle is smaller than the layer: everything
    // outside it takes the mask's default colour, which is zero here. The
    // PIXELS survive untouched — that is the whole of #48 in one assertion,
    // because this is exactly what D-027 destroyed.
    const Layer* patch = layerNamed(*masked, "Patch");
    REQUIRE(patch != nullptr);
    REQUIRE(patch->mask.has_value());
    CHECK(patch->mask->outside == 0);
    CHECK(pickColour(*masked, 10, 10).a == 255);      // inside the mask rectangle
    CHECK(patch->find(TileKey{0, 0}) != nullptr);
    for (const auto& [key, tile] : patch->tiles)
        for (int y = 0; y < TILE_SIZE; ++y)
            for (int x = 0; x < TILE_SIZE; ++x) {
                const std::int32_t cx = key.first  * TILE_SIZE + x;
                const std::int32_t cy = key.second * TILE_SIZE + y;
                const bool masked_off = cx >= 32 || cy >= 24 || cx < 8 || cy < 8;
                // Painted, and hidden by the mask rather than by its own alpha.
                if (masked_off && cx >= 8 && cy >= 8 && cx < 56 && cy < 40) {
                    REQUIRE(tile.pixel(x, y).a == 65535);
                    REQUIRE(maskCoverage(*patch->mask, cx, cy) == 0);
                }
            }

    // Partial coverage, which is what a mask is for and what an alpha of zero
    // could never have carried back out to a PSD.
    const Layer* bands = layerNamed(*masked, "Bands");
    REQUIRE(bands != nullptr);
    REQUIRE(bands->mask.has_value());
    CHECK(maskCoverage(*bands->mask, 10, 10) == 255);
    CHECK(maskCoverage(*bands->mask, 30, 10) == 128);
    CHECK(maskCoverage(*bands->mask, 50, 10) == 0);

    // A disabled mask is not a mask. This one is all zeroes, so applying it
    // anyway would delete the layer outright — and it is KEPT, switched off,
    // because that is what the file says and the artist may want it back.
    const Layer* unmasked = layerNamed(*masked, "Unmasked");
    REQUIRE(unmasked != nullptr);
    CHECK(!unmasked->tiles.empty());
    REQUIRE(unmasked->mask.has_value());
    CHECK(!unmasked->mask->enabled);
    CHECK(pickColour(*masked, 50, 40) == StraightRgba8{255, 255, 0, 255});

    // Nothing was given up, so there is nothing to warn about. #35's notice —
    // "the masks can no longer be edited" — is not true of this reader, and a
    // warning that is not true is worse than none.
    CHECK(masked->warnings.empty());
}

TEST_CASE("a PSD's layer masks survive a round trip through PSD (#48)") {
    // The other half of the acceptance criterion: imported with the mask
    // intact, and re-exported WITH it. Baking made this impossible by
    // construction — a baked mask leaves nothing to write to channel -2.
    const auto masked = importDocument(testData("masked.psd"));
    REQUIRE(masked.has_value());

    const auto path = scratchFile("mask_roundtrip.psd");
    REQUIRE(exportDocument(*masked, path).has_value());
    const auto back = importDocument(path);
    REQUIRE(back.has_value());

    const Layer* bands = layerNamed(*back, "Bands");
    REQUIRE(bands != nullptr);
    REQUIRE(bands->mask.has_value());
    CHECK(maskCoverage(*bands->mask, 10, 10) == 255);
    CHECK(maskCoverage(*bands->mask, 30, 10) == 128);
    CHECK(maskCoverage(*bands->mask, 50, 10) == 0);

    const Layer* unmasked = layerNamed(*back, "Unmasked");
    REQUIRE(unmasked != nullptr);
    REQUIRE(unmasked->mask.has_value());
    CHECK(!unmasked->mask->enabled);        // the disabled flag travels too

    // And the picture is the same one, which is what says the mask went out
    // as a mask rather than as a differently-shaped alpha.
    const std::vector<StraightRgba8> before = flatten(*masked);
    const std::vector<StraightRgba8> after  = flatten(*back);
    REQUIRE(before.size() == after.size());
    int worst = 0;
    for (std::size_t i = 0; i < before.size(); ++i)
        worst = std::max({worst, std::abs(before[i].r - after[i].r),
                          std::abs(before[i].g - after[i].g),
                          std::abs(before[i].b - after[i].b),
                          std::abs(before[i].a - after[i].a)});
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
    doc.undo.setMemoryBudget(static_cast<std::size_t>(tileBytes(ColourDepth::Bits8)) * 6);
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
    CHECK(layer->tiles.size() * tileBytes(ColourDepth::Bits8) < 2u * 1024u * 1024u);
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
    CHECK(doc.undo.memoryBytes() <= touched * tileBytes(ColourDepth::Bits8) + 4096);
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
    CHECK(narrow(baseTile->pixel(2, 2)) == PremulRgba8{255, 0, 0, 255});

    const Tile* insideTile = inside->find(TileKey{0, 0});
    REQUIRE(insideTile != nullptr);
    CHECK(narrow(insideTile->pixel(16, 16)) == PremulRgba8{0, 0, 255, 255});
    CHECK(narrow(insideTile->pixel(2, 2)) == PremulRgba8{0, 0, 0, 0});

    const Tile* fadeTile = fade->find(TileKey{0, 0});
    REQUIRE(fadeTile != nullptr);
    CHECK(narrow(fadeTile->pixel(5, 30)) == PremulRgba8{128, 0, 0, 128});

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

// ------------------------------------------------------------------ Krita (#9)

TEST_CASE("a .kra written by Krita imports with its layer stack intact") {
    // tests/fixtures/krita.kra was saved by Krita 6.0.3. It holds, bottom to
    // top: an empty Background, an opaque red Base, a half-opacity Group with
    // a blue square inside it, a multiply layer at 60%, a hidden layer, a
    // half-alpha layer, and an invert FILTER layer that Sable cannot read.
    const auto doc = importDocument(fixture("krita.kra"));
    REQUIRE(doc.has_value());
    CHECK(doc->path.empty());          // never the artist's file (D-024)
    CHECK(doc->width == 64);
    CHECK(doc->height == 48);
    CHECK(doc->dpi == 72);

    // Seven layers arrived; the filter layer was skipped rather than sinking
    // the load.
    CHECK(doc->layers.size() == 7);
    CHECK(byName(*doc, "Invert") == nullptr);

    // Krita's layers are unbounded, and the tiles it stores are only what
    // differs from the layer's default pixel. Krita's own Background layer
    // stores NO tiles at all and is opaque white purely by that default, so
    // ignoring it would quietly drop the white background out of every
    // document an artist brings over.
    const Layer* background = byName(*doc, "Background");
    REQUIRE(background != nullptr);
    const Tile* backgroundTile = background->find(TileKey{0, 0});
    REQUIRE(backgroundTile != nullptr);
    CHECK(narrow(backgroundTile->pixel(40, 40)) == PremulRgba8{255, 255, 255, 255});

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
    CHECK(group->opacity == doctest::Approx(128.0 / 255.0));
    CHECK(inside->parent == group->id);
    CHECK(!base->parent.has_value());
    CHECK(top->blend == BlendMode::Multiply);
    CHECK(top->opacity == doctest::Approx(153.0 / 255.0));
    CHECK(hidden->visible == false);

    // The pixels, which is what the tiled, LZF-compressed, plane-separated
    // layer data is for. Krita stores B, G, R, A planes with straight alpha;
    // any of those three read wrongly and these three checks fail.
    const Tile* baseTile = base->find(TileKey{0, 0});
    REQUIRE(baseTile != nullptr);
    CHECK(narrow(baseTile->pixel(2, 2)) == PremulRgba8{255, 0, 0, 255});

    const Tile* insideTile = inside->find(TileKey{0, 0});
    REQUIRE(insideTile != nullptr);
    CHECK(narrow(insideTile->pixel(16, 16)) == PremulRgba8{0, 0, 255, 255});
    CHECK(narrow(insideTile->pixel(2, 2)) == PremulRgba8{0, 0, 0, 0});

    const Tile* fadeTile = fade->find(TileKey{0, 0});
    REQUIRE(fadeTile != nullptr);
    CHECK(narrow(fadeTile->pixel(5, 30)) == PremulRgba8{128, 0, 0, 128});

    // The same document exported to ORA by Krita must import to the same
    // picture — two readers, two containers, one answer.
    const auto viaOra = importDocument(fixture("krita.ora"));
    REQUIRE(viaOra.has_value());
    CHECK(hashCanvas(*doc) == hashCanvas(*viaOra));
}

TEST_CASE("an import that drops a layer says so on the document (#40)") {
    // The failure this guards against is silence, not the dropped layer: an
    // artist who opens a .kra with an invert filter in it gets a document that
    // does not look like their file, and stderr is nowhere they will read.
    const auto doc = importDocument(fixture("krita.kra"));
    REQUIRE(doc.has_value());
    REQUIRE(!doc->warnings.empty());

    const bool namesTheLayer =
        std::ranges::any_of(doc->warnings, [](const std::string& warning) {
            return warning.find("Invert") != std::string::npos;
        });
    CHECK(namesTheLayer);       // which layer, not just "something was dropped"

    // Carried by the clone the autosave worker gets. `Document` is deep-copied
    // there field by field, so anything new is dropped by default (the rulers
    // and the selection both landed in that trap first).
    CHECK(cloneDocument(*doc).warnings == doc->warnings);

    // And a file with nothing to apologise for says nothing: a channel that
    // cried wolf on every import would be ignored on the one that mattered.
    const auto whole = importDocument(fixture("krita.ora"));
    REQUIRE(whole.has_value());
    CHECK(whole->warnings.empty());
}

TEST_CASE("a misnamed .kra is still read as one") {
    const auto path = scratchFile("misnamed_kra.sable");
    std::filesystem::copy_file(fixture("krita.kra"), path);

    const auto doc = importDocument(path);
    REQUIRE(doc.has_value());
    CHECK(doc->width == 64);
    CHECK(doc->path.empty());
    std::filesystem::remove(path);
}

TEST_CASE("a malformed .kra fails with a message rather than a crash") {
    const auto path = scratchFile("broken.kra");
    { FILE* out = std::fopen(path.string().c_str(), "wb");
      REQUIRE(out != nullptr);
      std::fwrite("PK\x03\x04 not really a Krita document", 1, 33, out);
      std::fclose(out); }

    const auto doc = importDocument(path);
    REQUIRE(!doc.has_value());
    CHECK(!doc.error().detail.empty());
    std::filesystem::remove(path);

    // Sable writes no .kra: reading someone else's format is interop, writing
    // it is a promise about fidelity we have not made.
    const auto written = exportDocument(oraSampleDocument(), scratchFile("nope.kra"));
    REQUIRE(!written.has_value());
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

// ------------------------------------------------------------- the GPU backend
//
// Every case here starts by asking for a backend and giving up quietly if
// there is not one — that is the normal answer in the `engine` CI job, which
// has no SDL3 and no graphics stack, and on any machine without a SPIR-V
// device (D-025). "No GPU" is not a failure; a GPU that disagrees with the CPU
// is (D-021).

namespace {

/// One device for the whole run. Creating a GPU device is expensive and these
/// cases would otherwise pay for it a dozen times.
std::unique_ptr<PaintBackend> g_gpu;
bool g_gpuTried = false;
bool g_gpuUsed  = false;

PaintBackend* gpuForTests() {
    if (!g_gpuTried) {
        g_gpuTried = true;
        std::string why;
        g_gpu = makeGpuBackend(&why);
        g_gpuUsed = g_gpu != nullptr;
        MESSAGE("GPU backend: " << why);
    }
    return g_gpu.get();
}

/// Installs a backend as the process default for a scope, which is what the
/// application's toggle does.
///
/// `PaintTarget::backend` steers the dab path only. Undo, saving and the layer
/// operations all follow the process default, so a case about any of those has
/// to set it — and a case that does not is testing a configuration nothing
/// ships in.
struct WithBackend {
    explicit WithBackend(PaintBackend* backend) { setPaintBackend(backend); }
    ~WithBackend() { setPaintBackend(nullptr); }
    WithBackend(const WithBackend&) = delete;
    WithBackend& operator=(const WithBackend&) = delete;
};

/// The biggest per-channel difference between two premultiplied buffers, and
/// how many pixels differ at all. D-021 wants the GPU checked against the CPU
/// pixel for pixel rather than by eye, and a single number hides which of the
/// two failure shapes it is: a handful of one-off edge pixels is rounding, a
/// large fraction differing is a wrong formula.
struct Divergence {
    int         maxChannel = 0;
    std::size_t pixels     = 0;
};

Divergence compare(const std::vector<PremulRgba8>& a, const std::vector<PremulRgba8>& b) {
    Divergence d;
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int dr = std::abs(int{a[i].r} - int{b[i].r});
        const int dg = std::abs(int{a[i].g} - int{b[i].g});
        const int db = std::abs(int{a[i].b} - int{b[i].b});
        const int da = std::abs(int{a[i].a} - int{b[i].a});
        const int worst = std::max({dr, dg, db, da});
        if (worst > 0) ++d.pixels;
        d.maxChannel = std::max(d.maxChannel, worst);
    }
    return d;
}

/// A document that exercises everything `compositeLevel` can do: every blend
/// mode, fractional opacity, a clipping group, and a folder.
Document layeredDocument(std::int32_t size, int rasterLayers) {
    Document doc = makeDocument(size, size, StraightRgba8{240, 230, 220, 255});
    std::uint32_t seed = 12345;
    const auto rnd = [&seed](std::uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 16) % n;
    };

    for (int i = 0; i < rasterLayers; ++i) {
        Layer& layer = doc.addLayer("layer " + std::to_string(i));
        layer.blend   = ALL_BLEND_MODES[static_cast<std::size_t>(i) % ALL_BLEND_MODES.size()];
        layer.opacity = 0.35f + static_cast<float>(i % 5) * 0.15f;
        // Every third layer clips to the one below it, and one is hidden.
        layer.clipToBelow = (i % 3) == 2;
        layer.visible     = i != 4;

        // Sparse on purpose: a layer with no tile at a key still has to
        // publish a zero clip mask, which is the easiest thing to get wrong.
        for (std::int32_t ty = 0; ty <= tileIndex(size - 1); ++ty) {
            for (std::int32_t tx = 0; tx <= tileIndex(size - 1); ++tx) {
                if (rnd(4) == 0) continue;
                Tile& tile = layer.tileFor(TileKey{tx, ty});
                PremulRgba8* px = tile.pixels8();
                for (int p = 0; p < TILE_PIXELS; ++p) {
                    const auto a = static_cast<std::uint8_t>(rnd(256));
                    px[p] = StraightRgba8{static_cast<std::uint8_t>(rnd(256)),
                                          static_cast<std::uint8_t>(rnd(256)),
                                          static_cast<std::uint8_t>(rnd(256)),
                                          a}
                                .premultiply();
                }
            }
        }
    }
    return doc;
}

}  // namespace

TEST_CASE("the GPU composites what the CPU composites") {
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    Document doc = layeredDocument(512, 8);

    // A folder, holding the top two layers, with its own blend and opacity —
    // the recursive case, which the shader replays off a stack.
    Layer& folder = doc.addLayer("group");
    folder.kind    = LayerKind::Folder;
    folder.blend   = BlendMode::Multiply;
    folder.opacity = 0.7f;
    const LayerId folderId = folder.id;
    for (std::size_t i = doc.layers.size() - 3; i + 1 < doc.layers.size(); ++i)
        doc.layers[i].parent = folderId;

    const auto cpu = cpuBackend().compositeRect(doc, 0, 0, doc.width, doc.height);
    const auto dev = gpu->compositeRect(doc, 0, 0, doc.width, doc.height);
    const Divergence d = compare(cpu, dev);

    MESSAGE("composite divergence: max channel " << d.maxChannel << ", "
            << d.pixels << " of " << cpu.size() << " pixels differ");
    // One level is invisible on screen and obvious in a diff, which D-021 says
    // is the wrong way round for a mistake to behave — so it is bounded, not
    // waved through. Anything above this is a formula that disagrees.
    CHECK(d.maxChannel <= 1);
}

TEST_CASE("every blend mode agrees on its own") {
    // One layer at a time, so a wrong formula cannot hide behind the
    // accumulated rounding of a tall stack. This is the case that pins each
    // W3C function down; the stacked ones below bound the drift.
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    for (const BlendMode mode : ALL_BLEND_MODES) {
        Document doc = layeredDocument(256, 1);
        doc.layers.back().blend       = mode;
        doc.layers.back().opacity     = 0.8f;
        doc.layers.back().clipToBelow = false;
        doc.layers.back().visible     = true;
        const Divergence d = compare(cpuBackend().compositeRect(doc, 0, 0, 256, 256),
                                     gpu->compositeRect(doc, 0, 0, 256, 256));
        INFO("blend mode ", blendModeName(mode));
        CHECK(d.maxChannel <= 1);
    }
}

TEST_CASE("a document too big for the arena still composites correctly") {
    // 12 layers of 2048 x 2048 is around 768 tiles against 512 slots, so this
    // runs the eviction path and the chunk sizing that stops a chunk from
    // evicting its own inputs. Getting either wrong puts a whole 65'536-pixel
    // tile of the wrong layer on screen, so the check is on the RATE of
    // disagreement rather than its size: a fraction of a percent is the
    // rounding drift below, and anything structural is orders above it.
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    // The stated exception to the one-level bound, and the reason it is not a
    // widened bound: #14's harness holds the GPU to +-1 and it passes there,
    // because no scenario in it stacks more than three blend levels. Twelve
    // levels of dodge, burn and soft light do exceed one level, and that is
    // ACCUMULATION, not a wrong formula:
    //
    //   * every mode on its own is within one level — asserted just above, in
    //     "every blend mode agrees on its own";
    //   * each level unpremultiplies to 8 bits, blends in float and rounds
    //     back, so a one-level difference at the bottom is an *input* to the
    //     next level, and Colour Dodge and Colour Burn amplify their inputs by
    //     construction;
    //   * the two float evaluations are independent by D-021's own design
    //     ("two implementations of the same blend maths, which will drift"),
    //     and GCC contracts the reference's three terms into FMAs while the
    //     shader is `precise`, so the last bit cannot be made to agree.
    //
    // So the ceiling is named rather than removed, and it fails if it grows.
    constexpr int kDeepStackCeiling = 8;

    struct Result { double rate; int worst; };
    const auto measure = [gpu](std::int32_t size) {
        Document doc = layeredDocument(size, 12);
        const Divergence d = compare(cpuBackend().compositeRect(doc, 0, 0, size, size),
                                     gpu->compositeRect(doc, 0, 0, size, size));
        MESSAGE("12 layers at " << size << "x" << size << ": max channel "
                << d.maxChannel << ", " << d.pixels << " of "
                << static_cast<std::size_t>(size) * size << " pixels differ");
        return Result{static_cast<double>(d.pixels) /
                          (static_cast<double>(size) * static_cast<double>(size)),
                      d.maxChannel};
    };

    const Result fits = measure(1024);       // 192 tiles: inside the arena
    const Result spills = measure(2048);     // 768 tiles: eviction and re-upload
    CHECK(fits.worst <= kDeepStackCeiling);
    CHECK(spills.worst <= kDeepStackCeiling);

    // And the arena check proper. Getting eviction or the chunk sizing wrong
    // puts a whole 65'536-pixel tile of the wrong layer on screen, so this is
    // on the RATE: the same picture at four times the area should drift in the
    // same proportion, and a tile served from the wrong slot would not be
    // subtle about it.
    CHECK(fits.rate < 0.001);
    CHECK(spills.rate < std::max(fits.rate * 3.0, 0.001));
}

TEST_CASE("the GPU composites an off-canvas rectangle the way the CPU does") {
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    Document doc = layeredDocument(300, 3);
    // Straddling two edges: the padding outside the canvas must stay
    // transparent rather than picking up the background.
    for (const auto& [x, y, w, h] : std::vector<std::array<std::int32_t, 4>>{
             {{-64, -64, 256, 256}}, {{200, 200, 256, 256}}, {{0, 0, 1, 1}}}) {
        const Divergence d = compare(cpuBackend().compositeRect(doc, x, y, w, h),
                                     gpu->compositeRect(doc, x, y, w, h));
        CHECK(d.maxChannel <= 1);
    }
}

TEST_CASE("the GPU picks the colour its own compositor shows") {
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    Document doc = layeredDocument(300, 4);
    const auto composited = gpu->compositeRect(doc, 0, 0, doc.width, doc.height);
    for (const auto& [x, y] : std::vector<std::pair<std::int32_t, std::int32_t>>{
             {0, 0}, {17, 200}, {299, 299}, {128, 64}}) {
        const StraightRgba8 picked = gpu->pickColour(doc, x, y);
        const StraightRgba8 shown =
            composited[static_cast<std::size_t>(y) * doc.width + x].unpremultiply();
        CHECK(picked == shown);
    }
}

TEST_CASE("a stroke painted on the GPU matches the same stroke on the CPU") {
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    const auto paint = [](Document& doc, PaintBackend* backend, const BrushPreset& p,
                          StraightRgba8 colour) {
        Layer* layer = doc.active();
        Stroke s;
        std::vector<Dab> scratch;
        beginStroke(s, p, colour, layer->id);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height,
                      nullptr, backend};
        for (int i = 0; i < 120; ++i)
            paintSample(s, t, at(30.0 + i * 2.0, 60.0 + std::sin(i * 0.15) * 40.0,
                                 0.3f + static_cast<float>(i % 7) * 0.1f),
                        scratch);
        doc.undo.push(std::move(s.pending));
    };

    for (const BrushPreset& preset : {defaultPencil(), defaultAirbrush(), defaultOpaque()}) {
        Document host = makeDocument(512, 256, StraightRgba8{255, 255, 255, 255});
        Document dev  = makeDocument(512, 256, StraightRgba8{255, 255, 255, 255});
        paint(host, &cpuBackend(), preset, StraightRgba8{20, 60, 200, 200});
        paint(dev, gpu, preset, StraightRgba8{20, 60, 200, 200});

        // The autosave hand-off contract: the host copy is only the truth
        // again once readback has returned (#12).
        REQUIRE(gpu->readback(dev).has_value());

        const Divergence d =
            compare(cpuBackend().compositeRect(host, 0, 0, 512, 256),
                    cpuBackend().compositeRect(dev, 0, 0, 512, 256));
        MESSAGE("dab divergence (" << preset.id << "): max channel " << d.maxChannel
                << ", " << d.pixels << " pixels differ");
        // Looser than compositing on purpose. The CPU measures the distance
        // from a dab's centre in double and the shader in float, so a pixel
        // whose coverage lands within an ULP of a rounding boundary tips the
        // other way. That is a fringe pixel of an anti-aliased edge.
        CHECK(d.maxChannel <= 2);
    }
}

TEST_CASE("a second stroke does not read the arena before the first is submitted") {
    // Regression, #14's "eraser stroke" scenario. Uploads and dabs are queued
    // on the host and only exist on the device once submitted, so the first
    // touch of a tile in a SECOND stroke — which downloads the tile to snapshot
    // it for undo — has to submit what is queued first. It did not, so it read
    // the arena slot back in whatever state the last document to use it left
    // it, and painted that document's pixels underneath the stroke.
    //
    // Invisible in a fresh process, which is why it survived: an untouched
    // slot reads as zeroes, which is exactly what an empty tile should give.
    // So this deliberately dirties the slot with a different document first.
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;
    const WithBackend installed(gpu);

    const auto stroke = [](Document& doc, PaintBackend* backend, const BrushPreset& p,
                           StraightRgba8 colour, double y) {
        Layer* layer = doc.active();
        Stroke s;
        std::vector<Dab> scratch;
        beginStroke(s, p, colour, layer->id);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height,
                      nullptr, backend};
        for (int i = 0; i < 30; ++i) paintSample(s, t, at(20.0 + i * 3.0, y), scratch);
        doc.undo.push(std::move(s.pending));
    };

    const auto twoStrokes = [&stroke](PaintBackend* backend) {
        // A bright document through the same backend, then thrown away: its
        // tiles land in the arena, and the next document's layer 1 tile (0, 0)
        // is the same cache key, so it may well be handed the same slot.
        {
            Document loud = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
            BrushPreset fat = defaultOpaque();
            fat.size = 90.0f;
            stroke(loud, backend, fat, StraightRgba8{255, 0, 255, 255}, 64.0);
            (void)backend->compositeRect(loud, 0, 0, 128, 128);
            REQUIRE(backend->readback(loud).has_value());
        }

        Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
        BrushPreset ink = defaultOpaque();
        ink.size     = 64.0f;
        ink.hardness = 0.8f;
        // No composite, no readback between the two: the only thing that
        // brings the first stroke's pixels home is the second stroke's own
        // first-touch snapshot, which is the path that was wrong.
        stroke(doc, backend, ink, StraightRgba8{30, 60, 120, 255}, 64.0);

        BrushPreset rubber = defaultEraser();
        rubber.size     = 27.0f;
        rubber.hardness = 0.3f;
        stroke(doc, backend, rubber, StraightRgba8{0, 0, 0, 255}, 58.0);
        return doc;
    };

    Document host = twoStrokes(&cpuBackend());
    Document dev  = twoStrokes(gpu);
    REQUIRE(gpu->readback(dev).has_value());
    const Divergence d = compare(cpuBackend().compositeRect(host, 0, 0, 128, 128),
                                 cpuBackend().compositeRect(dev, 0, 0, 128, 128));
    MESSAGE("stroke over stroke, no sync between: max channel " << d.maxChannel
            << ", " << d.pixels << " pixels differ");
    CHECK(d.maxChannel <= 1);

    // The snapshot the second stroke took has to be the canvas after the
    // first, not before it and not somebody else's document: undo the eraser
    // and the ink must still be there.
    const std::size_t inked = dev.active()->tiles.size();
    dev.undo.undo(dev);
    REQUIRE(dev.active() != nullptr);
    CHECK(dev.active()->tiles.size() == inked);
    host.undo.undo(host);
    CHECK(compare(cpuBackend().compositeRect(host, 0, 0, 128, 128),
                  cpuBackend().compositeRect(dev, 0, 0, 128, 128))
              .maxChannel <= 1);
}

TEST_CASE("erasing and preserve-opacity agree between the backends") {
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    const auto run = [](PaintBackend* backend) {
        Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
        Layer* layer = doc.active();
        std::vector<Dab> scratch;

        Stroke ink;
        beginStroke(ink, defaultOpaque(), StraightRgba8{200, 30, 30, 255}, layer->id);
        PaintTarget t{*layer, ink.pending, ink.touched, doc.width, doc.height,
                      nullptr, backend};
        for (int i = 0; i < 40; ++i) paintSample(ink, t, at(40.0 + i * 4.0, 128.0), scratch);
        doc.undo.push(std::move(ink.pending));

        layer->preserveOpacity = true;
        Stroke over;
        beginStroke(over, defaultAirbrush(), StraightRgba8{10, 10, 220, 180}, layer->id);
        PaintTarget t2{*layer, over.pending, over.touched, doc.width, doc.height,
                       nullptr, backend};
        for (int i = 0; i < 40; ++i) paintSample(over, t2, at(40.0 + i * 4.0, 132.0), scratch);
        doc.undo.push(std::move(over.pending));

        layer->preserveOpacity = false;
        BrushPreset rubber = defaultEraser();
        rubber.size = 30.0f;
        Stroke out;
        beginStroke(out, rubber, StraightRgba8{0, 0, 0, 200}, layer->id);
        PaintTarget t3{*layer, out.pending, out.touched, doc.width, doc.height,
                       nullptr, backend};
        for (int i = 0; i < 20; ++i) paintSample(out, t3, at(60.0 + i * 6.0, 128.0), scratch);
        doc.undo.push(std::move(out.pending));
        return doc;
    };

    Document host = run(&cpuBackend());
    Document dev  = run(gpu);
    REQUIRE(gpu->readback(dev).has_value());
    const Divergence d = compare(cpuBackend().compositeRect(host, 0, 0, 256, 256),
                                 cpuBackend().compositeRect(dev, 0, 0, 256, 256));
    MESSAGE("erase/preserve divergence: max channel " << d.maxChannel);
    CHECK(d.maxChannel <= 2);
}

TEST_CASE("undo in GPU mode restores the canvas the CPU would have restored") {
    // #12: undo works identically in both modes. The interesting part is that
    // UndoStack knows nothing about the backend — it swaps host tiles — so
    // this also checks the device copy notices it has been overtaken.
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;
    const WithBackend installed(gpu);

    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    Layer* layer = doc.active();
    std::vector<Dab> scratch;

    const auto blank = gpu->compositeRect(doc, 0, 0, 256, 256);

    Stroke s;
    beginStroke(s, defaultOpaque(), StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height, nullptr, gpu};
    for (int i = 0; i < 60; ++i) paintSample(s, t, at(20.0 + i * 3.0, 120.0), scratch);
    doc.undo.push(std::move(s.pending));

    const auto drawn = gpu->compositeRect(doc, 0, 0, 256, 256);
    CHECK(compare(blank, drawn).pixels > 0);

    doc.undo.undo(doc);
    CHECK(compare(blank, gpu->compositeRect(doc, 0, 0, 256, 256)).maxChannel == 0);
    doc.undo.redo(doc);
    CHECK(compare(drawn, gpu->compositeRect(doc, 0, 0, 256, 256)).maxChannel == 0);

    // A second stroke over the first, so its snapshots have something to hold.
    // Snapshots live on the host in GPU mode (#12's deliberate choice), so the
    // budget the status bar reports is still counting the memory it names —
    // one host tile per touched tile, and nothing hidden in VRAM.
    Stroke again;
    beginStroke(again, defaultOpaque(), StraightRgba8{200, 0, 0, 255}, layer->id);
    PaintTarget t2{*layer, again.pending, again.touched, doc.width, doc.height,
                   nullptr, gpu};
    for (int i = 0; i < 20; ++i) paintSample(again, t2, at(30.0 + i * 3.0, 120.0), scratch);
    const std::size_t touched = again.touched.size();
    doc.undo.push(std::move(again.pending));
    CHECK(touched >= 1);
    CHECK(doc.undo.memoryBytes() >= touched * tileBytes(ColourDepth::Bits8));
    CHECK(doc.undo.memoryBytes() <= (touched + 1) * tileBytes(ColourDepth::Bits8) + 4096);
}

TEST_CASE("the autosave clone gets the pixels the GPU painted") {
    // maybeAutosave calls readback() and then cloneDocument() on the main
    // thread, and hands the clone to a worker with no device context (#12).
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    Layer* layer = doc.active();
    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultOpaque(), StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height, nullptr, gpu};
    for (int i = 0; i < 40; ++i) paintSample(s, t, at(40.0 + i * 4.0, 100.0), scratch);
    doc.undo.push(std::move(s.pending));

    REQUIRE(gpu->readback(doc).has_value());
    const Document clone = cloneDocument(doc);

    // The worker's copy composites through the CPU backend by name, exactly as
    // encodeThumbnail does, and must see the stroke.
    const auto worker = cpuBackend().compositeRect(clone, 0, 0, 256, 256);
    const auto live   = cpuBackend().compositeRect(doc, 0, 0, 256, 256);
    CHECK(compare(worker, live).maxChannel == 0);
    CHECK(compare(worker, cpuBackend().compositeRect(
                              makeDocument(256, 256, StraightRgba8{255, 255, 255, 255}),
                              0, 0, 256, 256))
              .pixels > 0);
}

TEST_CASE("saving and duplicating in GPU mode carry the painted pixels") {
    // The other host-tile readers: the project writer encodes Tile::pixels()
    // straight into PNGs, and duplicateLayer clones them. Neither goes through
    // the backend, so both have to ask for the pixels back first (D-025).
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;
    const WithBackend installed(gpu);

    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    Layer* layer = doc.active();
    std::vector<Dab> scratch;
    Stroke s;
    beginStroke(s, defaultOpaque(), StraightRgba8{0, 0, 0, 255}, layer->id);
    PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height, nullptr, gpu};
    for (int i = 0; i < 40; ++i) paintSample(s, t, at(40.0 + i * 4.0, 100.0), scratch);
    doc.undo.push(std::move(s.pending));

    const LayerId original = layer->id;
    UndoRecord dup = duplicateLayer(doc, original);
    REQUIRE(!dup.empty());
    const Layer* source = doc.layerById(original);
    const Layer* copy   = doc.active();
    REQUIRE(copy != nullptr);
    REQUIRE(copy->id != original);
    REQUIRE(copy->tiles.size() == source->tiles.size());
    for (const auto& [key, tile] : source->tiles) {
        const Tile* other = copy->find(key);
        REQUIRE(other != nullptr);
        CHECK(std::memcmp(tile.pixels8(), other->pixels8(), tileBytes(ColourDepth::Bits8)) == 0);
    }
    // Not blank: a duplicate of nothing would pass the comparison above.
    CHECK(!source->tiles.begin()->second.isFullyTransparent());

    const auto file = scratchFile("gpu_roundtrip.sable");
    REQUIRE(exportDocument(doc, file).has_value());
    const auto reloaded = importDocument(file);
    REQUIRE(reloaded.has_value());
    CHECK(compare(cpuBackend().compositeRect(*reloaded, 0, 0, 256, 256),
                  cpuBackend().compositeRect(doc, 0, 0, 256, 256))
              .maxChannel == 0);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST_CASE("a bucket fill in GPU mode fills the region the GPU composited") {
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    const auto run = [](PaintBackend* backend) {
        Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
        Layer* layer = doc.active();
        std::vector<Dab> scratch;
        BrushPreset p = defaultOpaque();
        p.size = 12.0f;
        Stroke s;
        beginStroke(s, p, StraightRgba8{0, 0, 0, 255}, layer->id);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height,
                      nullptr, backend};
        for (int i = 0; i < 60; ++i) paintSample(s, t, at(128.0, 10.0 + i * 4.0), scratch);
        doc.undo.push(std::move(s.pending));

        UndoRecord rec = backend->bucketFill(doc, layer->id, 20, 20,
                                             StraightRgba8{0, 200, 0, 255}, 16);
        doc.undo.push(std::move(rec));
        return doc;
    };

    Document host = run(&cpuBackend());
    Document dev  = run(gpu);
    REQUIRE(gpu->readback(dev).has_value());
    const Divergence d = compare(cpuBackend().compositeRect(host, 0, 0, 256, 256),
                                 cpuBackend().compositeRect(dev, 0, 0, 256, 256));
    MESSAGE("bucket fill divergence: max channel " << d.maxChannel << ", "
            << d.pixels << " pixels differ");
    CHECK(d.maxChannel <= 2);
}

TEST_CASE("GPU against CPU on a large canvas with many layers"
          * doctest::skip()) {
    // The measurement #13 asks for. Skipped by default because it is a
    // benchmark and not an assertion — run it with:
    //   ./build/engine_tests --no-skip -tc="GPU against CPU*" -s
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) return;

    for (const int layers : {1, 4, 8, 16}) {
        Document doc = layeredDocument(2048, layers);

        const auto time = [&doc](PaintBackend& backend, int reps) {
            const auto start = std::chrono::steady_clock::now();
            std::size_t sink = 0;
            for (int i = 0; i < reps; ++i)
                sink += backend.compositeRect(doc, 0, 0, doc.width, doc.height).size();
            const auto end = std::chrono::steady_clock::now();
            REQUIRE(sink > 0);
            return std::chrono::duration<double, std::milli>(end - start).count() / reps;
        };

        (void)time(*gpu, 1);                 // warm the arena, not the clock
        const double cpuMs = time(cpuBackend(), 3);
        const double gpuMs = time(*gpu, 3);
        MESSAGE("2048x2048, " << layers << " layers: CPU " << cpuMs << " ms, GPU "
                << gpuMs << " ms, speedup " << (cpuMs / gpuMs) << "x");
    }

    const auto clock = [](auto&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start).count();
    };

    // Dabs on their own: 600 samples of a big soft brush, which is the shape
    // that costs the CPU the most per dab.
    for (PaintBackend* backend :
         std::array<PaintBackend*, 2>{&cpuBackend(), gpu}) {
        Document paper = makeDocument(2048, 2048, StraightRgba8{255, 255, 255, 255});
        Layer* layer = paper.active();
        BrushPreset p = defaultAirbrush();
        p.size = 200.0f;
        Stroke s;
        std::vector<Dab> scratch;
        scratch.reserve(1024);
        beginStroke(s, p, StraightRgba8{0, 0, 0, 200}, layer->id);
        PaintTarget t{*layer, s.pending, s.touched, paper.width, paper.height,
                      nullptr, backend};
        const double ms = clock([&] {
            for (int i = 0; i < 600; ++i)
                paintSample(s, t, at(200.0 + i * 2.5, 1024.0 + std::sin(i * 0.05) * 400.0),
                            scratch);
            (void)backend->readback(paper);   // the work is not done until it lands
        });
        MESSAGE("600 airbrush samples on 2048x2048 (" << backend->name() << "): "
                << ms << " ms");
    }

    // The click-frequency paths #12 asks to be measured rather than assumed.
    Document doc = layeredDocument(2048, 8);
    (void)gpu->compositeRect(doc, 0, 0, doc.width, doc.height);
    MESSAGE("pickColour: " << clock([&] { (void)gpu->pickColour(doc, 1000, 1000); })
            << " ms");
    MESSAGE("readback with nothing painted: "
            << clock([&] { (void)gpu->readback(doc); }) << " ms");
    MESSAGE("bucketFill: " << clock([&] {
        UndoRecord rec = gpu->bucketFill(doc, doc.layers.back().id, 5, 5,
                                         StraightRgba8{0, 0, 0, 255}, 200);
        (void)rec;
    }) << " ms");

    // The CPU reference on the same fill, which is also the check on
    // `Tile::stamp()`: it is a global increment inside `setPixel`, so a
    // four-million-pixel flood fill is where it would show up if anywhere.
    MESSAGE("bucketFill on the CPU: " << clock([&] {
        Document plain = layeredDocument(2048, 8);
        UndoRecord rec = cpuBackend().bucketFill(plain, plain.layers.back().id, 5, 5,
                                                 StraightRgba8{0, 0, 0, 255}, 200);
        (void)rec;
    }) << " ms");

    // The autosave pause: a stroke's worth of tiles coming back to the host,
    // which is what maybeAutosave pays on the main thread before the hand-off.
    {
        Layer* layer = doc.active();
        Stroke s;
        std::vector<Dab> scratch;
        BrushPreset p = defaultAirbrush();
        p.size = 300.0f;
        beginStroke(s, p, StraightRgba8{0, 0, 0, 200}, layer->id);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height, nullptr, gpu};
        for (int i = 0; i < 200; ++i) paintSample(s, t, at(300.0 + i * 6.0, 900.0), scratch);
        MESSAGE("readback of " << s.touched.size() << " painted tiles: "
                << clock([&] { (void)gpu->readback(doc); }) << " ms");
    }
}

/// LeakSanitizer asks this at exit, not at startup, so the answer can depend
/// on whether a GPU device was actually created.
///
/// Creating one dlopen()s a Vulkan driver, which allocates about 50 KB of
/// process-wide state and never frees it. The stacks are inside a shared
/// object that has since been unloaded, so a suppression cannot even name
/// them. Leak checking therefore stops for a run that touched a driver, and
/// stays on for every run that did not — which includes the CI sanitizer job,
/// where the engine is built without SDL3 at all. The engine's own leak
/// coverage is unchanged; it is the driver's that is beyond reach.
extern "C" int __lsan_is_turned_off() { return g_gpuUsed ? 1 : 0; }

int main(int argc, char** argv) {
    doctest::Context context(argc, argv);
    const int failed = context.run();
    // Before LeakSanitizer's exit check. Everything the driver hangs off a GPU
    // device stays allocated until the device is destroyed, and a static
    // destructor runs too late to be seen as anything but a leak.
    //
    // No SDL_Quit() here. It measurably freed nothing, and it tore SDL down
    // while differential.cpp's own backend was still alive in a static — which
    // then destroyed a GPU device through a shut-down SDL and took the process
    // with it. `makeGpuBackend` refcounts the video subsystem, so the last
    // backend to be destroyed already quits it.
    g_gpu.reset();
    return failed;
}

// ----------------------------------------------------------------------------
// Text (#20)
//
// The rasterising tests need a font, and a machine can have none — a CI image
// usually does, a minimal container does not. Those say so and pass rather than
// failing for a reason that has nothing to do with Sable. Everything checkable
// without a font is checked unconditionally, and that covers the two things
// most likely to break silently: the .sable round trip, and the UTF-8
// boundaries a CJK caret depends on.

namespace {

std::optional<FontFace> anyFont() {
    for (const FontEntry& entry : systemFonts())
        if (auto face = FontFace::load(entry.path); face.has_value())
            return std::move(*face);
    return std::nullopt;
}

/// How many pixels the text actually covered.
int inkedPixels(const Document& doc) {
    int n = 0;
    for (const Layer& layer : doc.layers)
        for (const auto& [key, tile] : layer.tiles)
            for (int i = 0; i < TILE_PIXELS; ++i)
                if (tile.pixels8()[i].a > 0) ++n;
    return n;
}

}  // namespace

TEST_CASE("a caret moves by characters, not by bytes") {
    // The whole reason this exists: "漢字" is six bytes and two characters, and
    // a backspace that took one byte would leave half a character behind.
    const std::string cjk = "a\xE6\xBC\xA2\xE5\xAD\x97z";   // a漢字z
    CHECK(utf8Next(cjk, 0) == 1);
    CHECK(utf8Next(cjk, 1) == 4);
    CHECK(utf8Next(cjk, 4) == 7);
    CHECK(utf8Next(cjk, 7) == 8);
    CHECK(utf8Next(cjk, 8) == 8);            // never past the end

    CHECK(utf8Prev(cjk, 8) == 7);
    CHECK(utf8Prev(cjk, 7) == 4);
    CHECK(utf8Prev(cjk, 4) == 1);
    CHECK(utf8Prev(cjk, 1) == 0);
    CHECK(utf8Prev(cjk, 0) == 0);            // never before the start

    // Landing mid-character still moves to a boundary rather than compounding
    // the mistake.
    CHECK(utf8Prev(cjk, 3) == 1);
    CHECK(utf8Next(cjk, 2) == 4);
}

TEST_CASE("a text layer round-trips through .sable") {
    Document doc = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255});
    Layer& layer = doc.layers[0];
    layer.kind = LayerKind::Text;

    TextContent text;
    text.utf8        = "hello\n\xE6\xBC\xA2\xE5\xAD\x97";   // two lines, one CJK
    text.fontPath    = "/usr/share/fonts/nowhere/Fake.ttf";
    text.fontName    = "Fake Sans";
    text.sizePx      = 33.5f;
    text.lineSpacing = 1.75f;
    text.align       = TextAlign::Centre;
    text.x           = 120.5;
    text.y           = 64.25;
    text.colour      = StraightRgba8{10, 20, 30, 200};
    layer.text = text;
    // A tile too: the pixels are what renders and the words are what edits, and
    // losing either half makes the other useless.
    layer.tileFor(TileKey{0, 0}).fill(PremulRgba8{10, 10, 10, 255});

    const auto path = std::filesystem::temp_directory_path() / "sable_text.sable";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    REQUIRE(saveProject(doc, path).has_value());

    const auto reloaded = loadProject(path);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->layers.size() == 1);
    const Layer& back = reloaded->layers[0];
    CHECK(back.kind == LayerKind::Text);
    REQUIRE(back.text.has_value());
    CHECK(back.text->utf8 == text.utf8);
    CHECK(back.text->fontPath == text.fontPath);
    CHECK(back.text->fontName == text.fontName);
    CHECK(back.text->sizePx == doctest::Approx(text.sizePx));
    CHECK(back.text->lineSpacing == doctest::Approx(text.lineSpacing));
    CHECK(back.text->align == TextAlign::Centre);
    CHECK(back.text->x == doctest::Approx(text.x));
    CHECK(back.text->y == doctest::Approx(text.y));
    CHECK(back.text->colour == text.colour);
    CHECK(back.tiles.size() == 1);

    std::filesystem::remove(path, ec);
}

TEST_CASE("a text layer survives the clone the autosave thread is handed") {
    // D-013's hand-off. A recovered file that had lost its words would still
    // look right and be uneditable, which is the worst of both.
    Document doc = makeDocument(64, 64, StraightRgba8{0, 0, 0, 0});
    doc.layers[0].kind = LayerKind::Text;
    TextContent text;
    text.utf8     = "recover me";
    text.fontName = "Fake Sans";
    doc.layers[0].text = text;

    const Document copy = cloneDocument(doc);
    REQUIRE(copy.layers[0].text.has_value());
    CHECK(copy.layers[0].text->utf8 == "recover me");
    CHECK(copy.layers[0].kind == LayerKind::Text);
}

TEST_CASE("editing text is one undo step covering the words and the pixels") {
    // The pair has to move together: undoing to pixels that say one thing and a
    // string that says another leaves the next edit resuming from a lie. This
    // is the record shape TextTool::finish builds.
    Document doc = makeDocument(64, 64, StraightRgba8{0, 0, 0, 0});
    Layer& layer = doc.layers[0];
    layer.kind = LayerKind::Text;

    TextContent before;
    before.utf8 = "before";
    layer.text  = before;
    const LayerProps props = propsOf(layer);

    UndoRecord rec;
    rec.tiles.push_back(TileSnapshot{layer.id, TileKey{0, 0}, std::nullopt});
    layer.tileFor(TileKey{0, 0}).fill(PremulRgba8{255, 0, 0, 255});
    TextContent after = before;
    after.utf8 = "after";
    layer.text = after;
    rec.label = "Text";
    rec.structure = LayerStructureDelta{LayerChange::Properties, layer.id, 0,
                                        std::nullopt, props};
    doc.undo.push(std::move(rec));

    doc.undo.undo(doc);
    CHECK(doc.layers[0].text->utf8 == "before");
    CHECK(doc.layers[0].tiles.empty());

    doc.undo.redo(doc);
    CHECK(doc.layers[0].text->utf8 == "after");
    CHECK(doc.layers[0].tiles.size() == 1);
}

TEST_CASE("rasterising text gives up the words and keeps the picture") {
    // The way out of a text layer: it is a property change, so it undoes, and
    // undoing has to give back the protection from paint as well as the words.
    Document doc = makeDocument(64, 64, StraightRgba8{0, 0, 0, 0});
    Layer& layer = doc.layers[0];
    TextContent text;
    text.utf8 = "words";
    layer.text = text;
    layer.kind = LayerKind::Text;
    layer.tileFor(TileKey{0, 0}).fill(PremulRgba8{5, 5, 5, 255});

    LayerProps props = propsOf(layer);
    props.text.reset();
    doc.undo.push(setLayerProps(doc, layer.id, props));
    CHECK(doc.layers[0].kind == LayerKind::Raster);   // paint is allowed again
    CHECK(!doc.layers[0].text.has_value());
    CHECK(doc.layers[0].tiles.size() == 1);           // the picture stayed

    doc.undo.undo(doc);
    CHECK(doc.layers[0].kind == LayerKind::Text);
    REQUIRE(doc.layers[0].text.has_value());
    CHECK(doc.layers[0].text->utf8 == "words");
}

TEST_CASE("text lays out down the page, and alignment moves the line") {
    const auto face = anyFont();
    if (!face.has_value()) { MESSAGE("no font here; layout not exercised"); return; }

    TextContent content;
    content.utf8   = "one\ntwo";
    content.sizePx = 32.0f;
    content.x      = 100.0;
    content.y      = 200.0;

    const TextLayout left = layoutText(*face, content);
    REQUIRE(left.lines.size() == 2);
    CHECK(left.lines[0].baseline == doctest::Approx(200.0));
    // The second line sits a whole line lower, never on top of the first.
    CHECK(left.lines[1].baseline - left.lines[0].baseline ==
          doctest::Approx(left.lineHeight));
    CHECK(left.lines[0].x == doctest::Approx(100.0));
    CHECK(left.lines[0].width > 0.0);

    // Spacing is a multiple of the font's own line, so doubling it doubles the
    // gap and changes nothing else.
    content.lineSpacing = 2.0f;
    const TextLayout loose = layoutText(*face, content);
    CHECK(loose.lineHeight == doctest::Approx(left.lineHeight / 1.2 * 2.0));

    content.lineSpacing = 1.2f;
    content.align = TextAlign::Centre;
    const TextLayout centred = layoutText(*face, content);
    CHECK(centred.lines[0].x ==
          doctest::Approx(100.0 - centred.lines[0].width * 0.5));

    content.align = TextAlign::Right;
    const TextLayout right = layoutText(*face, content);
    CHECK(right.lines[0].x == doctest::Approx(100.0 - right.lines[0].width));

    // A caret at the end of a line belongs to that line, not to the next one.
    CHECK(lineOf(left, 0) == 0);
    CHECK(lineOf(left, 3) == 0);
    CHECK(lineOf(left, 4) == 1);
}

TEST_CASE("drawing text puts ink on the canvas and undo takes all of it away") {
    const auto face = anyFont();
    if (!face.has_value()) { MESSAGE("no font here; rasterising not exercised"); return; }

    Document doc = makeDocument(512, 256, StraightRgba8{0, 0, 0, 0});
    Layer& layer = doc.layers[0];
    layer.kind = LayerKind::Text;

    TextContent content;
    content.utf8   = "Sable";
    content.sizePx = 64.0f;
    content.x      = 20.0;
    content.y      = 120.0;
    content.colour = StraightRgba8{0, 0, 0, 255};

    UndoRecord first = drawTextLayer(layer, content, *face, doc.width, doc.height);
    const int inkFirst = inkedPixels(doc);
    CHECK(inkFirst > 0);

    // Every keystroke redraws the layer from scratch, so tiles are replaced
    // rather than accumulated — otherwise deleting a character would leave it
    // on the canvas.
    content.utf8 = "S";
    UndoRecord second = drawTextLayer(layer, content, *face, doc.width, doc.height);
    CHECK(inkedPixels(doc) < inkFirst);

    // One session, one step: the merge keeps the state from before the FIRST
    // keystroke, which is what undoing a whole edit has to restore.
    mergeTileRecord(first, std::move(second));
    doc.undo.push(std::move(first));
    doc.undo.undo(doc);
    CHECK(inkedPixels(doc) == 0);
    CHECK(doc.layers[0].tiles.empty());

    doc.undo.redo(doc);
    CHECK(inkedPixels(doc) > 0);
}

TEST_CASE("text placed off the canvas allocates nothing") {
    const auto face = anyFont();
    if (!face.has_value()) { MESSAGE("no font here; clipping not exercised"); return; }

    Document doc = makeDocument(64, 64, StraightRgba8{0, 0, 0, 0});
    Layer& layer = doc.layers[0];

    TextContent content;
    content.utf8   = "off the edge";
    content.sizePx = 48.0f;
    content.x      = -4000.0;
    content.y      = -4000.0;
    UndoRecord rec = drawTextLayer(layer, content, *face, doc.width, doc.height);
    CHECK(layer.tiles.empty());

    content.x = 4000.0;
    content.y = 4000.0;
    rec = drawTextLayer(layer, content, *face, doc.width, doc.height);
    CHECK(layer.tiles.empty());
}

TEST_CASE("text composites for the screen exactly as it does for the export") {
    // #1, the worst bug of v1.0.0, applied to text: the tool draws into
    // ordinary tiles precisely so that there is no second path to disagree.
    const auto face = anyFont();
    if (!face.has_value()) { MESSAGE("no font here; compositing not exercised"); return; }

    Document doc = makeDocument(200, 120, StraightRgba8{255, 255, 255, 255});
    doc.layers[0].kind = LayerKind::Text;
    TextContent content;
    content.utf8   = "export";
    content.sizePx = 48.0f;
    content.x      = 10.0;
    content.y      = 80.0;
    content.colour = StraightRgba8{200, 30, 40, 255};
    const UndoRecord rec = drawTextLayer(doc.layers[0], content, *face,
                                         doc.width, doc.height);
    REQUIRE(!doc.layers[0].tiles.empty());

    const std::vector<StraightRgba8> exported = flatten(doc);
    const std::vector<PremulRgba8> onScreen =
        compositeRect(doc, 0, 0, doc.width, doc.height);
    REQUIRE(exported.size() == onScreen.size());
    for (std::size_t i = 0; i < exported.size(); ++i)
        REQUIRE(exported[i] == onScreen[i].unpremultiply());
}

TEST_CASE("a CJK glyph rasterises, where the machine has a font carrying one") {
    // The audience D-002 said an illustration tool cannot afford to exclude.
    // This is the half a headless test can reach — whether the glyphs draw at
    // all. Whether an input METHOD reaches them is a windowed question, and the
    // pull request says plainly whether it was tried.
    const std::string kanji = "\xE6\xBC\xA2";   // 漢
    for (const FontEntry& entry : systemFonts()) {
        auto face = FontFace::load(entry.path);
        if (!face.has_value()) continue;
        // The glyph, not merely an advance: a font with no Japanese in it still
        // advances the pen and still draws — the empty box. Counting that as
        // "CJK works" is exactly the assumption this issue said not to make.
        if (!face->hasGlyph(0x6F22)) continue;

        Document doc = makeDocument(128, 128, StraightRgba8{0, 0, 0, 0});
        TextContent content;
        content.utf8   = kanji;
        content.sizePx = 48.0f;
        content.x      = 20.0;
        content.y      = 80.0;
        const UndoRecord rec =
            drawTextLayer(doc.layers[0], content, *face, doc.width, doc.height);
        if (inkedPixels(doc) == 0) continue;    // an advance, but no outline

        MESSAGE("CJK rasterised through " << entry.name);
        CHECK(inkedPixels(doc) > 0);
        return;
    }
    MESSAGE("no font here carries CJK; rasterising not exercised");
}

// ====================================================================== #21
// 16-bit colour (D-023). Three things have to be true at once: a 16-bit
// document is measurably better at the workflow D-004 named as the cost it was
// accepting, an 8-bit document is not touched at all, and the numbers the
// artist is shown stay true at both depths.

TEST_CASE("widening a channel is lossless and narrowing undoes it exactly") {
    // The lemma the rest of this rests on. `narrow(widen(c)) == c` is what
    // lets an 8-bit source colour travel through 16-bit code — the dab
    // pipeline, the fills, the transform — and land on an 8-bit tile as the
    // byte it started as, which is why none of the 8-bit tests above moved.
    for (int v = 0; v <= 255; ++v) {
        const auto c = static_cast<std::uint8_t>(v);
        CHECK(narrowChannel(widenChannel(c)) == c);
    }
    CHECK(widenChannel(0)   == 0);
    CHECK(widenChannel(255) == 65535);
    // And the ends of the other direction, where an off-by-one hides.
    CHECK(narrowChannel(0)     == 0);
    CHECK(narrowChannel(65535) == 255);
    CHECK(narrowChannel(128)   == 0);      // 128/257 is 0.498
    CHECK(narrowChannel(129)   == 1);      // 129/257 is 0.502
}

TEST_CASE("an 8-bit tile costs exactly what it always cost") {
    // D-023's cost is doubled memory, and the promise beside it is that an
    // 8-bit document does not pay any of it.
    CHECK(tileBytes(ColourDepth::Bits8)  == 262144u);   // the old TILE_BYTES
    CHECK(tileBytes(ColourDepth::Bits16) == 524288u);

    const Tile narrowTile;
    CHECK(narrowTile.depth() == ColourDepth::Bits8);    // the default, still
    CHECK(narrowTile.byteSize() == 262144u);
    CHECK(narrowTile.pixels8()  != nullptr);
    // Null, not a truncating view: a path that has not been taught about the
    // other depth fails at once rather than writing half a drawing.
    CHECK(narrowTile.pixels16() == nullptr);

    const Tile wideTile(ColourDepth::Bits16);
    CHECK(wideTile.byteSize() == 524288u);
    CHECK(wideTile.pixels8()  == nullptr);
    CHECK(wideTile.pixels16() != nullptr);
}

TEST_CASE("a document's depth reaches its layers, its tiles and its clones") {
    Document doc = makeDocument(512, 512, StraightRgba8{0, 0, 0, 0},
                                ColourDepth::Bits16);
    REQUIRE(doc.depth == ColourDepth::Bits16);
    REQUIRE(doc.layers.size() == 1);
    CHECK(doc.layers[0].depth == ColourDepth::Bits16);
    CHECK(doc.active()->tileFor(TileKey{0, 0}).depth() == ColourDepth::Bits16);

    const UndoRecord added = addLayerAbove(doc, doc.activeLayer, "Second");
    CHECK(!added.empty());
    CHECK(doc.active()->depth == ColourDepth::Bits16);

    // cloneDocument is the autosave hand-off. A clone that lost the depth would
    // write a manifest saying 8 over tiles that are 16, and the recovery file
    // would open as a different painting from the one that was lost.
    const Document copy = cloneDocument(doc);
    CHECK(copy.depth == ColourDepth::Bits16);
    for (const Layer& layer : copy.layers) {
        CHECK(layer.depth == ColourDepth::Bits16);
        for (const auto& [key, tile] : layer.tiles)
            CHECK(tile.depth() == ColourDepth::Bits16);
    }
}

namespace {

/// One low-density airbrush dab, laid down `passes` times in the same place —
/// which is what soft shading actually is.
void stackAirbrush(Document& doc, int passes) {
    Layer* layer = doc.active();
    std::vector<Dab> scratch;
    for (int i = 0; i < passes; ++i) {
        Stroke s;
        beginStroke(s, defaultAirbrush(), StraightRgba8{0, 0, 0, 255}, layer->id);
        PaintTarget t{*layer, s.pending, s.touched, doc.width, doc.height};
        paintSample(s, t, at(128.0, 128.0), scratch);
    }
}

/// Alpha along a horizontal line, in 16-bit units at BOTH depths, so the two
/// profiles are directly comparable. An 8-bit tile answers `widen(its byte)`.
std::vector<std::uint16_t> alphaProfile(const Document& doc, std::int32_t y,
                                        std::int32_t x0, std::int32_t x1) {
    std::vector<std::uint16_t> out;
    const Layer& layer = doc.layers.front();
    for (std::int32_t x = x0; x < x1; ++x) {
        const TileKey key{tileIndex(x), tileIndex(y)};
        const Tile* tile = layer.find(key);
        out.push_back(tile == nullptr
                          ? std::uint16_t{0}
                          : tile->pixel(x - key.first * TILE_SIZE,
                                        y - key.second * TILE_SIZE).a);
    }
    return out;
}

/// The length of the longest run of identical values. THIS IS THE BAND: a
/// stretch of a gradient the storage could not tell apart, which is what the
/// eye sees as a step.
std::size_t longestBand(const std::vector<std::uint16_t>& profile) {
    std::size_t best = 0, run = 0;
    for (std::size_t i = 0; i < profile.size(); ++i) {
        run = (i > 0 && profile[i] == profile[i - 1]) ? run + 1 : 1;
        best = std::max(best, run);
    }
    return best;
}

std::size_t distinctLevels(std::vector<std::uint16_t> profile) {
    std::ranges::sort(profile);
    const auto stale = std::ranges::unique(profile);
    return static_cast<std::size_t>(stale.begin() - profile.begin());
}

}  // namespace

// ----------------------------------------------------------------------------
// Linework (#17)
//
// The same bargain D-026 struck for text, and for the same reason: the curves
// rasterise into ordinary tiles, so `compositeRect` gained no case and there is
// no second, screen-only path for #1 to come back through. Everything here is
// checkable without a font or a window, which is why none of it is conditional.

namespace {

/// A straight two-point stroke across the middle of a 128 px canvas.
LineStroke horizontalStroke(float pressureA = 1.0f, float pressureB = 1.0f) {
    LineStroke stroke;
    stroke.width         = 12.0f;
    stroke.minWidthRatio = 0.2f;
    stroke.colour        = StraightRgba8{0, 0, 0, 255};
    stroke.points.push_back(LinePoint{16.0, 64.0, pressureA});
    stroke.points.push_back(LinePoint{112.0, 64.0, pressureB});
    return stroke;
}

/// How many pixels of one column are inked — the line's thickness there.
int columnThickness(const Layer& layer, std::int32_t x, std::int32_t height) {
    int n = 0;
    for (std::int32_t y = 0; y < height; ++y) {
        const TileKey key{tileIndex(x), tileIndex(y)};
        const Tile* tile = layer.find(key);
        if (tile == nullptr) continue;
        if (tile->pixel(x - key.first * TILE_SIZE, y - key.second * TILE_SIZE).a > 0) ++n;
    }
    return n;
}

Document lineworkDocument(const LineworkContent& content) {
    Document doc = makeDocument(128, 128, StraightRgba8{0, 0, 0, 0});
    doc.layers[0].linework = content;
    doc.layers[0].kind     = LayerKind::Linework;
    return doc;
}

}  // namespace

TEST_CASE("stacked low-opacity airbrush passes band less at 16 bits") {
    // The acceptance criterion of #21, and D-004's recorded cost being paid
    // back: "visible banding when many low-opacity airbrush passes stack".
    //
    // Measured, not asserted. The airbrush's soft falloff IS the gradient —
    // coverage runs smoothly from 1 at the centre to 0 at the rim — so the
    // radial alpha profile is a ramp that a depth either resolves or does not.
    constexpr int kPasses = 25;
    constexpr std::int32_t kCentre = 128;
    // Inside the 30 px radius of the default airbrush, so every sample is on
    // the falloff rather than off the end of it.
    constexpr std::int32_t kFrom = kCentre + 1, kTo = kCentre + 29;

    Document narrowDoc = makeDocument(256, 256, StraightRgba8{0, 0, 0, 0});
    Document wideDoc   = makeDocument(256, 256, StraightRgba8{0, 0, 0, 0},
                                      ColourDepth::Bits16);
    stackAirbrush(narrowDoc, kPasses);
    stackAirbrush(wideDoc,   kPasses);

    const auto narrowProfile = alphaProfile(narrowDoc, kCentre, kFrom, kTo);
    const auto wideProfile   = alphaProfile(wideDoc,   kCentre, kFrom, kTo);
    REQUIRE(narrowProfile.size() == wideProfile.size());

    const std::size_t narrowBand = longestBand(narrowProfile);
    const std::size_t wideBand   = longestBand(wideProfile);
    const std::size_t narrowLevels = distinctLevels(narrowProfile);
    const std::size_t wideLevels   = distinctLevels(wideProfile);

    MESSAGE("banding over " << narrowProfile.size() << " px of airbrush falloff, "
            << kPasses << " stacked passes: 8-bit has " << narrowLevels
            << " distinct levels and a longest band of " << narrowBand
            << " px; 16-bit has " << wideLevels << " levels and a longest band of "
            << wideBand << " px");

    // Both halves of "less banding": more of the ramp is resolved, and the
    // flat stretches are shorter.
    CHECK(wideLevels > narrowLevels);
    CHECK(wideBand   < narrowBand);

    // And it is the same picture, not a different one. Not pixel-for-pixel:
    // twenty-five passes of rounding at one step per pass compound, and the
    // 8-bit ramp is the one that drifts — which is the defect, not a
    // disagreement about what to draw. So compare the totals, which is the
    // ink actually laid down.
    const auto total = [](const std::vector<std::uint16_t>& p) {
        double sum = 0.0;
        for (const std::uint16_t v : p) sum += v;
        return sum;
    };
    const double narrowInk = total(narrowProfile), wideInk = total(wideProfile);
    MESSAGE("ink over the same falloff: 8-bit " << narrowInk << ", 16-bit " << wideInk);
    CHECK(std::abs(narrowInk - wideInk) / wideInk < 0.15);

    // Monotonically non-increasing outward, at both depths: a profile that
    // wobbled would make "distinct levels" mean noise rather than resolution.
    for (std::size_t i = 1; i < wideProfile.size(); ++i)
        CHECK(wideProfile[i] <= wideProfile[i - 1]);
}

TEST_CASE("a very low density pass registers at 16 bits where 8 loses it") {
    // The other face of the same problem, and the one an artist meets first: a
    // pass so light that its whole contribution rounds to nothing. At 8 bits
    // the canvas is untouched however many times it is repeated.
    BrushPreset faint = defaultAirbrush();
    faint.density = 0.001f;                // a pen barely touching the tablet

    const auto paintOnce = [&](Document& doc) {
        std::vector<Dab> scratch;
        Stroke s;
        beginStroke(s, faint, StraightRgba8{0, 0, 0, 255}, doc.activeLayer);
        PaintTarget t{*doc.active(), s.pending, s.touched, doc.width, doc.height};
        paintSample(s, t, at(128.0, 128.0), scratch);
    };

    Document narrowDoc = makeDocument(256, 256, StraightRgba8{0, 0, 0, 0});
    Document wideDoc   = makeDocument(256, 256, StraightRgba8{0, 0, 0, 0},
                                      ColourDepth::Bits16);
    for (int i = 0; i < 20; ++i) { paintOnce(narrowDoc); paintOnce(wideDoc); }

    const std::uint16_t narrowAlpha = alphaProfile(narrowDoc, 128, 128, 129).front();
    const std::uint16_t wideAlpha   = alphaProfile(wideDoc,   128, 128, 129).front();
    MESSAGE("20 passes at density 0.001: 8-bit alpha " << narrowAlpha
            << ", 16-bit alpha " << wideAlpha << " (both in 16-bit units)");
    CHECK(narrowAlpha == 0);
    CHECK(wideAlpha   >  0);
}

TEST_CASE("the undo budget tells the truth at both depths") {
    // D-017's budget is SHOWN to the artist, so at 16 bits it has to say the
    // real number: twice the bytes per snapshot, and therefore about half the
    // history at the same setting. Under-reporting by half would be the status
    // bar quietly lying about how much undo is left.
    const auto strokeCost = [](ColourDepth depth) {
        Document doc = makeDocument(1024, 1024, StraightRgba8{255, 255, 255, 255},
                                    depth);
        // The tiles have to EXIST first. D-006 snapshots what was there, and
        // "nothing was there" costs no bytes — correctly, but it would measure
        // the wrong thing here.
        std::vector<Dab> scratch;
        {
            Stroke warm;
            beginStroke(warm, defaultPencil(), StraightRgba8{0, 0, 0, 255},
                        doc.activeLayer);
            PaintTarget t{*doc.active(), warm.pending, warm.touched, doc.width,
                          doc.height};
            for (int i = 0; i < 80; ++i) paintSample(warm, t, at(300.0 + i, 300.0), scratch);
        }

        Stroke s;
        beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, doc.activeLayer);
        PaintTarget t{*doc.active(), s.pending, s.touched, doc.width, doc.height};
        for (int i = 0; i < 80; ++i) paintSample(s, t, at(300.0 + i, 302.0), scratch);
        const std::size_t touched = s.touched.size();
        REQUIRE(touched > 0);
        doc.undo.push(std::move(s.pending));
        return std::pair<std::size_t, std::size_t>{doc.undo.memoryBytes(), touched};
    };

    const auto [narrowBytes, narrowTiles] = strokeCost(ColourDepth::Bits8);
    const auto [wideBytes,   wideTiles]   = strokeCost(ColourDepth::Bits16);
    REQUIRE(narrowTiles == wideTiles);

    // One snapshot per touched tile, at that tile's own size — not at a
    // constant that stopped being one.
    CHECK(narrowBytes >= narrowTiles * tileBytes(ColourDepth::Bits8));
    CHECK(narrowBytes <  narrowTiles * tileBytes(ColourDepth::Bits8) + 4096);
    CHECK(wideBytes   >= wideTiles   * tileBytes(ColourDepth::Bits16));
    CHECK(wideBytes   <  wideTiles   * tileBytes(ColourDepth::Bits16) + 4096);
    MESSAGE("one stroke over " << narrowTiles << " tiles costs " << narrowBytes
            << " bytes of history at 8 bits and " << wideBytes << " at 16");

    // And the eviction that follows from it: the same budget holds fewer 16-bit
    // records, which is the cost D-023 accepted in writing.
    const auto recordsHeld = [](ColourDepth depth, std::size_t budget) {
        Document doc = makeDocument(512, 512, StraightRgba8{0, 0, 0, 0}, depth);
        // Every tile allocated up front, so each stroke below snapshots a tile
        // that existed and the comparison is about bytes per snapshot rather
        // than about which stroke happened to create a tile.
        const UndoRecord warm =
            fillSelection(doc, doc.activeLayer, StraightRgba8{200, 200, 200, 255});
        CHECK(!warm.empty());
        doc.undo.setMemoryBudget(budget);
        std::vector<Dab> scratch;
        for (int i = 0; i < 12; ++i) {
            Stroke s;
            beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, doc.activeLayer);
            PaintTarget t{*doc.active(), s.pending, s.touched, doc.width, doc.height};
            paintSample(s, t, at(20.0 + i * 30.0, 20.0), scratch);
            doc.undo.push(std::move(s.pending));
        }
        CHECK(doc.undo.memoryBytes() <= doc.undo.memoryBudget());
        return doc.undo.size();
    };
    const std::size_t budget = 8u * tileBytes(ColourDepth::Bits8);
    const std::size_t narrowHeld = recordsHeld(ColourDepth::Bits8, budget);
    const std::size_t wideHeld   = recordsHeld(ColourDepth::Bits16, budget);
    MESSAGE("at a budget of eight 8-bit tiles: " << narrowHeld
            << " steps of history at 8 bits, " << wideHeld << " at 16");
    // "Roughly half the history at the same setting", which is what D-023 says
    // in words and what the status bar now has to be able to report.
    CHECK(wideHeld * 2 <= narrowHeld + 1);
    CHECK(wideHeld >= 1);          // D-017: one record always survives
}

TEST_CASE("a 16-bit document saves, reloads and keeps its extra bits") {
    Document doc = makeDocument(256, 256, StraightRgba8{0, 0, 0, 0},
                                ColourDepth::Bits16);
    stackAirbrush(doc, 25);
    // A value no 8-bit tile could hold, so a round trip that silently narrowed
    // would be caught by the pixel itself rather than only by the manifest.
    doc.active()->tileFor(TileKey{0, 0}).setPixel(
        4, 4, PremulRgba16{1000, 2000, 3000, 65535});

    const auto path = std::filesystem::temp_directory_path() / "sable_16bit.sable";
    REQUIRE(saveProject(doc, path).has_value());
    const auto loaded = loadProject(path);
    REQUIRE(loaded.has_value());

    CHECK(loaded->depth == ColourDepth::Bits16);
    for (const Layer& layer : loaded->layers) CHECK(layer.depth == ColourDepth::Bits16);

    const Tile* tile = loaded->layers.front().find(TileKey{0, 0});
    REQUIRE(tile != nullptr);
    REQUIRE(tile->depth() == ColourDepth::Bits16);
    const PremulRgba16 back = tile->pixel(4, 4);
    // Not exact: the tile PNG holds STRAIGHT alpha, so the round trip is
    // unpremultiply -> premultiply and rounds once each way. Well inside an
    // 8-bit step, which is the claim that matters.
    CHECK(std::abs(static_cast<int>(back.r) - 1000) <= 2);
    CHECK(std::abs(static_cast<int>(back.g) - 2000) <= 2);
    CHECK(std::abs(static_cast<int>(back.b) - 3000) <= 2);
    CHECK(back.a == 65535);

    // The whole painting, not one pixel: the airbrush falloff has to come back
    // as the falloff, or the file is 8-bit with extra steps.
    const auto before = alphaProfile(doc,     128, 129, 157);
    const auto after  = alphaProfile(*loaded, 128, 129, 157);
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK(std::abs(static_cast<int>(before[i]) - static_cast<int>(after[i])) <= 2);
    CHECK(distinctLevels(after) > 20);      // still a ramp, not a staircase

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("only a 16-bit file claims the newer format version") {
    // The last acceptance criterion: an older Sable must REFUSE a 16-bit file
    // rather than misread it — and must not be locked out of an 8-bit one for
    // a feature it is not using.
    const auto manifestOf = [](const std::filesystem::path& path) {
        mz_zip_archive zip{};
        REQUIRE(mz_zip_reader_init_file(&zip, path.string().c_str(), 0));
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, "document.json", &size, 0);
        REQUIRE(data != nullptr);
        const nlohmann::json manifest =
            nlohmann::json::parse(std::string(static_cast<const char*>(data), size));
        mz_free(data);
        mz_zip_reader_end(&zip);
        return manifest;
    };

    const auto dir = std::filesystem::temp_directory_path();
    const auto narrowPath = dir / "sable_depth8.sable";
    const auto widePath   = dir / "sable_depth16.sable";

    const Document narrowDoc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    const Document wideDoc   = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255},
                                            ColourDepth::Bits16);
    REQUIRE(saveProject(narrowDoc, narrowPath).has_value());
    REQUIRE(saveProject(wideDoc,   widePath).has_value());

    const nlohmann::json narrowManifest = manifestOf(narrowPath);
    const nlohmann::json wideManifest   = manifestOf(widePath);

    CHECK(narrowManifest["format_version"] == SABLE_FORMAT_VERSION_8BIT);
    CHECK(narrowManifest["colour"]["depth"] == 8);
    CHECK(wideManifest["format_version"] == SABLE_FORMAT_VERSION_16BIT);
    CHECK(wideManifest["colour"]["depth"] == 16);
    // Which is only worth anything if the two differ — otherwise the gate on
    // load has nothing to catch.
    CHECK(SABLE_FORMAT_VERSION_16BIT > SABLE_FORMAT_VERSION_8BIT);

    // #48 takes the same deal, and needs it more: an older Sable would open a
    // masked document, show every layer unmasked, and write the masks away on
    // the next save. An 8-bit document with no mask is unaffected by either.
    const auto maskedPath = dir / "sable_masked.sable";
    Document maskedDoc = makeDocument(64, 64, StraightRgba8{255, 255, 255, 255});
    maskedDoc.layers[0].mask.emplace();
    REQUIRE(saveProject(maskedDoc, maskedPath).has_value());
    CHECK(manifestOf(maskedPath)["format_version"] == SABLE_FORMAT_VERSION_MASK);
    CHECK(SABLE_FORMAT_VERSION_MASK > SABLE_FORMAT_VERSION_16BIT);
    CHECK(SABLE_FORMAT_VERSION == SABLE_FORMAT_VERSION_MASK);   // the newest known

    std::error_code ec;
    std::filesystem::remove(narrowPath, ec);
    std::filesystem::remove(widePath, ec);
    std::filesystem::remove(maskedPath, ec);
}

TEST_CASE("a 16-bit document composites, picks and exports") {
    // Nothing here is about precision: it is that every 8-bit consumer of a
    // document — the screen, the eyedropper, PNG export — still gets an answer,
    // and the same answer as each other (#1, US-13.3).
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255},
                                ColourDepth::Bits16);
    doc.active()->tileFor(TileKey{0, 0}).fill(PremulRgba16{0, 0, 32768, 32768});

    const std::vector<StraightRgba8> flat = flatten(doc);
    REQUIRE(flat.size() == 128u * 128u);
    const StraightRgba8 picked = pickColour(doc, 40, 40);
    CHECK(picked == flat[40 * 128 + 40]);
    // Half-alpha blue over white: opaque once composited, and still bluer than
    // it is red.
    CHECK(picked.a == 255);
    CHECK(picked.b > picked.r);

    const std::vector<PremulRgba8> rect = compositeRect(doc, 0, 0, 128, 128);
    REQUIRE(rect.size() == flat.size());
    CHECK(rect[40 * 128 + 40].unpremultiply() == flat[40 * 128 + 40]);

    const auto png = std::filesystem::temp_directory_path() / "sable_16bit.png";
    CHECK(exportPng(doc, png).has_value());
    std::error_code ec;
    std::filesystem::remove(png, ec);
}

TEST_CASE("a 16-bit layer merges, fills and transforms without narrowing") {
    Document doc = makeDocument(256, 256, StraightRgba8{0, 0, 0, 0},
                                ColourDepth::Bits16);
    const LayerId lower = doc.activeLayer;
    const UndoRecord added = addLayerAbove(doc, lower, "Upper");
    CHECK(!added.empty());
    const LayerId upper = doc.activeLayer;

    doc.layerById(lower)->tileFor(TileKey{0, 0}).fill(PremulRgba16{0, 0, 0, 65535});
    doc.layerById(upper)->tileFor(TileKey{0, 0}).fill(PremulRgba16{300, 0, 0, 65535});

    const UndoRecord merged = mergeLayerDown(doc, upper);
    CHECK(!merged.empty());
    const Tile* tile = doc.layerById(lower)->find(TileKey{0, 0});
    REQUIRE(tile != nullptr);
    CHECK(tile->depth() == ColourDepth::Bits16);
    // 300 sits between two 8-bit steps (257 and 514). A merge that went through
    // eight bits would land on one of them.
    CHECK(tile->pixel(9, 9).r == 300);

    // A fill takes an 8-bit colour from the picker, so nothing is lost by it —
    // but it must not damage the 16-bit tile it lands in either.
    const UndoRecord filled = fillSelection(doc, lower, StraightRgba8{10, 20, 30, 255});
    CHECK(!filled.empty());
    CHECK(doc.layerById(lower)->find(TileKey{0, 0})->depth() == ColourDepth::Bits16);
    CHECK(narrow(doc.layerById(lower)->find(TileKey{0, 0})->pixel(9, 9)) ==
          PremulRgba8{10, 20, 30, 255});

    // A transform reads pixels and puts them back. Reading at eight bits would
    // make nudging a selection a destructive edit.
    doc.layerById(lower)->tileFor(TileKey{0, 0}).fill(PremulRgba16{300, 600, 900, 65535});
    const UndoRecord moved =
        transformRegion(doc, lower, Selection{0, 0, 64, 64}, Transform{.dx = 64.0});
    CHECK(!moved.empty());
    CHECK(doc.layerById(lower)->find(TileKey{0, 0})->pixel(80, 20).r == 300);
}

TEST_CASE("the GPU backend declines a 16-bit document and the CPU finishes it") {
    // #21: the arena, both shaders and every transfer are 8-bit RGBA. Declining
    // has to mean "the CPU does it", not "nothing happens" and not "half the
    // depth gets painted" — so this drives the GPU backend as the process
    // default over a 16-bit document and asks for the same picture back.
    PaintBackend* gpu = gpuForTests();
    if (gpu == nullptr) {
        MESSAGE("no GPU device here; the decline path is not exercised");
        return;
    }

    const auto paint = [](Document& doc, PaintBackend* backend) {
        std::vector<Dab> scratch;
        Stroke s;
        beginStroke(s, defaultAirbrush(), StraightRgba8{200, 30, 60, 255}, doc.activeLayer);
        PaintTarget t{*doc.active(), s.pending, s.touched, doc.width, doc.height,
                      nullptr, backend};
        for (int i = 0; i < 30; ++i) paintSample(s, t, at(60.0 + i * 4, 120.0), scratch);
        doc.undo.push(std::move(s.pending));
    };

    Document reference = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255},
                                      ColourDepth::Bits16);
    Document candidate = makeDocument(256, 256, StraightRgba8{255, 255, 255, 255},
                                      ColourDepth::Bits16);
    paint(reference, &cpuBackend());
    {
        WithBackend installed{gpu};
        paint(candidate, gpu);
        // Whatever the backend thinks it is holding, the host tiles must be the
        // truth afterwards — this is the call save and autosave make.
        CHECK(gpu->readback(candidate).has_value());
    }

    // Identical, not merely close: the GPU never touched these pixels, so there
    // is no tolerance to allow. Anything else means it painted some of them.
    const Tile* wanted = reference.layers.front().find(TileKey{0, 0});
    const Tile* got    = candidate.layers.front().find(TileKey{0, 0});
    REQUIRE(wanted != nullptr);
    REQUIRE(got    != nullptr);
    CHECK(got->depth() == ColourDepth::Bits16);
    std::size_t differing = 0;
    for (int y = 0; y < TILE_SIZE; ++y)
        for (int x = 0; x < TILE_SIZE; ++x)
            if (!(got->pixel(x, y) == wanted->pixel(x, y))) ++differing;
    CHECK(differing == 0);

    // And the composite the artist would be looking at agrees too, which goes
    // through GpuBackend::compositeRect and its own decline.
    WithBackend installed{gpu};
    const std::vector<StraightRgba8> onGpu = flatten(candidate);
    const std::vector<StraightRgba8> onCpu = flatten(reference, cpuBackend());
    REQUIRE(onGpu.size() == onCpu.size());
    CHECK(onGpu == onCpu);
}

TEST_CASE("a linework curve rasterises into ordinary tiles") {
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());
    Document doc = lineworkDocument(content);

    UndoRecord rec = drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    CHECK(!doc.layers[0].tiles.empty());
    CHECK(inkedPixels(doc) > 0);

    // On the line, and off it. A 6 px half-width at full pressure puts the edge
    // well clear of both.
    CHECK(pickColour(doc, 64, 64).a == 255);
    CHECK(pickColour(doc, 64, 20).a == 0);

    // Undo takes every pixel back, including the tiles that did not exist
    // before — the record has to carry those as "was absent", or a redrawn
    // layer accumulates tiles nothing ever removes.
    doc.undo.push(std::move(rec));
    doc.undo.undo(doc);
    CHECK(inkedPixels(doc) == 0);
    CHECK(doc.layers[0].tiles.empty());
}

TEST_CASE("pressure varies the line's width along the curve") {
    // A linework layer is only worth having if the line tapers. Full pressure
    // at the left end, none at the right.
    LineworkContent content;
    content.strokes.push_back(horizontalStroke(1.0f, 0.0f));
    Document doc = lineworkDocument(content);
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);

    const int thick = columnThickness(doc.layers[0], 24, doc.height);
    const int thin  = columnThickness(doc.layers[0], 104, doc.height);
    CHECK(thick > thin);
    // minWidthRatio 0.2 of a 12 px width is 2.4 px against 12 at the other end
    // — a taper, not a rounding difference.
    CHECK(thick >= 10);
    CHECK(thin  <= 6);

    // And it is the pressure doing it, not the position: the same stroke at one
    // pressure throughout is the same width at both ends.
    LineworkContent even;
    even.strokes.push_back(horizontalStroke(1.0f, 1.0f));
    Document flat = lineworkDocument(even);
    (void)drawLineworkLayer(flat.layers[0], even, flat.width, flat.height);
    CHECK(columnThickness(flat.layers[0], 24, flat.height) ==
          columnThickness(flat.layers[0], 104, flat.height));
}

TEST_CASE("a control point moves, and the line moves with it") {
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());
    Document doc = lineworkDocument(content);
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    CHECK(pickColour(doc, 100, 64).a == 255);
    CHECK(pickColour(doc, 100, 100).a == 0);

    // The point the artist would have grabbed, then dragged.
    const std::optional<PointRef> grabbed = nearestPoint(content, 110.0, 66.0, 8.0);
    REQUIRE(grabbed.has_value());
    CHECK(grabbed->point == 1);
    content.strokes[grabbed->stroke].points[grabbed->point].y = 100.0;

    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    CHECK(pickColour(doc, 100, 100).a > 0);
    CHECK(pickColour(doc, 100, 64).a == 0);

    // Nothing within reach is nothing, not the nearest point on the canvas.
    CHECK(!nearestPoint(content, 5.0, 5.0, 8.0).has_value());
}

TEST_CASE("a control point is added on the curve and deleted off it") {
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());

    // Added ON the line, not at the click: a new point that moved the curve
    // before it had been dragged would be one the artist has to undo.
    const std::optional<PointRef> added = insertPoint(content, 64.0, 66.0, 8.0);
    REQUIRE(added.has_value());
    CHECK(added->point == 1);
    REQUIRE(content.strokes[0].points.size() == 3);
    CHECK(content.strokes[0].points[1].x == doctest::Approx(64.0).epsilon(0.05));
    CHECK(content.strokes[0].points[1].y == doctest::Approx(64.0).epsilon(0.05));

    // A click nowhere near the curve adds nothing.
    CHECK(!insertPoint(content, 10.0, 10.0, 4.0).has_value());
    CHECK(content.strokes[0].points.size() == 3);

    CHECK(erasePoint(content, PointRef{0, 1}));
    CHECK(content.strokes[0].points.size() == 2);

    // Down to one point there is no curve left, so the stroke goes with it — a
    // dot the artist cannot see the shape of is one they cannot get rid of.
    CHECK(erasePoint(content, PointRef{0, 0}));
    CHECK(content.strokes.empty());
    CHECK(!erasePoint(content, PointRef{0, 0}));      // and asking again is safe
}

TEST_CASE("the stabiliser leaves a shaky freehand line fewer, straighter points") {
    // #51.1. The stabiliser is a pure sample-to-sample function, so a linework
    // curve gets it in the same place the brush does — and the two halves that
    // decide what a freehand curve looks like, the smoothing and the point
    // spacing, are only meaningful together. A hand that shakes 5 px either
    // side of a straight line must not leave a control point on every wobble.
    const auto drawShaky = [](std::uint8_t level) {
        Stabilizer stabilizer;
        stabilizer.setLevel(level);
        LineStroke stroke;
        stroke.points.push_back(LinePoint{0.0, 0.0, 1.0f});
        for (int i = 1; i < 400; ++i) {
            const InputSample s = stabilizer.apply(at(i * 1.0, (i % 2 == 0) ? 5.0 : -5.0));
            (void)appendFreehand(stroke, LinePoint{s.x, s.y, 1.0f}, 14.0);
        }
        return stroke;
    };
    /// How far the control points wander off the line the artist meant, past
    /// the transient at the start where the string is still being pulled taut.
    const auto worstOffset = [](const LineStroke& stroke) {
        double worst = 0.0;
        for (const LinePoint& p : stroke.points)
            if (p.x > 100.0) worst = std::max(worst, std::abs(p.y));
        return worst;
    };

    const LineStroke raw      = drawShaky(0);
    const LineStroke smoothed = drawShaky(3);

    CHECK(worstOffset(raw) == doctest::Approx(5.0));
    CHECK(worstOffset(smoothed) < 1.0);
    // Fewer, as well as straighter: a wobble that is not smoothed out is extra
    // travel, and extra travel is extra handles to drag afterwards.
    CHECK(smoothed.points.size() < raw.points.size());
    // And it still gets to the end of the line — a stabilised curve that stops
    // short is the pulled-string bug the brush already had to fix (US-11.4).
    CHECK(smoothed.points.back().x > 300.0);
}

TEST_CASE("a closed curve joins with no seam and no double-darkened join") {
    // #51.3. The join is exactly the artefact `drawLineworkLayer` accumulates
    // coverage to avoid: the last segment ends where the first begins, so a
    // rasteriser that composited per stamp would put two layers of a
    // semi-transparent line on the one pixel and leave a dark dot there.
    LineStroke ring;
    ring.width         = 6.0f;
    ring.minWidthRatio = 1.0f;                      // even width, so alpha is comparable
    ring.colour        = StraightRgba8{0, 0, 0, 128};
    ring.points.push_back(LinePoint{32.0, 32.0, 1.0f});
    ring.points.push_back(LinePoint{96.0, 32.0, 1.0f});
    ring.points.push_back(LinePoint{96.0, 96.0, 1.0f});
    ring.points.push_back(LinePoint{32.0, 96.0, 1.0f});

    LineworkContent open;
    open.strokes.push_back(ring);
    Document unclosed = lineworkDocument(open);
    (void)drawLineworkLayer(unclosed.layers[0], open, unclosed.width, unclosed.height);

    LineworkContent shut;
    shut.strokes.push_back(ring);
    shut.strokes[0].closed = true;
    Document closed = lineworkDocument(shut);
    (void)drawLineworkLayer(closed.layers[0], shut, closed.width, closed.height);

    // No seam: a point well inside the closing segment — asked of the same
    // `samplePoints` the rasteriser walks, rather than guessed from the control
    // points, because the spline bulges past a corner it turns — is inked when
    // the stroke is closed and bare when it is not.
    const std::vector<LinePoint> walk = samplePoints(shut.strokes[0], 1.0);
    const LinePoint& onClosing = walk[walk.size() * 7 / 8];
    const auto cx = static_cast<std::int32_t>(std::lround(onClosing.x));
    const auto cy = static_cast<std::int32_t>(std::lround(onClosing.y));
    CHECK(pickColour(closed, cx, cy).a > 0);
    CHECK(pickColour(unclosed, cx, cy).a == 0);

    // No double-darkening. The ring ends on the pixel it started on, so if
    // coverage were composited per stamp rather than accumulated first, the
    // join would carry two passes of a half-transparent line and show as a dark
    // dot. Asked of every pixel, because the join is not the only place the
    // closed walk visits twice.
    std::uint8_t worst = 0;
    for (std::int32_t y = 0; y < closed.height; ++y)
        for (std::int32_t x = 0; x < closed.width; ++x)
            worst = std::max(worst, pickColour(closed, x, y).a);
    CHECK(worst == ring.colour.a);
    CHECK(pickColour(closed, 32, 32).a == ring.colour.a);
}

TEST_CASE("a whole stroke is grabbed by its line and moved as a unit") {
    // #51.2. `nearestPoint` answers "which handle", which is no use to an
    // artist who wants the line itself; this is the whole-stroke answer.
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());

    // The line, not a handle: the middle of the curve is 48 px from either end.
    const std::optional<std::size_t> grabbed = nearestStroke(content, 64.0, 66.0, 8.0);
    REQUIRE(grabbed.has_value());
    CHECK(*grabbed == 0);
    CHECK(!nearestStroke(content, 64.0, 20.0, 8.0).has_value());

    const LineStroke before = content.strokes[0];
    transformStrokes(content, {0}, Transform{10.0, 20.0, 1.0, 1.0, 0.0});
    for (std::size_t i = 0; i < before.points.size(); ++i) {
        CHECK(content.strokes[0].points[i].x == doctest::Approx(before.points[i].x + 10.0));
        CHECK(content.strokes[0].points[i].y == doctest::Approx(before.points[i].y + 20.0));
    }
    // A move is not a scale, so the line does not change thickness on the way.
    CHECK(content.strokes[0].width == doctest::Approx(before.width));

    // Indices that name nothing are ignored rather than fatal: a selection can
    // outlive an undo that removed the stroke it pointed at.
    transformStrokes(content, {7}, Transform{100.0, 0.0, 1.0, 1.0, 0.0});
    CHECK(content.strokes[0].points[0].x == doctest::Approx(before.points[0].x + 10.0));
}

TEST_CASE("a stroke turns and scales about its own centre") {
    // A diagonal, so a sign slip in the rotation shows: a horizontal line
    // turned a quarter turn either way lands on the same two points.
    LineworkContent content;
    LineStroke diagonal;
    diagonal.width = 12.0f;
    diagonal.points.push_back(LinePoint{32.0, 32.0, 1.0f});
    diagonal.points.push_back(LinePoint{96.0, 96.0, 1.0f});
    content.strokes.push_back(diagonal);

    // Clockwise about the centre of its own bounding box, which is the mapping
    // `transformRegion` applies to a region of pixels — one meaning of an angle
    // in the program, whether what is turning is a curve or a rectangle.
    transformStrokes(content, {0}, Transform{0.0, 0.0, 1.0, 1.0, std::numbers::pi / 2.0});
    CHECK(content.strokes[0].points[0].x == doctest::Approx(96.0));
    CHECK(content.strokes[0].points[0].y == doctest::Approx(32.0));
    CHECK(content.strokes[0].points[1].x == doctest::Approx(32.0));
    CHECK(content.strokes[0].points[1].y == doctest::Approx(96.0));

    // The width scales with the geometry: a curve blown up to twice the size
    // that kept a 12 px line is not the same drawing enlarged.
    transformStrokes(content, {0}, Transform{0.0, 0.0, 2.0, 2.0, 0.0});
    CHECK(content.strokes[0].width == doctest::Approx(24.0f));
    CHECK(content.strokes[0].points[0].x == doctest::Approx(128.0));
    CHECK(content.strokes[0].points[0].y == doctest::Approx(0.0));
}

TEST_CASE("recolouring a finished stroke is one undo step, curves and pixels") {
    // #51.4, and the reason `LayerProps::linework` exists: the colour is stored
    // per stroke and editable, so changing it is a property change that carries
    // the re-rasterised pixels with it. Undo has to put back both, or the next
    // edit starts from a colour that is not what is on the canvas.
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());
    Document doc = lineworkDocument(content);
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    const std::uint64_t asBlack = hashCanvas(doc);
    CHECK(pickColour(doc, 64, 64) == StraightRgba8{0, 0, 0, 255});

    // Exactly what the tool does: snapshot the properties, edit the curves,
    // re-rasterise, and push the two as one record.
    const LayerProps before = propsOf(doc.layers[0]);
    doc.layers[0].linework->strokes[0].colour = StraightRgba8{200, 30, 40, 255};
    UndoRecord rec = drawLineworkLayer(doc.layers[0], *doc.layers[0].linework,
                                       doc.width, doc.height);
    rec.structure = LayerStructureDelta{LayerChange::Properties, doc.layers[0].id, 0,
                                        std::nullopt, before};
    const std::size_t steps = doc.undo.size();
    doc.undo.push(std::move(rec));
    CHECK(doc.undo.size() == steps + 1);
    CHECK(pickColour(doc, 64, 64) == StraightRgba8{200, 30, 40, 255});

    doc.undo.undo(doc);
    REQUIRE(doc.layers[0].linework.has_value());
    CHECK(doc.layers[0].linework->strokes[0].colour == StraightRgba8{0, 0, 0, 255});
    CHECK(hashCanvas(doc) == asBlack);
}

TEST_CASE("linework composites for the screen exactly as it does for the export") {
    // #1 again. The proof is that turning the layer into a plain raster one
    // changes nothing: the compositor never knew the difference.
    LineworkContent content;
    content.strokes.push_back(horizontalStroke(1.0f, 0.3f));
    content.strokes.back().colour = StraightRgba8{20, 40, 200, 180};
    Document doc = lineworkDocument(content);
    doc.background = StraightRgba8{255, 255, 255, 255};
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);

    const std::vector<StraightRgba8> exported = flatten(doc);
    const std::vector<PremulRgba8> onScreen =
        compositeRect(doc, 0, 0, doc.width, doc.height);
    REQUIRE(exported.size() == onScreen.size());
    for (std::size_t i = 0; i < exported.size(); ++i)
        REQUIRE(exported[i] == onScreen[i].unpremultiply());

    const std::uint64_t asLinework = hashCanvas(doc);
    doc.layers[0].linework.reset();
    doc.layers[0].kind = LayerKind::Raster;
    CHECK(hashCanvas(doc) == asLinework);
}

TEST_CASE("a line that doubles back over itself is not drawn darker") {
    // The difference between a drawn line and a painted one. A curve that comes
    // back over its own path, or simply slows down, must come out one line —
    // which is why the rasteriser accumulates coverage with max rather than
    // blending stamp over stamp.
    LineworkContent content;
    LineStroke stroke = horizontalStroke();
    stroke.colour = StraightRgba8{0, 0, 0, 100};      // semi-transparent
    content.strokes.push_back(stroke);
    Document doc = lineworkDocument(content);
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    const StraightRgba8 once = pickColour(doc, 64, 64);
    CHECK(once.a > 0);

    // The same line, made to pass over its own middle a second time.
    content.strokes[0].points.insert(content.strokes[0].points.begin() + 1,
                                     LinePoint{80.0, 64.0, 1.0f});
    content.strokes[0].points.insert(content.strokes[0].points.begin() + 2,
                                     LinePoint{40.0, 64.0, 1.0f});
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    CHECK(pickColour(doc, 64, 64).a == once.a);
}

TEST_CASE("a linework layer refuses paint, like a text layer does") {
    // The protection is free: applyDab, bucketFill, fillSelection,
    // transformRegion and mergeLayerDown all already refuse anything that is
    // not Raster, so a curve cannot be lost to a stroke the next redraw wipes.
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());
    Document doc = lineworkDocument(content);

    UndoRecord rec;
    TouchedTiles touched;
    PaintTarget target{doc.layers[0], rec, touched, doc.width, doc.height};
    Dab dab;
    dab.x        = 64.0;
    dab.y        = 64.0;
    dab.radius   = 10.0f;
    dab.hardness = 1.0f;
    dab.colour   = widen(StraightRgba8{255, 0, 0, 255}.premultiply());
    applyDab(target, dab);
    CHECK(doc.layers[0].tiles.empty());

    CHECK(bucketFill(doc, doc.layers[0].id, 64, 64,
                     StraightRgba8{255, 0, 0, 255}, 8).empty());
}

TEST_CASE("a linework layer round-trips through .sable") {
    LineworkContent content;
    LineStroke curve;
    curve.width         = 9.25f;
    curve.minWidthRatio = 0.35f;
    curve.colour        = StraightRgba8{12, 34, 56, 210};
    curve.closed        = true;
    curve.points.push_back(LinePoint{10.5, 20.25, 0.25f});
    curve.points.push_back(LinePoint{60.0, 90.0, 1.0f});
    curve.points.push_back(LinePoint{100.75, 30.5, 0.5f});
    content.strokes.push_back(curve);

    Document doc = lineworkDocument(content);
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    const std::uint64_t before = hashCanvas(doc);
    REQUIRE(!doc.layers[0].tiles.empty());

    const auto path = scratchFile("sable_linework.sable");
    REQUIRE(saveProject(doc, path).has_value());

    const auto reloaded = loadProject(path);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->layers.size() == 1);
    const Layer& back = reloaded->layers[0];
    CHECK(back.kind == LayerKind::Linework);
    REQUIRE(back.linework.has_value());
    REQUIRE(back.linework->strokes.size() == 1);

    const LineStroke& out = back.linework->strokes[0];
    CHECK(out.colour == curve.colour);
    CHECK(out.width == doctest::Approx(curve.width));
    CHECK(out.minWidthRatio == doctest::Approx(curve.minWidthRatio));
    // A shape that comes back open is a shape the artist has to close again by
    // hand, and the pixels would no longer be what the curves rasterise to.
    CHECK(out.closed == curve.closed);
    REQUIRE(out.points.size() == curve.points.size());
    for (std::size_t i = 0; i < out.points.size(); ++i) {
        CHECK(out.points[i].x == doctest::Approx(curve.points[i].x));
        CHECK(out.points[i].y == doctest::Approx(curve.points[i].y));
        CHECK(out.points[i].pressure == doctest::Approx(curve.points[i].pressure));
    }
    // The pixels are what renders, so they have to survive too — a reader that
    // ignored the curves would still see the finished line art.
    CHECK(hashCanvas(*reloaded) == before);
    std::filesystem::remove(path);
}

TEST_CASE("a linework layer survives the clone the autosave thread is handed") {
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());
    Document doc = lineworkDocument(content);

    const Document copy = cloneDocument(doc);
    CHECK(copy.layers[0].kind == LayerKind::Linework);
    REQUIRE(copy.layers[0].linework.has_value());
    CHECK(*copy.layers[0].linework == content);
}

TEST_CASE("rasterising linework gives up the curves and keeps the picture") {
    LineworkContent content;
    content.strokes.push_back(horizontalStroke());
    Document doc = lineworkDocument(content);
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    const std::uint64_t drawn  = hashCanvas(doc);
    const std::size_t   tiles  = doc.layers[0].tiles.size();

    LayerProps props = propsOf(doc.layers[0]);
    props.linework.reset();
    doc.undo.push(setLayerProps(doc, doc.layers[0].id, props));
    CHECK(doc.layers[0].kind == LayerKind::Raster);   // paint is allowed again
    CHECK(!doc.layers[0].linework.has_value());
    CHECK(doc.layers[0].tiles.size() == tiles);       // the picture stayed
    CHECK(hashCanvas(doc) == drawn);

    doc.undo.undo(doc);
    CHECK(doc.layers[0].kind == LayerKind::Linework);
    REQUIRE(doc.layers[0].linework.has_value());
    CHECK(*doc.layers[0].linework == content);
}

TEST_CASE("linework off the canvas allocates nothing") {
    LineworkContent content;
    LineStroke away;
    away.points.push_back(LinePoint{-4000.0, -4000.0, 1.0f});
    away.points.push_back(LinePoint{-3000.0, -3900.0, 1.0f});
    content.strokes.push_back(away);

    Document doc = lineworkDocument(content);
    (void)drawLineworkLayer(doc.layers[0], content, doc.width, doc.height);
    CHECK(doc.layers[0].tiles.empty());
}

TEST_CASE("a curve through unevenly spaced points does not loop") {
    // Uniform Catmull-Rom answers uneven spacing with a loop, and pen input is
    // never evenly spaced. Centripetal is what keeps the line where the artist
    // put it — checked by asking that no sample leave the box the control
    // points sit in, which a loop or an overshoot does.
    LineStroke stroke;
    stroke.points.push_back(LinePoint{10.0, 100.0, 1.0f});
    stroke.points.push_back(LinePoint{11.0, 100.0, 1.0f});    // very close
    stroke.points.push_back(LinePoint{200.0, 100.0, 1.0f});   // then far away
    stroke.points.push_back(LinePoint{201.0, 100.0, 1.0f});

    for (const LinePoint& at : samplePoints(stroke, 1.0)) {
        CHECK(at.x >= 9.5);
        CHECK(at.x <= 201.5);
        CHECK(at.y == doctest::Approx(100.0).epsilon(0.001));
    }
}

// ----------------------------------------------------------- layer masks (#48)

namespace {

/// Paints into a layer's MASK with the ordinary brush, which is the whole
/// bargain #48 struck: a mask tile is a tile, so this is `paintSquare` with one
/// flag flipped and no second paint path anywhere behind it.
void paintMaskSquare(Document& doc, LayerId id, StraightRgba8 grey, double x,
                     double y, float size = 40.0f) {
    Layer* layer = doc.layerById(id);
    REQUIRE(layer != nullptr);
    REQUIRE(layer->mask.has_value());
    BrushPreset p = defaultPencil();
    p.size     = size;
    p.hardness = 1.0f;
    p.pressure = PressureMapping{};
    p.pressure.toSize = false;

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, p, grey, id);
    PaintTarget target{*layer,     s.pending, s.touched, doc.width,
                       doc.height, nullptr,   nullptr,   true};
    paintSample(s, target, at(x, y), scratch);
    doc.undo.push(std::move(s.pending));
}

}  // namespace

TEST_CASE("a mask hides the layer's pixels without destroying them") {
    Document doc = makeDocument(120, 80, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{255, 0, 0, 255}, 40.0, 40.0);
    const std::size_t painted = doc.layerById(id)->tiles.size();

    // A new mask hides nothing: `outside` is 255 and it has no tiles at all,
    // which is what stops "add a mask" costing a megabyte on a big canvas.
    doc.layerById(id)->mask.emplace();
    CHECK(doc.layerById(id)->mask->tiles.empty());
    CHECK(pickColour(doc, 40, 40) == StraightRgba8{255, 0, 0, 255});

    // Black hides. The pixels are untouched — that is the difference between a
    // mask and the alpha D-027 folded one into.
    paintMaskSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);
    CHECK(pickColour(doc, 40, 40) == StraightRgba8{255, 255, 255, 255});
    CHECK(doc.layerById(id)->tiles.size() == painted);
    CHECK(doc.layerById(id)->find(TileKey{0, 0})->pixel(40, 40).a == 65535);

    // And switching the mask off brings the layer straight back, which is the
    // capability the baked version could not offer at any price.
    doc.layerById(id)->mask->enabled = false;
    CHECK(pickColour(doc, 40, 40) == StraightRgba8{255, 0, 0, 255});
}

TEST_CASE("a mask stroke on a layer with no mask paints nothing at all") {
    // The tool stays switched on when the artist selects another layer, so the
    // alternative is a stroke landing in the pixels of a layer they were not
    // looking at — undoable, and invisible until much later.
    Document doc = makeDocument(60, 60, StraightRgba8{255, 255, 255, 255});
    Layer& layer = *doc.active();
    const std::uint64_t before = hashCanvas(doc);

    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, defaultPencil(), StraightRgba8{0, 0, 0, 255}, layer.id);
    PaintTarget target{layer,      s.pending, s.touched, doc.width,
                       doc.height, nullptr,   nullptr,   true};
    paintSample(s, target, at(30.0, 30.0), scratch);

    CHECK(s.pending.empty());
    CHECK(layer.tiles.empty());
    CHECK(hashCanvas(doc) == before);
}

TEST_CASE("mask coverage scales the layer rather than switching it") {
    // Half coverage is half the layer, not half of it rounded to on or off —
    // this is what a soft-edged mask is made of, and what `clipToBelow` (a
    // boolean borrowing another layer's alpha) has never been able to express.
    Document doc = makeDocument(64, 64, StraightRgba8{0, 0, 0, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{255, 255, 255, 255}, 32.0, 32.0);
    doc.layerById(id)->mask.emplace();
    paintMaskSquare(doc, id, StraightRgba8{128, 128, 128, 255}, 32.0, 32.0);

    CHECK(maskCoverage(*doc.layerById(id)->mask, 32, 32) == 128);
    const StraightRgba8 seen = pickColour(doc, 32, 32);
    CHECK(seen.r == 128);
    CHECK(seen.g == 128);
    CHECK(seen.b == 128);
}

TEST_CASE("pickColour and flatten agree with masks in play") {
    // The agreement `compositeLevel` and `pickLevel` are pinned to, with every
    // shape of mask the compositor has a separate branch for: a masked raster
    // layer, a clipped layer over a masked base — whose clip has to come from
    // the MASKED alpha — and a mask on a folder, which applies to what the
    // group composited to and to nothing else.
    Document doc = makeDocument(90, 70, StraightRgba8{200, 210, 220, 255});
    const LayerId base = doc.activeLayer;
    paintSquare(doc, base, StraightRgba8{255, 0, 0, 255}, 30.0, 30.0);
    doc.layerById(base)->mask.emplace();
    paintMaskSquare(doc, base, StraightRgba8{90, 90, 90, 255}, 36.0, 30.0, 30.0f);

    doc.undo.push(addLayerAbove(doc, base, "Clipped"));
    const LayerId clip = doc.activeLayer;
    paintSquare(doc, clip, StraightRgba8{0, 255, 0, 255}, 40.0, 30.0);
    doc.layerById(clip)->clipToBelow = true;

    doc.undo.push(addLayerAbove(doc, clip, "Group"));
    const LayerId group = doc.activeLayer;
    doc.layerById(group)->kind = LayerKind::Folder;
    doc.layerById(group)->mask.emplace();
    doc.layerById(group)->mask->outside = 200;
    doc.undo.push(addLayerAbove(doc, group, "In group"));
    doc.layerById(doc.activeLayer)->parent = group;
    paintSquare(doc, doc.activeLayer, StraightRgba8{0, 0, 255, 255}, 55.0, 40.0);
    paintMaskSquare(doc, group, StraightRgba8{40, 40, 40, 255}, 60.0, 40.0, 24.0f);

    const std::vector<StraightRgba8> full = flatten(doc);
    for (std::int32_t y = 0; y < doc.height; ++y)
        for (std::int32_t x = 0; x < doc.width; ++x)
            REQUIRE(pickColour(doc, x, y) ==
                    full[static_cast<std::size_t>(y) * doc.width + x]);
}

TEST_CASE("a mask stroke is one undo step, and undoing it is exact") {
    Document doc = makeDocument(80, 80, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{0, 0, 255, 255}, 40.0, 40.0);
    doc.layerById(id)->mask.emplace();
    const std::uint64_t shown = hashCanvas(doc);

    paintMaskSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);
    const std::uint64_t hidden = hashCanvas(doc);
    CHECK(hidden != shown);

    doc.undo.undo(doc);
    CHECK(hashCanvas(doc) == shown);
    // Undoing the first mask stroke on a tile REMOVES it, or the mask stops
    // being sparse in exactly the way D-005 exists to prevent.
    CHECK(doc.layerById(id)->mask->tiles.empty());
    doc.undo.redo(doc);
    CHECK(hashCanvas(doc) == hidden);

    // A second stroke snapshots the tile that is now there, and it costs the
    // budget a whole tile — because a mask tile IS one, which is the entire
    // reason `UndoRecord::memoryBytes` needed no case for it.
    const std::size_t history = doc.undo.memoryBytes();
    paintMaskSquare(doc, id, StraightRgba8{255, 255, 255, 255}, 20.0, 20.0);
    CHECK(doc.undo.memoryBytes() >= history + tileBytes(ColourDepth::Bits8));
}

TEST_CASE("deleting a mask is one undoable step, pixels and all") {
    Document doc = makeDocument(80, 80, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{0, 128, 0, 255}, 40.0, 40.0);
    doc.layerById(id)->mask.emplace();
    doc.layerById(id)->mask->outside = 90;
    paintMaskSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);
    const std::uint64_t masked = hashCanvas(doc);

    doc.undo.push(deleteLayerMask(doc, id));
    CHECK(!doc.layerById(id)->mask.has_value());
    const std::uint64_t bare = hashCanvas(doc);
    CHECK(bare != masked);

    doc.undo.undo(doc);
    REQUIRE(doc.layerById(id)->mask.has_value());
    CHECK(doc.layerById(id)->mask->outside == 90);      // the flags came back
    CHECK(hashCanvas(doc) == masked);                   // and so did the pixels

    doc.undo.redo(doc);
    CHECK(!doc.layerById(id)->mask.has_value());
    CHECK(hashCanvas(doc) == bare);
}

TEST_CASE("an ordinary property change leaves the mask alone") {
    // `applyProps` is what deletes a mask, so every OTHER caller has to carry
    // the current flags through it. If `propsOf` ever stops doing that, renaming
    // a layer silently throws its mask away.
    Document doc = makeDocument(60, 60, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 30.0, 30.0);
    doc.layerById(id)->mask.emplace();
    paintMaskSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 30.0, 30.0);
    const std::size_t tiles = doc.layerById(id)->mask->tiles.size();
    REQUIRE(tiles > 0);

    LayerProps renamed = propsOf(*doc.layerById(id));
    renamed.name = "Renamed";
    doc.undo.push(setLayerProps(doc, id, renamed));
    REQUIRE(doc.layerById(id)->mask.has_value());
    CHECK(doc.layerById(id)->mask->tiles.size() == tiles);
}

TEST_CASE("a mask survives a .sable round trip") {
    // #48's first acceptance criterion: paint, mask, save, reload, same canvas.
    Document doc = makeDocument(300, 200, StraightRgba8{250, 250, 250, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{200, 40, 90, 255}, 140.0, 100.0);
    doc.layerById(id)->mask.emplace();
    doc.layerById(id)->mask->outside = 210;
    paintMaskSquare(doc, id, StraightRgba8{30, 30, 30, 255}, 150.0, 100.0);
    const std::uint64_t before = hashCanvas(doc);

    const auto path = scratchFile("mask_roundtrip.sable");
    REQUIRE(saveProject(doc, path).has_value());
    const auto back = loadProject(path);
    REQUIRE(back.has_value());
    REQUIRE(back->layers.size() == 1);
    REQUIRE(back->layers[0].mask.has_value());
    CHECK(back->layers[0].mask->outside == 210);
    CHECK(back->layers[0].mask->enabled);
    CHECK(hashCanvas(*back) == before);
}

TEST_CASE("cloning and duplicating carry the mask") {
    // cloneDocument is the autosave hand-off (D-013). Without the mask the
    // recovery file opens showing everything the artist masked away, which is
    // a different painting from the one that was lost.
    Document doc = makeDocument(80, 80, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{10, 20, 200, 255}, 40.0, 40.0);
    doc.layerById(id)->mask.emplace();
    paintMaskSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);

    const Document copy = cloneDocument(doc);
    REQUIRE(copy.layers[0].mask.has_value());
    CHECK(hashCanvas(copy) == hashCanvas(doc));

    const UndoRecord duplicated = duplicateLayer(doc, id);
    CHECK(!duplicated.empty());
    REQUIRE(doc.layers.size() == 2);
    CHECK(doc.layers[1].mask.has_value());
}

TEST_CASE("merging down bakes the upper layer's mask into the pixels") {
    // The upper layer stops existing, so its mask has to go somewhere or the
    // merge puts back on screen exactly what the artist masked away.
    Document doc = makeDocument(80, 80, StraightRgba8{255, 255, 255, 255});
    const LayerId base = doc.activeLayer;
    paintSquare(doc, base, StraightRgba8{255, 255, 255, 255}, 40.0, 40.0);
    doc.undo.push(addLayerAbove(doc, base, "Top"));
    const LayerId top = doc.activeLayer;
    paintSquare(doc, top, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);
    doc.layerById(top)->mask.emplace();
    paintMaskSquare(doc, top, StraightRgba8{128, 128, 128, 255}, 40.0, 40.0);

    const std::uint64_t before = hashCanvas(doc);
    doc.undo.push(mergeLayerDown(doc, top));
    REQUIRE(doc.layers.size() == 1);
    CHECK(hashCanvas(doc) == before);          // not one pixel moved
}

TEST_CASE("a mask on a 16-bit document is still an 8-bit mask") {
    // Coverage is eight bits everywhere it is stored, read or exported, so a
    // 16-bit document must not pay double the undo budget for shades of grey
    // nothing can show (D-023's own argument, applied to the mask).
    Document doc = makeDocument(80, 80, StraightRgba8{255, 255, 255, 255},
                                ColourDepth::Bits16);
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{200, 0, 0, 255}, 40.0, 40.0);
    doc.layerById(id)->mask.emplace();
    paintMaskSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);

    const Tile* maskTile = doc.layerById(id)->mask->find(TileKey{0, 0});
    REQUIRE(maskTile != nullptr);
    CHECK(maskTile->depth() == ColourDepth::Bits8);
    CHECK(doc.layerById(id)->find(TileKey{0, 0})->depth() == ColourDepth::Bits16);
    CHECK(pickColour(doc, 40, 40) == StraightRgba8{255, 255, 255, 255});
}

TEST_CASE("an ORA export bakes the mask into the alpha it writes") {
    // OpenRaster has no mask element — a layer is a PNG and nothing else — so
    // the choice is between a file that looks like the painting and one that
    // shows what the artist masked away. The .sable keeps the editable version.
    Document doc = makeDocument(80, 80, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintSquare(doc, id, StraightRgba8{200, 30, 30, 255}, 40.0, 40.0);
    doc.layerById(id)->mask.emplace();
    paintMaskSquare(doc, id, StraightRgba8{0, 0, 0, 255}, 40.0, 40.0);

    const auto path = scratchFile("mask_export.ora");
    REQUIRE(exportDocument(doc, path).has_value());
    const auto back = importDocument(path);
    REQUIRE(back.has_value());
    CHECK(!back->layers[0].mask.has_value());     // nowhere to put one

    // Within a step, not identical: baking multiplies straight alpha once and
    // the compositor scales a premultiplied colour, so the soft edge of the
    // brush rounds the other way on a handful of pixels.
    const std::vector<StraightRgba8> ours   = flatten(doc);
    const std::vector<StraightRgba8> theirs = flatten(*back);
    REQUIRE(ours.size() == theirs.size());
    int worst = 0;
    for (std::size_t i = 0; i < ours.size(); ++i)
        worst = std::max({worst, std::abs(ours[i].r - theirs[i].r),
                          std::abs(ours[i].g - theirs[i].g),
                          std::abs(ours[i].b - theirs[i].b),
                          std::abs(ours[i].a - theirs[i].a)});
    CHECK(worst <= 2);
}
