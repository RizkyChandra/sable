// The differential harness (#14): the same input through two PaintBackend
// implementations, compared pixel for pixel.
//
// Why it exists before a GPU kernel does (#13): if two backends disagree, the
// artist paints one thing and saves another, and that is silent corruption of
// the only artefact that matters. A kernel written against this is checked as
// it is written, rather than argued about afterwards.
//
// It compares any two backends, not "CPU vs GPU" — today the candidate is a
// second CpuBackend, which must deviate by exactly nothing, and a deliberately
// wrong backend, which must be caught. See `candidate()` for the one line #13
// changes.
//
// Separate from tests.cpp because it is a suite in its own right, and because
// that file is heavily contended.
#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sbl/backend.hpp"
#include "sbl/gpu.hpp"
#include "sbl/canvas.hpp"
#include "sbl/io.hpp"
#include "sbl/paint.hpp"

using namespace sbl;

namespace {

// ------------------------------------------------------------------ tolerance
//
// Stated here once, with the argument, because a bare number in an assert is
// something people widen when it fails.
//
// COLOUR: +/- 1 per channel. Every blend mode but Normal is computed in fp32
// on straight-alpha channels and rounded once at the end
// (`blendChannel` / `blendOver`, canvas.cpp). A GPU doing the same arithmetic
// in the same precision still reassociates it, contracts a multiply-add, or
// flushes a denormal, and where the exact result sits near a rounding boundary
// that moves the 8-bit answer by one step. One step of 255 is a quarter of a
// percent and is not visible; demanding zero would mean demanding a particular
// instruction schedule from a driver we do not control.
//
// ALPHA: zero. Exact, no tolerance at all — for two reasons.
//
//  1. An alpha error compounds. Colour is rounded once per composite and the
//     errors do not accumulate in a fixed direction; alpha multiplies through
//     every layer above it, so being one step light on a mask is one step
//     light on everything drawn through it, over and over, and it shows up as
//     a fringe long before anyone can point at which layer caused it.
//  2. Nothing in the reference forces a rounding choice on a backend here.
//     Alpha never goes through `blendChannel`. Under Normal it is pure
//     integer: `src.a + mul255(dst.a, 255 - src.a)`. Under every other mode it
//     is Porter-Duff `ao = as + ab * (1 - as)`, one multiply and two adds. Both
//     forms agree with round-to-nearest fp32 for all 65 536 input pairs — the
//     first test below proves it rather than asserting it. So a backend that
//     cannot reproduce alpha exactly is not losing a last bit; it is doing
//     different arithmetic, and that is a bug to fix, not a tolerance to widen.
//
// CPU vs CPU must of course be zero on both. The tolerances are the budget a
// *different* implementation is allowed, and the harness reports the worst
// deviation it saw whether or not it is inside them — when #13 lands, that
// number is how anyone judges whether a kernel is right.
constexpr int kColourTolerance = 1;
constexpr int kAlphaTolerance  = 0;

// ----------------------------------------------------------------- deviation

/// Worst per-channel deviation between two backends over one scenario.
struct Deviation {
    std::array<int, 4> worst{};      // R, G, B, A
    std::size_t differing = 0;       // pixels not bit-identical
    std::size_t pixels    = 0;
    std::size_t worstAt   = 0;       // index of the worst colour deviation

    [[nodiscard]] int colour() const noexcept {
        return std::max({worst[0], worst[1], worst[2]});
    }
    [[nodiscard]] int alpha() const noexcept { return worst[3]; }
    [[nodiscard]] bool acceptable() const noexcept {
        return colour() <= kColourTolerance && alpha() <= kAlphaTolerance;
    }
    void mergeWorst(const Deviation& other) noexcept {
        for (std::size_t i = 0; i < worst.size(); ++i)
            worst[i] = std::max(worst[i], other.worst[i]);
        differing += other.differing;
        pixels    += other.pixels;
    }
};

/// Compared PREMULTIPLIED, which is the space the engine computes in.
/// Unpremultiplying first would scale a last-bit difference by 255/alpha and
/// report a thirty-step deviation on a pixel that is three percent opaque —
/// a number that says nothing about the kernel that produced it.
Deviation compare(const std::vector<PremulRgba8>& reference,
                  const std::vector<PremulRgba8>& candidate) {
    REQUIRE(reference.size() == candidate.size());
    Deviation dev;
    dev.pixels = reference.size();
    int worstColour = -1;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const PremulRgba8 a = reference[i];
        const PremulRgba8 b = candidate[i];
        const std::array<int, 4> delta{
            std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
            std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
            std::abs(static_cast<int>(a.b) - static_cast<int>(b.b)),
            std::abs(static_cast<int>(a.a) - static_cast<int>(b.a)),
        };
        bool any = false;
        for (std::size_t c = 0; c < delta.size(); ++c) {
            dev.worst[c] = std::max(dev.worst[c], delta[c]);
            any = any || delta[c] != 0;
        }
        const int colour = std::max({delta[0], delta[1], delta[2]});
        if (colour > worstColour) {
            worstColour = colour;
            dev.worstAt = i;
        }
        if (any) ++dev.differing;
    }
    return dev;
}

std::string describe(const Deviation& dev, std::int32_t width) {
    const auto n = [](int v) { return std::to_string(v); };
    std::string text = "worst dR=" + n(dev.worst[0]) + " dG=" + n(dev.worst[1]) +
                       " dB=" + n(dev.worst[2]) + " dA=" + n(dev.worst[3]) +
                       ", " + std::to_string(dev.differing) + "/" +
                       std::to_string(dev.pixels) + " px differ";
    if (dev.differing != 0 && width > 0) {
        const auto w = static_cast<std::size_t>(width);
        text += ", worst at (" + std::to_string(dev.worstAt % w) + ", " +
                std::to_string(dev.worstAt / w) + ")";
    }
    return text;
}

// ------------------------------------------------------------------ backends

/// The default backend is process-wide state, so putting it back is not
/// optional. Scenario code reaches the backend two ways: `PaintTarget::backend`
/// where the API takes one, and the process default where it does not —
/// `bucketFill` and friends have no per-call backend.
struct InstalledBackend {
    explicit InstalledBackend(PaintBackend& b) noexcept { setPaintBackend(&b); }
    ~InstalledBackend() { setPaintBackend(nullptr); }
    InstalledBackend(const InstalledBackend&)            = delete;
    InstalledBackend& operator=(const InstalledBackend&) = delete;
};

/// The backend under test. The reference is always `cpuBackend()` (D-021).
///
/// When #13 lands this one line becomes `static GpuBackend backend;` and
/// nothing else in this file changes.
/// The backend under test. #13 landed, so this is the GPU one when a device
/// can be created.
///
/// When it cannot — no Vulkan driver, no SPIR-V support, CI — this returns
/// null rather than quietly falling back to `cpuBackend()`. A CPU-versus-CPU
/// run reports zero deviation and proves nothing, and a harness that passes
/// while testing nothing is worse than one that fails: every scenario below
/// SKIPs loudly instead.
PaintBackend* candidate() {
    static std::string why;
    static std::unique_ptr<PaintBackend> backend = makeGpuBackend(&why);
    static bool announced = false;
    if (backend == nullptr && !announced) {
        announced = true;
        MESSAGE("no GPU backend, differential scenarios SKIPPED: "
                << (why.empty() ? "no device" : why));
    }
    return backend.get();
}

/// Wrong on purpose, by a settable amount, and only in the two places a real
/// backend would be wrong: what it writes, and what it hands back when read.
///
/// This is how the harness is mutation-tested. A comparison that has never
/// failed has not been shown to work.
class PerturbedBackend final : public PaintBackend {
public:
    int dabRedBias         = 0;    // steps subtracted from every dab's red
    int compositeRedBias   = 0;    // steps subtracted from every composited red
    int compositeAlphaBias = 0;    // ...and from every composited alpha

    [[nodiscard]] std::string_view name() const noexcept override { return "perturbed"; }

    void applyDab(PaintTarget& t, const Dab& dab) override {
        Dab wrong = dab;
        // `Dab::colour` is 16-bit (D-023) while the bias is quoted in the
        // 8-bit steps this file's tolerances are written in, so the mutation is
        // scaled to keep meaning exactly what it meant: `dabRedBias` steps of
        // the eight-bit ladder, not of the sixteen-bit one.
        wrong.colour.r = down16(wrong.colour.r, dabRedBias * 257);
        cpuBackend().applyDab(t, wrong);
    }
    UndoRecord bucketFill(Document& doc, LayerId target, std::int32_t x, std::int32_t y,
                          StraightRgba8 colour, int tolerance) override {
        return cpuBackend().bucketFill(doc, target, x, y, colour, tolerance);
    }
    UndoRecord fillSelection(Document& doc, LayerId target, StraightRgba8 c) override {
        return cpuBackend().fillSelection(doc, target, c);
    }
    UndoRecord transformRegion(Document& doc, LayerId target, const Selection& source,
                               const Transform& transform) override {
        return cpuBackend().transformRegion(doc, target, source, transform);
    }
    UndoRecord clearLayer(Layer& layer) override { return cpuBackend().clearLayer(layer); }
    UndoRecord mergeLayerDown(Document& doc, LayerId id) override {
        return cpuBackend().mergeLayerDown(doc, id);
    }
    std::vector<PremulRgba8> compositeRect(const Document& doc, std::int32_t x,
                                           std::int32_t y, std::int32_t w,
                                           std::int32_t h) override {
        std::vector<PremulRgba8> px = cpuBackend().compositeRect(doc, x, y, w, h);
        for (PremulRgba8& p : px) {
            p.r = down(p.r, compositeRedBias);
            p.a = down(p.a, compositeAlphaBias);
        }
        return px;
    }
    StraightRgba8 pickColour(const Document& doc, std::int32_t x,
                             std::int32_t y) override {
        return cpuBackend().pickColour(doc, x, y);
    }
    std::expected<void, Error> readback(const Document& doc) override {
        return cpuBackend().readback(doc);
    }

private:
    /// Down rather than up: an opaque white canvas is already at 255, and a
    /// perturbation that saturates is a perturbation that cannot be seen.
    static std::uint8_t down(std::uint8_t v, int by) noexcept {
        return static_cast<std::uint8_t>(std::max(0, static_cast<int>(v) - by));
    }
    static std::uint16_t down16(std::uint16_t v, int by) noexcept {
        return static_cast<std::uint16_t>(std::max(0, static_cast<int>(v) - by));
    }
};

// ----------------------------------------------------------------- scenarios

InputSample at(double x, double y, float pressure = 1.0f) {
    InputSample s;
    s.x = x;
    s.y = y;
    s.pressure = pressure;
    return s;
}

void paintStroke(Document& doc, PaintBackend& backend, LayerId id,
                 const BrushPreset& preset, StraightRgba8 colour,
                 std::initializer_list<InputSample> path) {
    Layer* layer = doc.layerById(id);
    REQUIRE(layer != nullptr);
    Stroke s;
    std::vector<Dab> scratch;
    beginStroke(s, preset, colour, id);
    PaintTarget target{*layer,     s.pending, s.touched, doc.width,
                       doc.height, nullptr,   &backend};
    for (const InputSample& sample : path) paintSample(s, target, sample, scratch);
    doc.undo.push(std::move(s.pending));
}

BrushPreset softBrush(float size, float hardness) {
    BrushPreset p = defaultPencil();
    p.size     = size;
    p.hardness = hardness;
    return p;
}

/// A stroke whose pressure varies as it crosses a tile edge.
///
/// 384 wide puts the edge at x = 256, halfway along. The spacing carry-over
/// across a segment boundary is the most fragile arithmetic in the engine (see
/// "dabs are evenly spaced across segment boundaries" in tests.cpp), and a
/// backend that restarts the walk per tile — or per dispatch — shows up here
/// and almost nowhere else. The pressures and sample positions are deliberately
/// not multiples of the spacing.
Document tileBoundaryStroke(PaintBackend& b) {
    Document doc = makeDocument(384, 128, StraightRgba8{255, 255, 255, 255});
    BrushPreset p = softBrush(23.0f, 0.35f);
    p.spacingFactor      = 0.13f;
    p.pressure.toSize    = true;
    p.pressure.toDensity = true;
    paintStroke(doc, b, doc.activeLayer, p, StraightRgba8{20, 40, 180, 255},
                {at(181.3, 41.7, 0.11f), at(233.9, 52.1, 0.57f), at(268.4, 63.3, 0.98f),
                 at(301.7, 71.8, 0.42f), at(349.1, 88.6, 0.16f)});
    return doc;
}

/// One blend mode over the full alpha sweep.
///
/// The sweep from "blending never produces colour brighter than its own alpha"
/// (tests.cpp), laid out as a document: backdrop alpha steps down the image,
/// source alpha steps across it, so a single composite covers all 16 x 16
/// combinations of the two — including both degenerate ends, which is where the
/// dodge and burn guards live. Run over an opaque and over a transparent
/// backdrop, because they take different paths through the compositor.
Document alphaSweep(BlendMode mode, bool opaqueBackdrop, PaintBackend&) {
    constexpr std::int32_t kCell  = 8;
    constexpr std::int32_t kSteps = 16;
    Document doc = makeDocument(kCell * kSteps, kCell * kSteps,
                                opaqueBackdrop ? StraightRgba8{255, 255, 255, 255}
                                               : StraightRgba8{0, 0, 0, 0});
    const LayerId backdrop = doc.activeLayer;
    doc.undo.push(addLayerAbove(doc, backdrop, "source"));
    const LayerId source = doc.activeLayer;
    doc.layerById(source)->blend = mode;

    for (std::int32_t i = 0; i < kSteps; ++i) {
        const auto a = static_cast<std::uint8_t>(i * 17);
        doc.selection = Selection{0, i * kCell, doc.width, kCell};
        doc.undo.push(fillSelection(doc, backdrop, StraightRgba8{0, 200, 255, a}));
        doc.selection = Selection{i * kCell, 0, kCell, doc.height};
        doc.undo.push(fillSelection(doc, source, StraightRgba8{255, 128, 0, a}));
    }
    doc.selection.reset();
    return doc;
}

/// A clipping group inside nested folders, with opacity on both folders.
///
/// A folder composites its children into a scratch buffer and blends that as a
/// unit, so this is the one scenario where a backend has to get an intermediate
/// surface right as well as the final one — and the clip mask is carried
/// between siblings, which is state a parallel backend has to reproduce in
/// order.
Document clippingInNestedFolders(PaintBackend& b) {
    Document doc = makeDocument(128, 128, StraightRgba8{245, 245, 240, 255});
    const LayerId back = doc.activeLayer;
    paintStroke(doc, b, back, softBrush(70.0f, 0.5f), StraightRgba8{40, 120, 60, 255},
                {at(20.0, 100.0), at(110.0, 96.0)});

    doc.undo.push(addLayerAbove(doc, back, "outer"));
    const LayerId outer = doc.activeLayer;
    doc.layerById(outer)->kind    = LayerKind::Folder;
    doc.layerById(outer)->opacity = 0.8f;
    doc.layerById(outer)->blend   = BlendMode::Multiply;

    doc.undo.push(addLayerAbove(doc, outer, "inner"));
    const LayerId inner = doc.activeLayer;
    doc.layerById(inner)->kind    = LayerKind::Folder;
    doc.layerById(inner)->parent  = outer;
    doc.layerById(inner)->opacity = 0.65f;

    doc.undo.push(addLayerAbove(doc, inner, "base"));
    const LayerId base = doc.activeLayer;
    doc.layerById(base)->parent = inner;
    paintStroke(doc, b, base, softBrush(48.0f, 0.25f), StraightRgba8{230, 60, 30, 255},
                {at(36.0, 44.0, 0.6f), at(84.0, 58.0, 1.0f)});

    doc.undo.push(addLayerAbove(doc, base, "shade"));
    const LayerId shade = doc.activeLayer;
    doc.layerById(shade)->parent      = inner;
    doc.layerById(shade)->clipToBelow = true;
    doc.layerById(shade)->blend       = BlendMode::HardLight;
    // Painted right across the canvas, well past the base: everything outside
    // the base's alpha must be masked away, at every partial coverage in
    // between.
    paintStroke(doc, b, shade, softBrush(60.0f, 0.9f), StraightRgba8{20, 40, 200, 200},
                {at(0.0, 50.0), at(128.0, 52.0)});
    return doc;
}

/// Eraser strokes, which take the other branch of applyDab entirely: they
/// scale alpha down rather than compositing colour over it, and a soft eraser
/// leaves every partial alpha between the two.
Document eraserStroke(PaintBackend& b) {
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintStroke(doc, b, id, softBrush(64.0f, 0.8f), StraightRgba8{30, 60, 120, 255},
                {at(28.0, 64.0), at(100.0, 64.0)});

    BrushPreset rubber = defaultEraser();
    rubber.size     = 27.0f;
    rubber.hardness = 0.3f;
    paintStroke(doc, b, id, rubber, StraightRgba8{0, 0, 0, 255},
                {at(41.0, 48.0, 0.35f), at(92.0, 79.0, 0.95f)});
    return doc;
}

/// preserveOpacity, which multiplies every dab's coverage by the alpha already
/// under it — so the layer's silhouette has to survive the stroke exactly.
Document preserveOpacityStroke(PaintBackend& b) {
    Document doc = makeDocument(128, 128, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintStroke(doc, b, id, softBrush(56.0f, 0.4f), StraightRgba8{200, 200, 80, 255},
                {at(40.0, 64.0), at(88.0, 64.0)});
    doc.layerById(id)->preserveOpacity = true;
    paintStroke(doc, b, id, softBrush(34.0f, 0.6f), StraightRgba8{10, 20, 200, 255},
                {at(8.0, 30.0, 0.7f), at(120.0, 96.0, 0.5f)});
    return doc;
}

/// Bucket fill with a tolerance, clicked at a region boundary.
///
/// The soft stroke below gives a band of every intermediate alpha to match
/// against, and the click sits in it, so the region's extent is decided by
/// pixels sitting right on the tolerance threshold. This is the case where a
/// one-step disagreement does not stay one step: it flips whole spans in or out
/// of the region. That is exactly why it is worth running.
Document bucketFillAtBoundary(PaintBackend& b) {
    Document doc = makeDocument(160, 128, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintStroke(doc, b, id, softBrush(74.0f, 0.15f), StraightRgba8{25, 25, 25, 255},
                {at(80.0, 20.0), at(80.0, 108.0)});
    doc.undo.push(bucketFill(doc, id, 118, 64, StraightRgba8{240, 90, 10, 255}, 48));
    return doc;
}

/// Transform with rotation and a non-uniform scale — bilinear sampling on
/// premultiplied values, which is the most float-heavy path in the engine and
/// the one where a GPU's own sampler is most tempting and most likely to
/// disagree.
Document rotatedNonUniformTransform(PaintBackend& b) {
    Document doc = makeDocument(160, 160, StraightRgba8{255, 255, 255, 255});
    const LayerId id = doc.activeLayer;
    paintStroke(doc, b, id, softBrush(40.0f, 0.55f), StraightRgba8{180, 40, 140, 255},
                {at(50.0, 60.0), at(96.0, 66.0), at(92.0, 104.0)});
    doc.undo.push(transformRegion(doc, id, Selection{30, 40, 84, 80},
                                  Transform{.dx     = 11.5,
                                            .dy     = -6.25,
                                            .scaleX = 1.7,
                                            .scaleY = 0.6,
                                            .angle  = 0.41}));
    return doc;
}

struct Case {
    std::string name;
    std::function<Document(PaintBackend&)> build;
};

/// Built once. Every blend mode comes from ALL_BLEND_MODES rather than a list
/// written out here, so a fourteenth mode is covered the day it is added.
const std::vector<Case>& cases() {
    static const std::vector<Case> all = [] {
        std::vector<Case> v;
        v.push_back({"stroke with varying pressure across a tile boundary",
                     tileBoundaryStroke});
        for (const BlendMode mode : ALL_BLEND_MODES) {
            for (const bool opaque : {true, false}) {
                v.push_back({std::string(blendModeName(mode)) + ", alpha sweep over " +
                                 (opaque ? "an opaque" : "a transparent") + " backdrop",
                             [mode, opaque](PaintBackend& b) {
                                 return alphaSweep(mode, opaque, b);
                             }});
            }
        }
        v.push_back({"clipping group inside nested folders", clippingInNestedFolders});
        v.push_back({"eraser stroke", eraserStroke});
        v.push_back({"stroke on a preserveOpacity layer", preserveOpacityStroke});
        v.push_back({"bucket fill with tolerance at a region boundary",
                     bucketFillAtBoundary});
        v.push_back({"transform with rotation and non-uniform scale",
                     rotatedNonUniformTransform});
        return v;
    }();
    return all;
}

/// Builds the scenario through `backend` and returns what it composites.
std::vector<PremulRgba8> run(PaintBackend& backend, const Case& c,
                             std::int32_t& width) {
    const InstalledBackend installed{backend};
    Document doc = c.build(backend);
    width = doc.width;
    // A backend holding pixels on a device brings them home first, exactly as
    // save and autosave do.
    REQUIRE(backend.readback(doc).has_value());
    std::vector<PremulRgba8> px =
        backend.compositeRect(doc, 0, 0, doc.width, doc.height);
    // A backend that quietly failed would otherwise be judged on the pixels it
    // did not draw.
    const std::optional<Error> failure = backend.takeError();
    CHECK_MESSAGE(!failure.has_value(), c.name);
    return px;
}

Deviation differ(PaintBackend& reference, PaintBackend& subject, const Case& c,
                 std::int32_t& width) {
    std::int32_t refWidth = 0;
    const std::vector<PremulRgba8> a = run(reference, c, refWidth);
    const std::vector<PremulRgba8> b = run(subject, c, width);
    CHECK(refWidth == width);
    // The way a differential harness lies to you: a scenario that quietly
    // painted nothing compares equal and reports zero deviation for ever.
    CHECK_MESSAGE(std::ranges::any_of(a, [&](PremulRgba8 p) { return !(p == a.front()); }),
                  "scenario produced a uniform canvas: " << c.name);
    return compare(a, b);
}

}  // namespace

// ---------------------------------------------------------------------- tests

TEST_CASE("the alpha tolerance's premise: 255-scaling is exact in float too") {
    // kAlphaTolerance is zero because the reference's alpha arithmetic asks
    // nothing of a backend that fp32 cannot deliver. That claim is checkable,
    // so check it rather than believing it: over all 65 536 pairs, both the
    // integer round-to-nearest form and the Porter-Duff float form agree with
    // each other and with lround(). If this ever fails, the tolerance argument
    // above is what has to change — not the number.
    for (int c = 0; c <= 255; ++c) {
        for (int a = 0; a <= 255; ++a) {
            const auto uc = static_cast<std::uint8_t>(c);
            const auto ua = static_cast<std::uint8_t>(a);

            // mul255, reached through the one public caller that is pure.
            const int scaled = StraightRgba8{uc, uc, uc, ua}.premultiply().r;
            REQUIRE(scaled == std::lround(static_cast<float>(c) *
                                          static_cast<float>(a) / 255.0f));

            // Source-over alpha, integer, against ao = as + ab * (1 - as).
            const PremulRgba8 src{0, 0, 0, ua};
            const PremulRgba8 dst{0, 0, 0, uc};
            const float as = static_cast<float>(a) / 255.0f;
            const float ab = static_cast<float>(c) / 255.0f;
            REQUIRE(static_cast<int>(over(src, dst).a) ==
                    std::lround((as + ab * (1.0f - as)) * 255.0f));
        }
    }
}

TEST_CASE("the reference and the candidate backend agree on every scenario") {
    // The acceptance criterion, and the number that matters when #13 lands:
    // not pass or fail, but how far apart the two backends were at their worst.
    PaintBackend* subject = candidate();
    if (subject == nullptr) {
        // Skipping is the honest outcome with no device. Comparing the CPU
        // against itself would report a perfect score for a backend nobody ran.
        MESSAGE("no GPU device — every scenario skipped, nothing was compared");
        return;
    }

    Deviation total;
    std::size_t worstCase = 0;
    for (std::size_t i = 0; i < cases().size(); ++i) {
        const Case& c = cases()[i];
        std::int32_t width = 0;
        const Deviation dev = differ(cpuBackend(), *subject, c, width);
        if (dev.colour() > total.colour() || dev.alpha() > total.alpha()) worstCase = i;
        total.mergeWorst(dev);

        CHECK_MESSAGE(dev.colour() <= kColourTolerance,
                      c.name << ": " << describe(dev, width));
        CHECK_MESSAGE(dev.alpha() <= kAlphaTolerance,
                      c.name << ": " << describe(dev, width));
        // Quiet when identical; a scenario that deviates at all is worth
        // reading even when it is inside tolerance.
        if (dev.differing != 0)
            MESSAGE(c.name << ": " << describe(dev, width));
    }
    MESSAGE("differential: " << cases().size() << " scenarios, "
                             << cpuBackend().name() << " vs " << subject->name()
                             << " — worst per-channel deviation R/G/B/A "
                             << total.worst[0] << "/" << total.worst[1] << "/"
                             << total.worst[2] << "/" << total.worst[3]
                             << " (tolerance: colour " << kColourTolerance << ", alpha "
                             << kAlphaTolerance << "), " << total.differing << "/"
                             << total.pixels << " px differ"
                             << (total.differing == 0
                                     ? std::string{}
                                     : ", worst in \"" + cases()[worstCase].name + "\""));

    // The determinism check this file has always carried, updated for the
    // candidate no longer being a second copy of the reference.
    //
    // It used to read `total.colour() == 0`, which was exact because the
    // candidate WAS `cpuBackend()` — the same code over the same integers. A
    // GPU candidate is allowed the one step `kColourTolerance` names, so that
    // form now contradicts the tolerance twenty lines above rather than
    // measuring anything. The property worth keeping is the one it was
    // protecting: a harness whose numbers move between runs is a harness whose
    // numbers are worthless. So the candidate is run against ITSELF, where the
    // answer must still be exactly zero — and for a GPU that is the stronger
    // check, because an uninitialised arena slot or a missing barrier shows up
    // here and nowhere else.
    for (const Case& c : cases()) {
        std::int32_t width = 0;
        const Deviation twice = differ(*subject, *subject, c, width);
        CHECK_MESSAGE(twice.colour() == 0, c.name << ": " << describe(twice, width));
        CHECK_MESSAGE(twice.alpha() == 0, c.name << ": " << describe(twice, width));
    }
}

TEST_CASE("the harness catches a backend that composites two steps off") {
    // Mutation test. Two steps, not one: a one-step error is inside the colour
    // tolerance by design, so a mutant that small would prove nothing about
    // whether the comparison works.
    PerturbedBackend wrong;
    wrong.compositeRedBias = 2;
    for (const Case& c : cases()) {
        std::int32_t width = 0;
        const Deviation dev = differ(cpuBackend(), wrong, c, width);
        CHECK_MESSAGE(!dev.acceptable(), "undetected in " << c.name);
        CHECK_MESSAGE(dev.colour() >= 2, c.name << ": " << describe(dev, width));
    }
}

TEST_CASE("the harness catches a backend that paints two steps off") {
    // The other end of the pipeline: a writer that is wrong, seen through a
    // compositor that is right. Only the scenarios that lay down dabs can show
    // this, so it runs on the one that matters most.
    PerturbedBackend wrong;
    wrong.dabRedBias = 2;
    const Case& c = cases().front();
    REQUIRE(c.name.starts_with("stroke with varying pressure"));
    std::int32_t width = 0;
    const Deviation dev = differ(cpuBackend(), wrong, c, width);
    CHECK_MESSAGE(!dev.acceptable(), "undetected in " << c.name);
    MESSAGE("mutant (dab red -2): " << describe(dev, width));
}

TEST_CASE("the harness catches a one-step alpha error the colour rule would pass") {
    // Why alpha is treated separately. This mutant is inside the colour
    // tolerance in every channel it touches — one step — and it must still be a
    // hard failure, because that step compounds through every composite above
    // it.
    PerturbedBackend wrong;
    wrong.compositeAlphaBias = 1;
    std::size_t detected = 0;
    for (const Case& c : cases()) {
        std::int32_t width = 0;
        const Deviation dev = differ(cpuBackend(), wrong, c, width);
        CHECK(dev.colour() <= kColourTolerance);   // colour alone would say "fine"
        if (!dev.acceptable()) ++detected;
    }
    CHECK(detected == cases().size());
}
