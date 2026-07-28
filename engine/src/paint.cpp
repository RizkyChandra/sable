#include "sbl/paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

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
[[nodiscard]] float coverage(double d, float radius, float hardness) noexcept {
    const float feather = std::max(radius * (1.0f - hardness), kAaWidth);
    const float inner   = std::max(radius - feather, 0.0f);
    if (d <= inner)             return 1.0f;
    if (d >= inner + feather)   return 0.0f;
    return 1.0f - static_cast<float>(d - inner) / feather;
}

[[nodiscard]] std::uint8_t scale8(std::uint8_t c, float f) noexcept {
    return static_cast<std::uint8_t>(std::lround(static_cast<float>(c) * f));
}

[[nodiscard]] Dab makeDab(const BrushPreset& p, StraightRgba8 colour,
                          const InputSample& s) noexcept {
    Dab d;
    d.x        = s.x;
    d.y        = s.y;
    d.radius   = radiusFor(p, s.pressure);
    d.density  = densityFor(p, s.pressure);
    d.hardness = std::clamp(p.hardness, 0.0f, 1.0f);
    d.angle    = 0.0f;                     // round brushes only in v1
    d.erase    = p.isEraser;

    // Density is folded into the colour here, not at blend time, so the
    // per-pixel loop stays a multiply and an add.
    const float alpha = d.density * (static_cast<float>(colour.a) / 255.0f);
    d.colour = PremulRgba8{scale8(colour.r, alpha), scale8(colour.g, alpha),
                           scale8(colour.b, alpha),
                           static_cast<std::uint8_t>(std::lround(alpha * 255.0f))};
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

}  // namespace

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
    BrushPreset p;
    p.id   = "marker";
    p.name = "Marker";
    p.size            = 18.0f;
    p.density         = 0.55f;
    p.minDensityRatio = 0.4f;
    p.spacingFactor   = 0.05f;
    p.hardness        = 0.75f;
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

    s.loadedColour = PremulRgba8{};
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

void applyDab(PaintTarget& t, const Dab& dab) noexcept {
    if (t.layer.locked || t.layer.kind != LayerKind::Raster) return;
    if (dab.colour.a == 0 || dab.radius <= 0.0f) return;

    const double r = dab.radius;
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

            PremulRgba8* px = tile->pixels();
            for (std::int32_t y = y0; y <= y1; ++y) {
                const double ddy = (static_cast<double>(y) + 0.5) - dab.y;
                PremulRgba8* row = px + static_cast<std::size_t>(y - oy) * TILE_SIZE;
                for (std::int32_t x = x0; x <= x1; ++x) {
                    if (t.selection != nullptr && !t.selection->contains(x, y)) continue;
                    const double ddx = (static_cast<double>(x) + 0.5) - dab.x;
                    float cov = coverage(std::sqrt(ddx * ddx + ddy * ddy),
                                         dab.radius, dab.hardness);
                    if (cov <= 0.0f) continue;

                    PremulRgba8& dst = row[x - ox];
                    if (t.layer.preserveOpacity)
                        cov *= static_cast<float>(dst.a) / 255.0f;

                    if (dab.erase) {
                        const float keep =
                            1.0f - cov * (static_cast<float>(dab.colour.a) / 255.0f);
                        dst = PremulRgba8{scale8(dst.r, keep), scale8(dst.g, keep),
                                          scale8(dst.b, keep), scale8(dst.a, keep)};
                    } else {
                        const PremulRgba8 src{
                            scale8(dab.colour.r, cov), scale8(dab.colour.g, cov),
                            scale8(dab.colour.b, cov), scale8(dab.colour.a, cov)};
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

    PremulRgba8 under{};
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
                     (1.0f - persistence) * (static_cast<float>(under.a) / 255.0f);

    // Deposit what is carried, thinned by dilution. The first dab of a stroke
    // carries nothing, so a smudge starts by picking up rather than painting.
    const float alpha = std::clamp(dab.density * s.loadedAmount * (1.0f - dilution),
                                   0.0f, 1.0f);
    const StraightRgba8 carried = s.loadedColour.unpremultiply();
    dab.colour = StraightRgba8{carried.r, carried.g, carried.b,
                               static_cast<std::uint8_t>(std::lround(alpha * 255.0f))}
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

    void set(std::int32_t x, std::int32_t y, PremulRgba8 colour) {
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
};

bool withinTolerance(StraightRgba8 a, StraightRgba8 b, int tolerance) noexcept {
    const auto diff = [](std::uint8_t p, std::uint8_t q) {
        return std::abs(static_cast<int>(p) - static_cast<int>(q));
    };
    return diff(a.r, b.r) <= tolerance && diff(a.g, b.g) <= tolerance &&
           diff(a.b, b.b) <= tolerance && diff(a.a, b.a) <= tolerance;
}

}  // namespace

UndoRecord bucketFill(Document& doc, LayerId target, std::int32_t x, std::int32_t y,
                      StraightRgba8 colour, int tolerance) {
    UndoRecord rec;
    Layer* layer = doc.layerById(target);
    if (layer == nullptr || layer->locked || layer->kind != LayerKind::Raster) return rec;
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return rec;

    const Selection* selection =
        doc.selection.has_value() && !doc.selection->empty() ? &*doc.selection : nullptr;
    if (selection != nullptr && !selection->contains(x, y)) return rec;

    // Match against what the artist sees, so a fill inside line art on another
    // layer works. One flatten per fill is affordable; per pixel would not be.
    const std::vector<StraightRgba8> composite = flatten(doc);
    if (composite.empty()) return rec;

    const auto w = doc.width;
    const auto h = doc.height;
    const auto index = [w](std::int32_t px, std::int32_t py) {
        return static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + px;
    };

    const StraightRgba8 seed = composite[index(x, y)];
    const PremulRgba8 fill = colour.premultiply();
    tolerance = std::clamp(tolerance, 0, 255);

    // Already the target colour and nothing to spread into: filling would be a
    // no-op that still costs an undo step.
    if (withinTolerance(seed, colour, 0) && tolerance == 0) return rec;

    rec.label = "Fill";
    TouchedTiles touched;
    PixelWriter writer{*layer, rec, touched};

    // Scanline flood fill: pushes spans rather than pixels, so a 4000 x 4000
    // fill does not put four million entries on the stack.
    std::vector<bool> done(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), false);
    std::vector<std::pair<std::int32_t, std::int32_t>> pending;
    pending.emplace_back(x, y);

    const auto matches = [&](std::int32_t px, std::int32_t py) {
        if (px < 0 || py < 0 || px >= w || py >= h) return false;
        if (done[index(px, py)]) return false;
        if (selection != nullptr && !selection->contains(px, py)) return false;
        return withinTolerance(composite[index(px, py)], seed, tolerance);
    };

    while (!pending.empty()) {
        const auto [sx, sy] = pending.back();
        pending.pop_back();
        if (!matches(sx, sy)) continue;

        std::int32_t left = sx;
        while (matches(left - 1, sy)) --left;
        std::int32_t right = sx;
        while (matches(right + 1, sy)) ++right;

        for (std::int32_t px = left; px <= right; ++px) {
            done[index(px, sy)] = true;
            writer.set(px, sy, fill);
        }
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

    if (rec.tiles.empty()) rec.label.clear();
    return rec;
}

UndoRecord fillSelection(Document& doc, LayerId target, StraightRgba8 colour) {
    UndoRecord rec;
    Layer* layer = doc.layerById(target);
    if (layer == nullptr || layer->locked || layer->kind != LayerKind::Raster) return rec;

    Selection area{0, 0, doc.width, doc.height};
    if (doc.selection.has_value() && !doc.selection->empty()) area = *doc.selection;

    const std::int32_t x0 = std::max(0, area.x);
    const std::int32_t y0 = std::max(0, area.y);
    const std::int32_t x1 = std::min(doc.width, area.x + area.w);
    const std::int32_t y1 = std::min(doc.height, area.y + area.h);
    if (x0 >= x1 || y0 >= y1) return rec;

    rec.label = "Fill selection";
    TouchedTiles touched;
    PixelWriter writer{*layer, rec, touched};
    const PremulRgba8 fill = colour.premultiply();
    for (std::int32_t y = y0; y < y1; ++y)
        for (std::int32_t x = x0; x < x1; ++x) writer.set(x, y, fill);
    return rec;
}

UndoRecord transformRegion(Document& doc, LayerId target, const Selection& source,
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
    std::vector<PremulRgba8> lifted(srcW * srcH);
    for (std::size_t row = 0; row < srcH; ++row) {
        for (std::size_t col = 0; col < srcW; ++col) {
            const std::int32_t px = sx0 + static_cast<std::int32_t>(col);
            const std::int32_t py = sy0 + static_cast<std::int32_t>(row);
            const TileKey key{tileIndex(px), tileIndex(py)};
            if (const Tile* tile = layer->find(key); tile != nullptr)
                lifted[row * srcW + col] =
                    tile->pixel(px - key.first * TILE_SIZE, py - key.second * TILE_SIZE);
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

    // 3. Clear the source. Snapshots happen through the same writer, so the
    // undo record covers the source and the destination alike.
    for (std::int32_t y = sy0; y < sy1; ++y)
        for (std::int32_t x = sx0; x < sx1; ++x) writer.set(x, y, PremulRgba8{});

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

            const auto sample = [&](std::int64_t px, std::int64_t py) -> PremulRgba8 {
                if (px < 0 || py < 0 || px >= static_cast<std::int64_t>(srcW) ||
                    py >= static_cast<std::int64_t>(srcH)) return PremulRgba8{};
                return lifted[static_cast<std::size_t>(py) * srcW +
                              static_cast<std::size_t>(px)];
            };
            const PremulRgba8 p00 = sample(x0, y0);
            const PremulRgba8 p10 = sample(x0 + 1, y0);
            const PremulRgba8 p01 = sample(x0, y0 + 1);
            const PremulRgba8 p11 = sample(x0 + 1, y0 + 1);

            const auto mix = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c,
                                 std::uint8_t d) {
                const double top    = a + (b - a) * tx;
                const double bottom = c + (d - c) * tx;
                return static_cast<std::uint8_t>(
                    std::clamp(std::lround(top + (bottom - top) * ty), 0L, 255L));
            };
            const PremulRgba8 value{mix(p00.r, p10.r, p01.r, p11.r),
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

UndoRecord clearLayer(Layer& layer) {
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
