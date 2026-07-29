#include "sbl/paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include "sbl/backend.hpp"
#include "sbl/io.hpp"

namespace sbl {
namespace {

/// Width of the anti-aliased edge on a fully hard brush. Without it a hardness
/// of 1 gives stair-stepped edges (US-02.6); with it the dab's outer extent is
/// still exactly `radius`, so the bounding box below stays honest.
constexpr float kAaWidth = 0.7f;

constexpr float lerpf(float a, float b, float t) noexcept { return a + (b - a) * t; }

[[nodiscard]] float radiusFor(const BrushPreset& p, float pressure) noexcept {
    const float scale = p.pressure.toSize ? lerpf(p.minSizeRatio, 1.0f, pressure) : 1.0f;
    return std::max(p.size * 0.5f * scale, 0.1f);
}

[[nodiscard]] float densityFor(const BrushPreset& p, float pressure) noexcept {
    const float scale = p.pressure.toDensity ? lerpf(p.minDensityRatio, 1.0f, pressure) : 1.0f;
    return std::clamp(p.density * scale, 0.0f, 1.0f);
}

/// Coverage of the pixel at distance `d` from the dab centre. Hard core out to
/// `inner`, linear falloff to `radius`.
///
/// `d` is a float, and is measured in float by the caller. It used to be a
/// double, which cost the two backends their agreement: `dab.comp` has no
/// doubles to measure it with, so nearly every partially covered pixel came
/// out a fraction apart and whichever ones landed near a rounding boundary
/// tipped. D-025 accepted that as one fringe pixel per stroke; a textured dab
/// (D-029) multiplies coverage by a mask and so has far more values sitting
/// near a boundary, which turned it into a whole edge of them. Nothing is lost
/// by measuring in float: the result is quantised to eight bits either way.
[[nodiscard]] float coverage(float d, float radius, float hardness) noexcept {
    const float feather = std::max(radius * (1.0f - hardness), kAaWidth);
    const float inner   = std::max(radius - feather, 0.0f);
    if (d <= inner)             return 1.0f;
    if (d >= inner + feather)   return 0.0f;
    return 1.0f - (d - inner) / feather;
}

static_assert((MASK_SIZE & (MASK_SIZE - 1)) == 0,
              "grain tiles the canvas with a bitmask, so the side has to be a power of two");

/// One texel, and zero outside the mask: a stamp has to fade to nothing at its
/// own border. Clamping the edge row outwards instead would smear whatever
/// happens to be on the mask's rim along the whole bounding box.
[[nodiscard]] float maskTexel(const BrushMask& m, std::int32_t x,
                              std::int32_t y) noexcept {
    if (x < 0 || y < 0 || x >= MASK_SIZE || y >= MASK_SIZE) return 0.0f;
    return static_cast<float>(
        m.coverage[static_cast<std::size_t>(y) * MASK_SIZE + static_cast<std::size_t>(x)]);
}

/// The stamp's coverage at one pixel, bilinear, in the dab's turned frame.
///
/// `float` and not `double`, and every operation in the order `dab.comp`
/// writes it: this is the one new piece of per-pixel arithmetic both backends
/// run, and `tests/differential.cpp` allows their alphas to differ by exactly
/// nothing. Written with `a + (b - a) * t` rather than a `mix`, because GLSL's
/// `mix` is `x * (1 - a) + y * a` and that is a different rounding.
[[nodiscard]] float stampCoverage(const BrushMask& m, float dx, float dy, float radius,
                                  float ca, float sa) noexcept {
    const float u = (dx * ca + dy * sa) / radius;
    const float v = (dy * ca - dx * sa) / radius;
    const float tx = (u * 0.5f + 0.5f) * static_cast<float>(MASK_SIZE) - 0.5f;
    const float ty = (v * 0.5f + 0.5f) * static_cast<float>(MASK_SIZE) - 0.5f;

    const float fx0 = std::floor(tx);
    const float fy0 = std::floor(ty);
    const auto  x0  = static_cast<std::int32_t>(fx0);
    const auto  y0  = static_cast<std::int32_t>(fy0);
    const float fx  = tx - fx0;
    const float fy  = ty - fy0;

    const float top = lerpf(maskTexel(m, x0, y0), maskTexel(m, x0 + 1, y0), fx);
    const float bot = lerpf(maskTexel(m, x0, y0 + 1), maskTexel(m, x0 + 1, y0 + 1), fx);
    return lerpf(top, bot, fy) * (1.0f / 255.0f);
}

/// What grain leaves of a pixel's coverage.
///
/// Sampled in CANVAS space, so two dabs that overlap reveal the same tooth in
/// the same place. Riding with the dab instead would make the texture read as
/// noise the brush emits rather than as paper it is being dragged across —
/// and a slow stroke would then average its own noise back to flat.
///
/// Nearest, not bilinear: the index is integer on both backends, so there is
/// no interpolation for a driver to round differently, and grain wants a tooth
/// rather than a blur anyway.
[[nodiscard]] float grainFactor(const BrushMask& m, std::int32_t x, std::int32_t y,
                                float strength) noexcept {
    const float g =
        static_cast<float>(m.coverage[static_cast<std::size_t>(y & (MASK_SIZE - 1)) *
                                          MASK_SIZE +
                                      static_cast<std::size_t>(x & (MASK_SIZE - 1))]) *
        (1.0f / 255.0f);
    return 1.0f - strength * (1.0f - g);
}

[[nodiscard]] std::uint8_t scale8(std::uint8_t c, float f) noexcept {
    return static_cast<std::uint8_t>(std::lround(static_cast<float>(c) * f));
}

[[nodiscard]] std::uint16_t scale16(std::uint16_t c, float f) noexcept {
    return static_cast<std::uint16_t>(
        std::clamp<long>(std::lround(static_cast<float>(c) * f), 0L, 65535L));
}

/// Not `noexcept`, unlike everything around it: the registry builds its
/// built-in masks on first use, and that allocates. It happens once, before
/// the first dab of the process, and never again — the allocation test
/// (US-02.9) paints before it starts counting for exactly this kind of reason.
[[nodiscard]] Dab makeDab(const BrushPreset& p, StraightRgba8 colour,
                          const InputSample& s) {
    Dab d;
    d.x        = s.x;
    d.y        = s.y;
    d.radius   = radiusFor(p, s.pressure);
    d.density  = densityFor(p, s.pressure);
    d.hardness = std::clamp(p.hardness, 0.0f, 1.0f);
    d.erase    = p.isEraser;

    // Looked up once per dab rather than once per stroke, because the artist
    // may edit the preset mid-stroke and `Stroke::preset` is a copy for that
    // very reason. A miss leaves the pointer null, which is the round,
    // untextured dab — a preset naming a mask this build does not have has to
    // paint, not fail.
    const TextureRegistry& masks = textureRegistry();
    if (p.shape == ShapeId::Stamp) d.stamp = masks.find(p.stampMask);
    if (p.texture.has_value() && p.textureStrength > 0.0f) {
        d.grain         = masks.find(*p.texture);
        d.grainStrength = std::clamp(p.textureStrength, 0.0f, 1.0f);
    }

    // A nib points where the pen leans and turns with the barrel; a round dab
    // has no orientation to get wrong, and leaving it at zero there keeps the
    // symmetry ruler — which mirrors this angle — doing exactly what it did.
    d.angle = d.stamp != nullptr ? std::atan2(s.tiltY, s.tiltX) + s.rotation : 0.0f;

    // Density is folded into the colour here, not at blend time, so the
    // per-pixel loop stays a multiply and an add.
    //
    // Widened FIRST and scaled second. Because `widen` is exact and 65535/257
    // is 255, `narrow(scale16(widen(c), f))` is the old `scale8(c, f)` — so an
    // 8-bit document gets the dab it always got, and a 16-bit one gets the bits
    // that the old order threw away before they could be used.
    const float alpha = d.density * (static_cast<float>(colour.a) / 255.0f);
    const StraightRgba16 wide = widen(colour);
    d.colour = PremulRgba16{scale16(wide.r, alpha), scale16(wide.g, alpha),
                            scale16(wide.b, alpha),
                            static_cast<std::uint16_t>(
                                std::lround(std::clamp(alpha, 0.0f, 1.0f) * 65535.0f))};
    return d;
}

[[nodiscard]] InputSample lerpSample(const InputSample& a, const InputSample& b,
                                     double t) noexcept {
    InputSample s = b;
    s.x        = a.x + (b.x - a.x) * t;
    s.y        = a.y + (b.y - a.y) * t;
    s.pressure = lerpf(a.pressure, b.pressure, static_cast<float>(t));
    s.tiltX    = lerpf(a.tiltX, b.tiltX, static_cast<float>(t));
    s.tiltY    = lerpf(a.tiltY, b.tiltY, static_cast<float>(t));
    return s;
}

// ------------------------------------------------------------------- masks

/// A hash, not a generator: the same texel has to produce the same byte in
/// every process on every machine, because the GPU backend ships these bytes
/// to the device and the differential harness then compares the two pixel for
/// pixel. `<random>`'s distributions are not specified to be portable and its
/// engines are only portable if you never touch a distribution; this is one
/// line and has neither problem.
[[nodiscard]] float hashUnit(std::int32_t x, std::int32_t y) noexcept {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B9u ^
                      static_cast<std::uint32_t>(y) * 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) * (1.0f / 16777215.0f);
}

/// Value noise on a lattice of `cell`-texel squares, WRAPPED at the mask edge.
///
/// Wrapped is the whole point: grain is tiled across the canvas, and a mask
/// whose opposite edges do not meet draws a visible 64-pixel grid over any
/// wide fill — which is the one artefact a paper texture must not have.
///
/// `low` is how dark the deepest pit gets; the peaks always reach 1, so the
/// texture takes coverage away and never adds it.
[[nodiscard]] BrushMask noiseMask(std::string name, std::int32_t cell, float low) {
    BrushMask m;
    m.name = std::move(name);
    m.coverage.resize(static_cast<std::size_t>(MASK_SIZE) * MASK_SIZE);

    const std::int32_t lattice = MASK_SIZE / cell;
    for (std::int32_t y = 0; y < MASK_SIZE; ++y) {
        for (std::int32_t x = 0; x < MASK_SIZE; ++x) {
            const std::int32_t gx = x / cell, gy = y / cell;
            const float tx = static_cast<float>(x % cell) / static_cast<float>(cell);
            const float ty = static_cast<float>(y % cell) / static_cast<float>(cell);
            // Smoothstep rather than linear: a linear lattice leaves creases
            // along its own grid lines, and a grain made of creases is a weave.
            const float sx = tx * tx * (3.0f - 2.0f * tx);
            const float sy = ty * ty * (3.0f - 2.0f * ty);
            const auto at = [&](std::int32_t ix, std::int32_t iy) {
                return hashUnit(ix % lattice, iy % lattice);
            };
            const float top = lerpf(at(gx, gy), at(gx + 1, gy), sx);
            const float bot = lerpf(at(gx, gy + 1), at(gx + 1, gy + 1), sx);
            const float v   = lerpf(top, bot, sy);
            m.coverage[static_cast<std::size_t>(y) * MASK_SIZE +
                       static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(std::lround(lerpf(low, 1.0f, v) * 255.0f));
        }
    }
    return m;
}

/// A flat nib: long axis along the mask's x, short across it, so `Dab::angle`
/// of zero is a horizontal chisel.
///
/// Analytic and soft-edged in the mask itself, for two reasons. There is no
/// mip chain, so a hard edge here crawls when the mask is minified onto a
/// small brush; and the long axis stops at 0.92 rather than 1.0 so that the
/// round falloff — which is still what applyDab multiplies this by — does not
/// clip the tips off the nib at hardness 1.
[[nodiscard]] BrushMask chiselMask() {
    constexpr float kLong  = 0.92f;
    constexpr float kShort = 0.30f;
    constexpr float kEdge  = 0.10f;   // in the ellipse's own units

    BrushMask m;
    m.name = "Chisel";
    m.coverage.resize(static_cast<std::size_t>(MASK_SIZE) * MASK_SIZE);
    for (std::int32_t y = 0; y < MASK_SIZE; ++y) {
        for (std::int32_t x = 0; x < MASK_SIZE; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) * (2.0f / MASK_SIZE) - 1.0f;
            const float v = (static_cast<float>(y) + 0.5f) * (2.0f / MASK_SIZE) - 1.0f;
            const float du = u / kLong, dv = v / kShort;
            const float d  = std::sqrt(du * du + dv * dv);
            const float cov = std::clamp((1.0f - d) / kEdge, 0.0f, 1.0f);
            m.coverage[static_cast<std::size_t>(y) * MASK_SIZE +
                       static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(std::lround(cov * 255.0f));
        }
    }
    return m;
}

}  // namespace

TextureId TextureRegistry::add(BrushMask mask) {
    // Sized to fit rather than refused. The dab loop indexes a mask without
    // checking, and refusing would have to hand back an id that finds nothing
    // — which the next successful `add` would then quietly give to a different
    // mask, so a preset holding the old id would paint with the new one.
    mask.coverage.resize(static_cast<std::size_t>(MASK_SIZE) * MASK_SIZE, 0);
    masks_.push_back(std::move(mask));
    return static_cast<TextureId>(masks_.size() - 1);
}

const BrushMask* TextureRegistry::find(TextureId id) const noexcept {
    return id < masks_.size() ? &masks_[id] : nullptr;
}

TextureRegistry& textureRegistry() {
    static TextureRegistry registry = [] {
        TextureRegistry r;
        // The order IS the TEXTURE_/SHAPE_ constants in paint.hpp.
        r.add(noiseMask("Paper", 2, 0.30f));    // TEXTURE_PAPER
        r.add(noiseMask("Canvas", 8, 0.20f));   // TEXTURE_CANVAS
        r.add(chiselMask());                    // SHAPE_CHISEL
        return r;
    }();
    return registry;
}

// ------------------------------------------------------------------- presets

BrushPreset defaultPencil() {
    BrushPreset p;
    p.id   = "pencil";
    p.name = "Pencil";
    p.size            = 8.0f;
    p.minSizeRatio    = 0.15f;
    p.density         = 1.0f;
    p.minDensityRatio = 0.25f;
    p.spacingFactor   = 0.08f;
    p.hardness        = 0.9f;
    // The tooth is what makes a pencil a pencil: graphite catches the paper's
    // peaks and skips its pits, so a light pass is broken and a heavy one
    // fills in. Without it this preset is the opaque brush at another size.
    p.texture         = TEXTURE_PAPER;
    p.textureStrength = 0.45f;
    p.pressure        = PressureMapping{.toSize = true, .toDensity = true};
    return p;
}

BrushPreset defaultEraser() {
    BrushPreset p = defaultPencil();
    p.id       = "eraser";
    p.name     = "Eraser";
    p.size     = 24.0f;
    p.hardness = 0.8f;
    p.isEraser = true;
    // Deliberately not the pencil's tooth, which came with the copy above: an
    // eraser that leaves speckles of the old colour behind reads as a bug, and
    // "rub harder" is not a fix an artist should have to find.
    p.texture.reset();
    p.textureStrength = 0.0f;
    return p;
}

BrushPreset defaultOpaque() {
    // Flat, fully covering colour — the base-colour brush.
    BrushPreset p;
    p.id   = "opaque";
    p.name = "Opaque";
    p.size            = 30.0f;
    p.minSizeRatio    = 0.4f;
    p.density         = 1.0f;
    p.spacingFactor   = 0.06f;
    p.hardness        = 1.0f;
    p.pressure        = PressureMapping{.toSize = true, .toDensity = false};
    return p;
}

BrushPreset defaultAirbrush() {
    // Soft edge and low per-dab density, so colour builds up with repeated
    // passes rather than arriving all at once.
    BrushPreset p;
    p.id   = "airbrush";
    p.name = "Airbrush";
    p.size            = 60.0f;
    p.minSizeRatio    = 0.7f;
    p.density         = 0.06f;
    p.minDensityRatio = 0.0f;
    p.spacingFactor   = 0.04f;
    p.hardness        = 0.0f;
    p.pressure        = PressureMapping{.toSize = false, .toDensity = true};
    return p;
}

BrushPreset defaultMarker() {
    // Fixed width, pressure only on density — a marker does not get thinner
    // when you press lightly, it gets fainter.
    //
    // A chisel nib, which is what makes it a marker rather than a translucent
    // pencil: the mark is wide across the stroke and thin along it, and it
    // turns with the pen. Hardness is 1 because for a stamp the round falloff
    // is what the mask is cut out of, and a soft one would round the nib off.
    BrushPreset p;
    p.id   = "marker";
    p.name = "Marker";
    p.size            = 18.0f;
    p.density         = 0.55f;
    p.minDensityRatio = 0.4f;
    p.spacingFactor   = 0.05f;
    p.hardness        = 1.0f;
    p.shape           = ShapeId::Stamp;
    p.stampMask       = SHAPE_CHISEL;
    p.pressure        = PressureMapping{.toSize = false, .toDensity = true};
    return p;
}

BrushPreset defaultWatercolour() {
    // Picks colour up off the canvas and carries it. See applyDab's smudge
    // path for what blending, dilution and persistence actually do.
    BrushPreset p;
    p.id   = "watercolour";
    p.name = "Watercolour";
    p.size            = 40.0f;
    p.minSizeRatio    = 0.5f;
    p.density         = 0.35f;
    p.minDensityRatio = 0.2f;
    p.spacingFactor   = 0.05f;
    p.hardness        = 0.15f;
    p.blending        = 0.6f;
    p.dilution        = 0.35f;
    p.persistence     = 0.75f;
    // Coarse weave, not the pencil's fine tooth: wet media pool in the hollows
    // of the paper, at a scale you can see from across the room.
    p.texture         = TEXTURE_CANVAS;
    p.textureStrength = 0.4f;
    p.pressure        = PressureMapping{.toSize = true, .toDensity = true};
    return p;
}

BrushPreset defaultSmudge() {
    // Pure smear: picks up almost everything, deposits it undiluted, and
    // carries it a long way.
    BrushPreset p;
    p.id   = "smudge";
    p.name = "Smudge";
    p.size            = 30.0f;
    p.minSizeRatio    = 0.5f;
    p.density         = 1.0f;
    p.spacingFactor   = 0.03f;
    p.hardness        = 0.3f;
    p.blending        = 0.9f;
    p.dilution        = 0.0f;
    p.persistence     = 0.95f;
    p.pressure        = PressureMapping{.toSize = true, .toDensity = true};
    return p;
}

std::vector<BrushPreset> defaultBrushes() {
    // Implementation order from PRD §11: pencil, eraser, opaque, airbrush,
    // marker, watercolour, smudge.
    return {defaultPencil(), defaultOpaque(), defaultAirbrush(), defaultMarker(),
            defaultWatercolour(), defaultSmudge(), defaultEraser()};
}

// -------------------------------------------------------------------- stroke

void beginStroke(Stroke& s, const BrushPreset& preset, StraightRgba8 colour,
                 LayerId target, std::size_t expectedSamples) {
    s.preset = preset;                    // copied: the artist may edit it mid-stroke
    s.colour = colour;
    s.target = target;

    s.samples.clear();
    s.samples.reserve(expectedSamples);   // the paint path allocates nothing (D-012)
    s.leftoverDistance = 0.0;

    s.pending = UndoRecord{};
    s.pending.label = preset.name;
    s.pending.tiles.reserve(16);
    s.touched.clear();

    s.loadedColour = PremulRgba16{};
    s.loadedAmount = 0.0f;
}

void appendSample(Stroke& s, const InputSample& sample, std::vector<Dab>& out) {
    if (s.samples.empty()) {
        // A click without movement paints one dab — a click never does nothing.
        s.samples.push_back(sample);
        out.push_back(makeDab(s.preset, s.colour, sample));
        s.leftoverDistance = 0.0;
        return;
    }

    const InputSample prev = s.samples.back();
    const double dx   = sample.x - prev.x;
    const double dy   = sample.y - prev.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    s.samples.push_back(sample);
    if (dist <= 0.0) return;

    // `carried` is the distance already travelled toward the next dab. It
    // starts at whatever the previous segment left over — restarting it here
    // is the bug that clumps paint at slow speeds and gaps it at fast ones.
    double carried = s.leftoverDistance;
    double pos     = 0.0;
    float  radius  = radiusFor(s.preset, prev.pressure);

    for (;;) {
        const double need =
            std::max(0.0, spacingFor(radius, s.preset.spacingFactor) - carried);
        if (pos + need > dist) break;
        pos += need;
        carried = 0.0;

        const Dab d = makeDab(s.preset, s.colour, lerpSample(prev, sample, pos / dist));
        radius = d.radius;
        out.push_back(d);
    }

    s.leftoverDistance = carried + (dist - pos);
}

// --------------------------------------------------------------- dab -> tiles

void CpuBackend::applyDab(PaintTarget& t, const Dab& dab) {
    if (t.layer.locked || t.layer.kind != LayerKind::Raster) return;
    if (dab.colour.a == 0 || dab.radius <= 0.0f) return;

    const double r = dab.radius;
    // Turned once per dab, not once per pixel. The centre goes to float here
    // for the same reason `coverage` takes one: the shader only ever sees
    // these two values, so measuring from anything wider measures a different
    // dab and the two backends stop agreeing.
    const float ca   = std::cos(dab.angle);
    const float sa   = std::sin(dab.angle);
    const float dabX = static_cast<float>(dab.x);
    const float dabY = static_cast<float>(dab.y);
    const auto lo = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    const auto hi = [](double v) { return static_cast<std::int32_t>(std::ceil(v)); };

    // Clipped to the canvas: dragging off the edge and back paints only the
    // on-canvas part, with no wrap-around (US-02.4).
    const std::int32_t minX = std::max<std::int32_t>(0, lo(dab.x - r));
    const std::int32_t maxX = std::min<std::int32_t>(t.width  - 1, hi(dab.x + r));
    const std::int32_t minY = std::max<std::int32_t>(0, lo(dab.y - r));
    const std::int32_t maxY = std::min<std::int32_t>(t.height - 1, hi(dab.y + r));
    if (minX > maxX || minY > maxY) return;

    for (std::int32_t ty = tileIndex(minY); ty <= tileIndex(maxY); ++ty) {
        for (std::int32_t tx = tileIndex(minX); tx <= tileIndex(maxX); ++tx) {
            const TileKey key{tx, ty};

            // D-006: copy on FIRST touch only. Later dabs on this tile within
            // the same stroke cost nothing.
            Tile* tile = t.layer.find(key);
            if (t.touched.insert(key).second) {
                TileSnapshot snap;
                snap.layer = t.layer.id;
                snap.key   = key;
                if (tile != nullptr) snap.before.emplace(tile->clone());
                t.undo.tiles.push_back(std::move(snap));
            }
            if (tile == nullptr) {
                if (dab.erase) continue;          // nothing to erase from an empty tile
                tile = &t.layer.tileFor(key);
            }

            const std::int32_t ox = tx * TILE_SIZE;
            const std::int32_t oy = ty * TILE_SIZE;
            const std::int32_t x0 = std::max(minX, ox);
            const std::int32_t x1 = std::min(maxX, ox + TILE_SIZE - 1);
            const std::int32_t y0 = std::max(minY, oy);
            const std::int32_t y1 = std::min(maxY, oy + TILE_SIZE - 1);

            // The coverage geometry is identical at both depths, so it is
            // written once; only the four lines that touch a pixel differ, and
            // they are the two blocks below. D-023 asks for the second
            // implementation written out rather than a template over the
            // channel type, and this is as much of it as actually differs.
            const bool wide = tile->depth() == ColourDepth::Bits16;
            PremulRgba8*  px8  = wide ? nullptr : tile->pixels8();
            PremulRgba16* px16 = wide ? tile->pixels16() : nullptr;
            // One narrow per dab, not per pixel: an 8-bit document pays a
            // handful of nanoseconds a dab and nothing at all per pixel.
            const PremulRgba8 dab8 = narrow(dab.colour);

            for (std::int32_t y = y0; y <= y1; ++y) {
                const float ddy = (static_cast<float>(y) + 0.5f) - dabY;
                const std::size_t rowAt = static_cast<std::size_t>(y - oy) * TILE_SIZE;
                for (std::int32_t x = x0; x <= x1; ++x) {
                    // Scaled by the selection, not gated on it: a lasso edge is
                    // a fraction of a pixel, and rounding it to in-or-out is
                    // what makes a clipped stroke look sawn off. A rectangle
                    // answers 255 everywhere and pays one multiply.
                    float clip = 1.0f;
                    if (t.selection != nullptr) {
                        const std::uint8_t inside = t.selection->coverage(x, y);
                        if (inside == 0) continue;
                        clip = static_cast<float>(inside) * (1.0f / 255.0f);
                    }
                    const float ddx = (static_cast<float>(x) + 0.5f) - dabX;
                    float cov = coverage(std::sqrt(ddx * ddx + ddy * ddy),
                                         dab.radius, dab.hardness) * clip;
                    if (cov <= 0.0f) continue;

                    // Shape first, then grain — the order dab.comp uses. The
                    // stamp is a hole cut in the round falloff rather than a
                    // replacement for it, so hardness keeps meaning what it
                    // means and the bounding box above stays honest: nothing
                    // outside `radius` can be reached either way.
                    if (dab.stamp != nullptr) {
                        cov *= stampCoverage(*dab.stamp, ddx, ddy, dab.radius, ca, sa);
                        if (cov <= 0.0f) continue;
                    }
                    if (dab.grain != nullptr)
                        cov *= grainFactor(*dab.grain, x, y, dab.grainStrength);

                    if (wide) {
                        PremulRgba16& dst = px16[rowAt + static_cast<std::size_t>(x - ox)];
                        if (t.layer.preserveOpacity)
                            cov *= static_cast<float>(dst.a) / 65535.0f;

                        if (dab.erase) {
                            const float keep =
                                1.0f - cov * (static_cast<float>(dab.colour.a) / 65535.0f);
                            dst = PremulRgba16{scale16(dst.r, keep), scale16(dst.g, keep),
                                               scale16(dst.b, keep), scale16(dst.a, keep)};
                        } else {
                            const PremulRgba16 src{
                                scale16(dab.colour.r, cov), scale16(dab.colour.g, cov),
                                scale16(dab.colour.b, cov), scale16(dab.colour.a, cov)};
                            dst = over(src, dst);
                        }
                        continue;
                    }

                    PremulRgba8& dst = px8[rowAt + static_cast<std::size_t>(x - ox)];
                    if (t.layer.preserveOpacity)
                        cov *= static_cast<float>(dst.a) / 255.0f;

                    if (dab.erase) {
                        const float keep =
                            1.0f - cov * (static_cast<float>(dab8.a) / 255.0f);
                        dst = PremulRgba8{scale8(dst.r, keep), scale8(dst.g, keep),
                                          scale8(dst.b, keep), scale8(dst.a, keep)};
                    } else {
                        const PremulRgba8 src{
                            scale8(dab8.r, cov), scale8(dab8.g, cov),
                            scale8(dab8.b, cov), scale8(dab8.a, cov)};
                        dst = over(src, dst);
                    }
                }
            }
        }
    }
}

namespace {

PremulRgba8 lerpPremul(PremulRgba8 a, PremulRgba8 b, float t) noexcept {
    const auto mix = [t](std::uint8_t p, std::uint8_t q) {
        return static_cast<std::uint8_t>(
            std::lround(lerpf(static_cast<float>(p), static_cast<float>(q), t)));
    };
    return PremulRgba8{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

PremulRgba16 lerpPremul(PremulRgba16 a, PremulRgba16 b, float t) noexcept {
    const auto mix = [t](std::uint16_t p, std::uint16_t q) {
        return static_cast<std::uint16_t>(std::clamp<long>(
            std::lround(lerpf(static_cast<float>(p), static_cast<float>(q), t)),
            0L, 65535L));
    };
    return PremulRgba16{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

/// Turns a dab into a smear of colour carried from earlier in the stroke.
///
/// blending    — how much canvas colour under the dab is pulled into the load.
/// dilution    — how much that load is weakened toward transparency.
/// persistence — how slowly the load fades, so a colour travels further.
void loadAndDeposit(Stroke& s, const PaintTarget& t, Dab& dab) noexcept {
    // ponytail: samples the single pixel under the dab centre rather than
    // averaging the disc. Cheap and behaves well; average the disc if smearing
    // over hard edges ever looks steppy.
    const auto px = static_cast<std::int32_t>(std::lround(dab.x));
    const auto py = static_cast<std::int32_t>(std::lround(dab.y));

    PremulRgba16 under{};
    if (px >= 0 && py >= 0 && px < t.width && py < t.height) {
        const TileKey key{tileIndex(px), tileIndex(py)};
        if (const Tile* tile = t.layer.find(key); tile != nullptr)
            under = tile->pixel(px - key.first * TILE_SIZE, py - key.second * TILE_SIZE);
    }

    const float blending    = std::clamp(s.preset.blending, 0.0f, 1.0f);
    const float dilution    = std::clamp(s.preset.dilution, 0.0f, 1.0f);
    const float persistence = std::clamp(s.preset.persistence, 0.0f, 0.999f);

    s.loadedColour = lerpPremul(s.loadedColour, under, blending);
    s.loadedAmount = s.loadedAmount * persistence +
                     (1.0f - persistence) * (static_cast<float>(under.a) / 65535.0f);

    // Deposit what is carried, thinned by dilution. The first dab of a stroke
    // carries nothing, so a smudge starts by picking up rather than painting.
    const float alpha = std::clamp(dab.density * s.loadedAmount * (1.0f - dilution),
                                   0.0f, 1.0f);
    const StraightRgba16 carried = s.loadedColour.unpremultiply();
    dab.colour = StraightRgba16{carried.r, carried.g, carried.b,
                                static_cast<std::uint16_t>(std::lround(alpha * 65535.0f))}
                     .premultiply();
}

}  // namespace

std::size_t paintSample(Stroke& s, PaintTarget& t, const InputSample& sample,
                        std::vector<Dab>& scratch) {
    scratch.clear();
    appendSample(s, sample, scratch);

    const bool smears = s.preset.blending > 0.0f && !s.preset.isEraser;
    for (Dab& d : scratch) {
        if (smears) loadAndDeposit(s, t, d);
        applyDab(t, d);
    }
    return scratch.size();
}

namespace {

/// Writes one pixel into a layer, snapshotting its tile on first touch.
/// Shared by both fills so the undo bookkeeping exists in exactly one place.
struct PixelWriter {
    Layer&        layer;
    UndoRecord&   undo;
    TouchedTiles& touched;

    [[nodiscard]] PremulRgba16 get(std::int32_t x, std::int32_t y) {
        const TileKey key{tileIndex(x), tileIndex(y)};
        const Tile* tile = layer.find(key);
        if (tile == nullptr) return PremulRgba16{};
        return tile->pixel(x - key.first * TILE_SIZE, y - key.second * TILE_SIZE);
    }

    /// Lays `colour` down at `cov`/255 strength. Full coverage is a plain
    /// write — which is what every rectangular selection still gets — and
    /// anything less is the anti-aliased edge of a lasso or a wand fading out
    /// rather than stepping.
    ///
    /// The mix happens at the LAYER's depth, not always at 16 bits: at 16 bits
    /// so a soft edge over 16-bit paint keeps it, and at 8 bits so an 8-bit
    /// document produces the byte it always produced. Fills take an 8-bit
    /// colour either way — it came from the colour picker — so nothing is lost
    /// by widening it on the way in.
    void blendIn(std::int32_t x, std::int32_t y, PremulRgba8 colour,
                 std::uint8_t cov) {
        if (cov == 255) { set(x, y, colour); return; }
        const float t = static_cast<float>(cov) / 255.0f;
        if (layer.depth == ColourDepth::Bits16)
            set(x, y, lerpPremul(get(x, y), widen(colour), t));
        else
            set(x, y, lerpPremul(narrow(get(x, y)), colour, t));
    }

    void set(std::int32_t x, std::int32_t y, PremulRgba16 colour) {
        const TileKey key{tileIndex(x), tileIndex(y)};
        Tile* tile = layer.find(key);
        if (touched.insert(key).second) {
            TileSnapshot snap;
            snap.layer = layer.id;
            snap.key   = key;
            if (tile != nullptr) snap.before.emplace(tile->clone());
            undo.tiles.push_back(std::move(snap));
        }
        if (tile == nullptr) tile = &layer.tileFor(key);
        tile->setPixel(x - key.first * TILE_SIZE, y - key.second * TILE_SIZE, colour);
    }

    /// 8-bit in is always lossless (see Tile::setPixel), so the two fills and
    /// the transform's clear pass keep passing the colour they already have.
    void set(std::int32_t x, std::int32_t y, PremulRgba8 colour) {
        set(x, y, widen(colour));
    }
};

bool withinTolerance(StraightRgba8 a, StraightRgba8 b, int tolerance) noexcept {
    const auto diff = [](std::uint8_t p, std::uint8_t q) {
        return std::abs(static_cast<int>(p) - static_cast<int>(q));
    };
    return diff(a.r, b.r) <= tolerance && diff(a.g, b.g) <= tolerance &&
           diff(a.b, b.b) <= tolerance && diff(a.a, b.a) <= tolerance;
}

}  // namespace

std::vector<bool> floodRegion(const std::vector<StraightRgba8>& composite,
                              std::int32_t width, std::int32_t height,
                              std::int32_t x, std::int32_t y, int tolerance,
                              const Selection* clip) {
    std::vector<bool> done(static_cast<std::size_t>(width) *
                           static_cast<std::size_t>(height), false);
    if (x < 0 || y < 0 || x >= width || y >= height) return done;
    if (composite.size() != done.size()) return done;

    const auto index = [width](std::int32_t px, std::int32_t py) {
        return static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + px;
    };
    const StraightRgba8 seed = composite[index(x, y)];
    tolerance = std::clamp(tolerance, 0, 255);

    const auto matches = [&](std::int32_t px, std::int32_t py) {
        if (px < 0 || py < 0 || px >= width || py >= height) return false;
        if (done[index(px, py)]) return false;
        // Any coverage at all counts as inside, so a soft selection edge does
        // not saw a region in half. For a rectangle this is `contains`.
        if (clip != nullptr && clip->coverage(px, py) == 0) return false;
        return withinTolerance(composite[index(px, py)], seed, tolerance);
    };
    if (!matches(x, y)) return done;

    // Scanline flood fill: pushes spans rather than pixels, so a 4000 x 4000
    // fill does not put four million entries on the stack.
    std::vector<std::pair<std::int32_t, std::int32_t>> pending;
    pending.emplace_back(x, y);

    while (!pending.empty()) {
        const auto [sx, sy] = pending.back();
        pending.pop_back();
        if (!matches(sx, sy)) continue;

        std::int32_t left = sx;
        while (matches(left - 1, sy)) --left;
        std::int32_t right = sx;
        while (matches(right + 1, sy)) ++right;

        for (std::int32_t px = left; px <= right; ++px) done[index(px, sy)] = true;
        // Seed the rows above and below once per contiguous run, not per pixel.
        for (const std::int32_t ny : {sy - 1, sy + 1}) {
            bool inRun = false;
            for (std::int32_t px = left; px <= right; ++px) {
                const bool ok = matches(px, ny);
                if (ok && !inRun) pending.emplace_back(px, ny);
                inRun = ok;
            }
        }
    }
    return done;
}

UndoRecord CpuBackend::bucketFill(Document& doc, LayerId target, std::int32_t x,
                                  std::int32_t y, StraightRgba8 colour, int tolerance) {
    return floodFill(doc, target, x, y, colour, tolerance);
}

UndoRecord PaintBackend::floodFill(Document& doc, LayerId target, std::int32_t x,
                                   std::int32_t y, StraightRgba8 colour, int tolerance) {
    UndoRecord rec;
    Layer* layer = doc.layerById(target);
    if (layer == nullptr || layer->locked || layer->kind != LayerKind::Raster) return rec;
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return rec;

    const Selection* selection =
        doc.selection.has_value() && !doc.selection->empty() ? &*doc.selection : nullptr;
    if (selection != nullptr && !selection->contains(x, y)) return rec;

    // Match against what the artist sees, so a fill inside line art on another
    // layer works. One flatten per fill is affordable; per pixel would not be.
    // *this, not the process default: whichever backend was asked for the fill
    // is the one whose compositing the artist is looking at.
    const std::vector<StraightRgba8> composite = flatten(doc, *this);
    if (composite.empty()) return rec;

    const auto w = doc.width;
    const auto h = doc.height;
    const auto index = [w](std::int32_t px, std::int32_t py) {
        return static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + px;
    };

    const StraightRgba8 seed = composite[index(x, y)];
    const PremulRgba8 fill = colour.premultiply();

    // Already the target colour and nothing to spread into: filling would be a
    // no-op that still costs an undo step.
    if (withinTolerance(seed, colour, 0) && std::clamp(tolerance, 0, 255) == 0)
        return rec;

    // The shared flood — the magic wand runs this same call (#18).
    const std::vector<bool> region =
        floodRegion(composite, w, h, x, y, tolerance, selection);

    rec.label = "Fill";
    TouchedTiles touched;
    PixelWriter writer{*layer, rec, touched};
    for (std::int32_t py = 0; py < h; ++py) {
        for (std::int32_t px = 0; px < w; ++px) {
            if (!region[index(px, py)]) continue;
            writer.blendIn(px, py, fill,
                           selection != nullptr ? selection->coverage(px, py) : 255);
        }
    }

    if (rec.tiles.empty()) rec.label.clear();
    return rec;
}

UndoRecord CpuBackend::fillSelection(Document& doc, LayerId target,
                                     StraightRgba8 colour) {
    UndoRecord rec;
    Layer* layer = doc.layerById(target);
    if (layer == nullptr || layer->locked || layer->kind != LayerKind::Raster) return rec;

    // Referenced, not copied: a mask is up to a byte per canvas pixel, and
    // "fill what is selected" has no business duplicating it.
    const Selection whole{0, 0, doc.width, doc.height};
    const Selection& area = doc.selection.has_value() && !doc.selection->empty()
                                ? *doc.selection : whole;

    const std::int32_t x0 = std::max(0, area.x);
    const std::int32_t y0 = std::max(0, area.y);
    const std::int32_t x1 = std::min(doc.width, area.x + area.w);
    const std::int32_t y1 = std::min(doc.height, area.y + area.h);
    if (x0 >= x1 || y0 >= y1) return rec;

    rec.label = "Fill selection";
    TouchedTiles touched;
    PixelWriter writer{*layer, rec, touched};
    const PremulRgba8 fill = colour.premultiply();
    for (std::int32_t y = y0; y < y1; ++y) {
        for (std::int32_t x = x0; x < x1; ++x) {
            const std::uint8_t cov = area.coverage(x, y);
            if (cov != 0) writer.blendIn(x, y, fill, cov);
        }
    }
    if (rec.tiles.empty()) rec.label.clear();
    return rec;
}

UndoRecord CpuBackend::transformRegion(Document& doc, LayerId target,
                                       const Selection& source,
                                       const Transform& transform) {
    UndoRecord rec;
    Layer* layer = doc.layerById(target);
    if (layer == nullptr || layer->locked || layer->kind != LayerKind::Raster) return rec;
    if (source.empty()) return rec;

    const std::int32_t sx0 = std::max(0, source.x);
    const std::int32_t sy0 = std::max(0, source.y);
    const std::int32_t sx1 = std::min(doc.width,  source.x + source.w);
    const std::int32_t sy1 = std::min(doc.height, source.y + source.h);
    if (sx0 >= sx1 || sy0 >= sy1) return rec;

    const auto srcW = static_cast<std::size_t>(sx1 - sx0);
    const auto srcH = static_cast<std::size_t>(sy1 - sy0);

    // 1. Read the region out first. Everything after this writes.
    //
    // Coverage decides how much of each pixel travels, so a lasso moves its own
    // shape rather than the box around it. A rectangle covers everything it
    // spans and this is the old behaviour exactly.
    // Lifted at 16 bits whatever the layer is. Reading a 16-bit layer at 8
    // would quantise the artist's paint as the price of nudging it — a
    // transform is not supposed to cost precision — and on an 8-bit layer
    // `pixel()` hands back exactly `widen(the stored byte)`, so the round trip
    // through `writer.set` puts the same byte back.
    std::vector<PremulRgba16> lifted(srcW * srcH);
    for (std::size_t row = 0; row < srcH; ++row) {
        for (std::size_t col = 0; col < srcW; ++col) {
            const std::int32_t px = sx0 + static_cast<std::int32_t>(col);
            const std::int32_t py = sy0 + static_cast<std::int32_t>(row);
            const std::uint8_t cov = source.coverage(px, py);
            if (cov == 0) continue;
            const TileKey key{tileIndex(px), tileIndex(py)};
            const Tile* tile = layer->find(key);
            if (tile == nullptr) continue;
            PremulRgba16 c =
                tile->pixel(px - key.first * TILE_SIZE, py - key.second * TILE_SIZE);
            if (cov != 255) {
                const float f = static_cast<float>(cov) / 255.0f;
                c = PremulRgba16{scale16(c.r, f), scale16(c.g, f), scale16(c.b, f),
                                 scale16(c.a, f)};
            }
            lifted[row * srcW + col] = c;
        }
    }

    const double centreX = (static_cast<double>(sx0) + sx1) * 0.5;
    const double centreY = (static_cast<double>(sy0) + sy1) * 0.5;
    const double scaleX = std::abs(transform.scaleX) < 1e-6 ? 1e-6 : transform.scaleX;
    const double scaleY = std::abs(transform.scaleY) < 1e-6 ? 1e-6 : transform.scaleY;
    const double cosA = std::cos(transform.angle);
    const double sinA = std::sin(transform.angle);

    // 2. Where the transformed region lands, from its four corners.
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    for (int corner = 0; corner < 4; ++corner) {
        const double cx = (corner & 1) ? sx1 : sx0;
        const double cy = (corner & 2) ? sy1 : sy0;
        const double ox = (cx - centreX) * scaleX;
        const double oy = (cy - centreY) * scaleY;
        const double rx = ox * cosA - oy * sinA + centreX + transform.dx;
        const double ry = ox * sinA + oy * cosA + centreY + transform.dy;
        if (corner == 0) { minX = maxX = rx; minY = maxY = ry; }
        minX = std::min(minX, rx); maxX = std::max(maxX, rx);
        minY = std::min(minY, ry); maxY = std::max(maxY, ry);
    }

    const std::int32_t dx0 = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(minX)));
    const std::int32_t dy0 = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(minY)));
    const std::int32_t dx1 = std::min<std::int32_t>(doc.width,  static_cast<std::int32_t>(std::ceil(maxX)) + 1);
    const std::int32_t dy1 = std::min<std::int32_t>(doc.height, static_cast<std::int32_t>(std::ceil(maxY)) + 1);

    rec.label = "Transform";
    TouchedTiles touched;
    PixelWriter writer{*layer, rec, touched};

    // 3. Clear the source, by however much of it was lifted. Snapshots happen
    // through the same writer, so the undo record covers the source and the
    // destination alike.
    for (std::int32_t y = sy0; y < sy1; ++y) {
        for (std::int32_t x = sx0; x < sx1; ++x) {
            const std::uint8_t cov = source.coverage(x, y);
            if (cov == 0) continue;
            writer.blendIn(x, y, PremulRgba8{}, cov);
        }
    }

    // 4. Inverse-map each destination pixel back into the lifted buffer.
    const double invCos =  cosA;
    const double invSin = -sinA;
    for (std::int32_t y = dy0; y < dy1; ++y) {
        for (std::int32_t x = dx0; x < dx1; ++x) {
            const double ox = (static_cast<double>(x) + 0.5) - centreX - transform.dx;
            const double oy = (static_cast<double>(y) + 0.5) - centreY - transform.dy;
            const double rx = (ox * invCos - oy * invSin) / scaleX;
            const double ry = (ox * invSin + oy * invCos) / scaleY;

            const double fx = rx + centreX - sx0 - 0.5;
            const double fy = ry + centreY - sy0 - 0.5;
            if (fx < -1.0 || fy < -1.0 ||
                fx > static_cast<double>(srcW) || fy > static_cast<double>(srcH)) continue;

            const auto x0 = static_cast<std::int64_t>(std::floor(fx));
            const auto y0 = static_cast<std::int64_t>(std::floor(fy));
            const double tx = fx - static_cast<double>(x0);
            const double ty = fy - static_cast<double>(y0);

            const auto sample = [&](std::int64_t px, std::int64_t py) -> PremulRgba16 {
                if (px < 0 || py < 0 || px >= static_cast<std::int64_t>(srcW) ||
                    py >= static_cast<std::int64_t>(srcH)) return PremulRgba16{};
                return lifted[static_cast<std::size_t>(py) * srcW +
                              static_cast<std::size_t>(px)];
            };
            const PremulRgba16 p00 = sample(x0, y0);
            const PremulRgba16 p10 = sample(x0 + 1, y0);
            const PremulRgba16 p01 = sample(x0, y0 + 1);
            const PremulRgba16 p11 = sample(x0 + 1, y0 + 1);

            const auto mix = [&](std::uint16_t a, std::uint16_t b, std::uint16_t c,
                                 std::uint16_t d) {
                const double top    = a + (b - a) * tx;
                const double bottom = c + (d - c) * tx;
                return static_cast<std::uint16_t>(
                    std::clamp(std::lround(top + (bottom - top) * ty), 0L, 65535L));
            };
            const PremulRgba16 value{mix(p00.r, p10.r, p01.r, p11.r),
                                     mix(p00.g, p10.g, p01.g, p11.g),
                                     mix(p00.b, p10.b, p01.b, p11.b),
                                     mix(p00.a, p10.a, p01.a, p11.a)};
            if (value.a == 0) continue;      // leave what the clear left behind
            writer.set(x, y, value);
        }
    }

    if (rec.tiles.empty()) rec.label.clear();
    return rec;
}

UndoRecord CpuBackend::clearLayer(Layer& layer) {
    UndoRecord rec;
    rec.label = "Clear";
    rec.tiles.reserve(layer.tiles.size());
    for (auto& [key, tile] : layer.tiles) {
        TileSnapshot snap;
        snap.layer = layer.id;
        snap.key   = key;
        snap.before.emplace(std::move(tile));
        rec.tiles.push_back(std::move(snap));
    }
    layer.tiles.clear();
    return rec;
}

}  // namespace sbl
