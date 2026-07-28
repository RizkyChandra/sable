#include "sbl/linework.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace sbl {
namespace {

/// Width of the anti-aliased edge, in pixels. The same constant the brush uses
/// at hardness 1 (`paint.cpp`), because linework and a hard pencil have to look
/// like they came from the same program.
constexpr double kAaWidth = 0.7;

/// A line thinner than this is still drawn a pixel wide. The alternative is a
/// stroke that fades out as pressure drops instead of tapering, which is not
/// what a taper looks like — and it keeps the walk below from needing steps
/// small enough to fill a sub-pixel gap.
constexpr double kMinRadius = 0.5;

/// How far apart the stamps along a curve are. Below `kMinRadius` by enough
/// that consecutive stamps always overlap, which is what stops a taper from
/// coming out dotted.
constexpr double kStepPx = 0.4;

/// Centripetal Catmull-Rom, the exponent on the distance between knots.
/// Centripetal rather than uniform because pen input arrives unevenly spaced,
/// and uniform Catmull-Rom answers uneven spacing with a loop — a curve that
/// visibly departs from the points the artist placed.
constexpr double kAlpha = 0.5;

[[nodiscard]] double knotDelta(const LinePoint& a, const LinePoint& b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    // Never zero: two control points at the same place would otherwise divide
    // by zero in the interpolation below, and a doubled point is something a
    // fast pen produces routinely.
    return std::max(std::pow(std::sqrt(dx * dx + dy * dy), kAlpha), 1e-6);
}

/// One reflected neighbour for an end of the curve. `p + (p - q)` keeps the
/// knot spacing sane where duplicating the endpoint would make it zero.
[[nodiscard]] LinePoint reflect(const LinePoint& p, const LinePoint& q) noexcept {
    return LinePoint{2.0 * p.x - q.x, 2.0 * p.y - q.y, p.pressure};
}

/// Barry-Goldman's pyramid, which is Catmull-Rom written so that non-uniform
/// knots fall out of it rather than being bolted on.
[[nodiscard]] LinePoint interpolate(const LinePoint& p0, const LinePoint& p1,
                                    const LinePoint& p2, const LinePoint& p3,
                                    double u) noexcept {
    const double t0 = 0.0;
    const double t1 = t0 + knotDelta(p0, p1);
    const double t2 = t1 + knotDelta(p1, p2);
    const double t3 = t2 + knotDelta(p2, p3);
    const double t  = t1 + (t2 - t1) * u;

    const auto mix = [](const LinePoint& a, const LinePoint& b, double ta, double tb,
                        double at) {
        const double w = (at - ta) / (tb - ta);
        return LinePoint{a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.pressure};
    };

    const LinePoint a1 = mix(p0, p1, t0, t1, t);
    const LinePoint a2 = mix(p1, p2, t1, t2, t);
    const LinePoint a3 = mix(p2, p3, t2, t3, t);
    const LinePoint b1 = mix(a1, a2, t0, t2, t);
    const LinePoint b2 = mix(a2, a3, t1, t3, t);
    LinePoint c = mix(b1, b2, t1, t2, t);

    // Pressure follows the segment linearly rather than the spline. It is the
    // artist's own reading of how hard they pressed at each end, and a spline
    // through it can overshoot past 1 or below 0 between two points that never
    // did — a taper that gets thicker where nobody pressed harder.
    c.pressure = p1.pressure + (p2.pressure - p1.pressure) * static_cast<float>(u);
    return c;
}

[[nodiscard]] double radiusAt(const LineStroke& stroke, float pressure) noexcept {
    const float ratio = std::clamp(stroke.minWidthRatio, 0.0f, 1.0f);
    const float scale = ratio + (1.0f - ratio) * std::clamp(pressure, 0.0f, 1.0f);
    return std::max(static_cast<double>(stroke.width) * 0.5 * scale, kMinRadius);
}

/// Coverage of a pixel at distance `d` from a stamp centre of `radius`.
[[nodiscard]] double stampCoverage(double d, double radius) noexcept {
    const double inner = std::max(radius - kAaWidth, 0.0);
    if (d <= inner)             return 1.0;
    if (d >= inner + kAaWidth)  return 0.0;
    return 1.0 - (d - inner) / kAaWidth;
}

/// One stroke's coverage, sparse like the tiles it will be composited into.
using CoverageTiles = std::unordered_map<TileKey, std::vector<std::uint8_t>, TileKeyHash>;

/// Stamps a round dab, keeping the GREATER of what is already there.
///
/// Max rather than accumulate: a curve that doubles back on itself, or simply
/// slows down, must not come out darker there. That is the difference between
/// a drawn line and a painted one, and the reason this cannot just call
/// `applyDab` in a loop.
void stamp(CoverageTiles& out, double cx, double cy, double radius,
           std::int32_t docWidth, std::int32_t docHeight) {
    const auto minX = static_cast<std::int32_t>(std::floor(cx - radius));
    const auto maxX = static_cast<std::int32_t>(std::ceil(cx + radius));
    const auto minY = static_cast<std::int32_t>(std::floor(cy - radius));
    const auto maxY = static_cast<std::int32_t>(std::ceil(cy + radius));

    const std::int32_t x0 = std::max<std::int32_t>(minX, 0);
    const std::int32_t x1 = std::min<std::int32_t>(maxX, docWidth  - 1);
    const std::int32_t y0 = std::max<std::int32_t>(minY, 0);
    const std::int32_t y1 = std::min<std::int32_t>(maxY, docHeight - 1);
    if (x0 > x1 || y0 > y1) return;

    for (std::int32_t y = y0; y <= y1; ++y) {
        const double dy = (static_cast<double>(y) + 0.5) - cy;
        for (std::int32_t x = x0; x <= x1; ++x) {
            const double dx = (static_cast<double>(x) + 0.5) - cx;
            const double cov = stampCoverage(std::sqrt(dx * dx + dy * dy), radius);
            if (cov <= 0.0) continue;

            const TileKey key{tileIndex(x), tileIndex(y)};
            auto it = out.find(key);
            if (it == out.end())
                it = out.emplace(key, std::vector<std::uint8_t>(TILE_PIXELS, 0)).first;

            const auto at = static_cast<std::size_t>(y - key.second * TILE_SIZE) *
                                TILE_SIZE +
                            static_cast<std::size_t>(x - key.first * TILE_SIZE);
            const auto value = static_cast<std::uint8_t>(std::lround(cov * 255.0));
            it->second[at] = std::max(it->second[at], value);
        }
    }
}

/// A point on the curve, and which control-point segment it came from.
struct CurveHit {
    std::size_t stroke  = 0;
    std::size_t segment = 0;      // between points[segment] and points[segment + 1]
    LinePoint at;
    double distance = 0.0;
};

/// Walks every stroke's curve looking for the sample nearest (x, y).
[[nodiscard]] std::optional<CurveHit> nearestOnCurve(const LineworkContent& content,
                                                     double x, double y, double within) {
    std::optional<CurveHit> best;
    for (std::size_t s = 0; s < content.strokes.size(); ++s) {
        const LineStroke& stroke = content.strokes[s];
        if (stroke.points.size() < 2) continue;

        for (std::size_t i = 0; i + 1 < stroke.points.size(); ++i) {
            const LinePoint& p1 = stroke.points[i];
            const LinePoint& p2 = stroke.points[i + 1];
            const LinePoint p0 = i == 0 ? reflect(p1, p2) : stroke.points[i - 1];
            const LinePoint p3 = i + 2 < stroke.points.size() ? stroke.points[i + 2]
                                                             : reflect(p2, p1);

            const double chord = std::hypot(p2.x - p1.x, p2.y - p1.y);
            const auto steps = static_cast<int>(
                std::max(1.0, std::ceil(chord * 1.5 / 1.0)));
            for (int k = 0; k <= steps; ++k) {
                const LinePoint at =
                    interpolate(p0, p1, p2, p3, static_cast<double>(k) / steps);
                const double d = std::hypot(at.x - x, at.y - y);
                // <= so that a later stroke wins a tie: it is the one drawn on
                // top, so it is the one under the artist's cursor.
                if (d <= within && (!best.has_value() || d <= best->distance))
                    best = CurveHit{s, i, at, d};
            }
        }
    }
    return best;
}

}  // namespace

std::vector<LinePoint> samplePoints(const LineStroke& stroke, double stepPx) {
    std::vector<LinePoint> out;
    if (stroke.points.empty()) return out;
    if (stroke.points.size() == 1) {
        out.push_back(stroke.points.front());
        return out;
    }

    const double step = std::max(stepPx, 0.05);
    out.push_back(stroke.points.front());
    for (std::size_t i = 0; i + 1 < stroke.points.size(); ++i) {
        const LinePoint& p1 = stroke.points[i];
        const LinePoint& p2 = stroke.points[i + 1];
        const LinePoint p0 = i == 0 ? reflect(p1, p2) : stroke.points[i - 1];
        const LinePoint p3 = i + 2 < stroke.points.size() ? stroke.points[i + 2]
                                                         : reflect(p2, p1);

        // The chord underestimates a curved segment's length, so the 1.5 buys
        // back the slack. Overshooting costs a few stamps that land on pixels
        // already covered; undershooting leaves gaps in the line.
        const double chord = std::hypot(p2.x - p1.x, p2.y - p1.y);
        const auto steps = static_cast<int>(std::max(1.0, std::ceil(chord * 1.5 / step)));
        for (int k = 1; k <= steps; ++k)
            out.push_back(interpolate(p0, p1, p2, p3, static_cast<double>(k) / steps));
    }
    return out;
}

UndoRecord drawLineworkLayer(Layer& layer, const LineworkContent& content,
                             std::int32_t docWidth, std::int32_t docHeight) {
    UndoRecord rec;
    rec.label = "Linework";

    // Every tile goes: the curves own all of them, so "what was here before" is
    // the whole layer. Moved, not cloned — this runs on every drag of a point.
    for (auto& [key, tile] : layer.tiles)
        rec.tiles.push_back(TileSnapshot{layer.id, key, std::move(tile)});
    layer.tiles.clear();

    CoverageTiles coverage;
    for (const LineStroke& stroke : content.strokes) {
        if (stroke.points.empty() || stroke.colour.a == 0) continue;

        coverage.clear();
        for (const LinePoint& at : samplePoints(stroke, kStepPx))
            stamp(coverage, at.x, at.y, radiusAt(stroke, at.pressure),
                  docWidth, docHeight);

        // One composite per stroke, from coverage that was accumulated with
        // max: the line is drawn at its own opacity, once, however many stamps
        // went into it.
        // Widened once, then composited through the depth-agnostic accessors:
        // a linework layer in a 16-bit document must not quietly narrow its
        // own line to 8 bits on the way in. `narrow(widen(c)) == c`, so an
        // 8-bit document is bit-identical to what this did before.
        const PremulRgba16 colour = widen(stroke.colour.premultiply());
        for (const auto& [key, cov] : coverage) {
            Tile& tile = layer.tileFor(key);
            for (int i = 0; i < TILE_PIXELS; ++i) {
                const std::uint8_t c = cov[static_cast<std::size_t>(i)];
                if (c == 0) continue;
                const auto scale = [c](std::uint16_t v) {
                    return static_cast<std::uint16_t>((v * c + 127) / 255);
                };
                const int x = i % TILE_SIZE;
                const int y = i / TILE_SIZE;
                tile.setPixel(x, y,
                              over(PremulRgba16{scale(colour.r), scale(colour.g),
                                                scale(colour.b), scale(colour.a)},
                                   tile.pixel(x, y)));
            }
        }
    }

    // Anything newly created is part of the same change: undoing has to remove
    // it, which is what an empty `before` means.
    const std::size_t existing = rec.tiles.size();
    for (const auto& [key, tile] : layer.tiles) {
        const bool known = std::any_of(
            rec.tiles.begin(), rec.tiles.begin() + static_cast<std::ptrdiff_t>(existing),
            [&](const TileSnapshot& s) { return s.key == key; });
        if (!known) rec.tiles.push_back(TileSnapshot{layer.id, key, std::nullopt});
    }
    return rec;
}

std::optional<PointRef> nearestPoint(const LineworkContent& content, double x, double y,
                                     double within) {
    std::optional<PointRef> best;
    double closest = within;
    for (std::size_t s = 0; s < content.strokes.size(); ++s) {
        const std::vector<LinePoint>& points = content.strokes[s].points;
        for (std::size_t p = 0; p < points.size(); ++p) {
            const double d = std::hypot(points[p].x - x, points[p].y - y);
            if (d <= closest) {
                closest = d;
                best    = PointRef{s, p};
            }
        }
    }
    return best;
}

std::optional<PointRef> insertPoint(LineworkContent& content, double x, double y,
                                    double within) {
    const std::optional<CurveHit> hit = nearestOnCurve(content, x, y, within);
    if (!hit.has_value()) return std::nullopt;

    std::vector<LinePoint>& points = content.strokes[hit->stroke].points;
    const std::size_t at = hit->segment + 1;
    points.insert(points.begin() + static_cast<std::ptrdiff_t>(at), hit->at);
    return PointRef{hit->stroke, at};
}

bool erasePoint(LineworkContent& content, PointRef ref) {
    if (ref.stroke >= content.strokes.size()) return false;
    std::vector<LinePoint>& points = content.strokes[ref.stroke].points;
    if (ref.point >= points.size()) return false;

    points.erase(points.begin() + static_cast<std::ptrdiff_t>(ref.point));
    if (points.size() < 2)
        content.strokes.erase(content.strokes.begin() +
                              static_cast<std::ptrdiff_t>(ref.stroke));
    return true;
}

}  // namespace sbl
