#include "sbl/select.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "sbl/io.hpp"
#include "sbl/paint.hpp"

namespace sbl {
namespace {

/// Vertical supersamples per pixel row for the lasso. Horizontal coverage is
/// exact, so four rows is already better than the eye can pick out on a
/// diagonal — and it costs four edge walks per row rather than sixteen
/// point-in-polygon tests per pixel.
constexpr int kSubRows = 4;

[[nodiscard]] std::uint8_t toByte(float v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
}

/// Trims a masked selection to the pixels it actually covers, and drops the
/// mask entirely when what remains is a solid rectangle.
///
/// This is what keeps the fast path reachable after a combine: without it every
/// selection an artist ever touches with a modifier stays a mask for the rest of
/// the session, including the rectangles.
[[nodiscard]] Selection tighten(const Selection& in) {
    if (in.mask.empty() || in.empty()) return in;

    std::int32_t minX = in.w, minY = in.h, maxX = -1, maxY = -1;
    for (std::int32_t row = 0; row < in.h; ++row) {
        const std::uint8_t* line =
            in.mask.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(in.w);
        for (std::int32_t col = 0; col < in.w; ++col) {
            if (line[col] == 0) continue;
            minX = std::min(minX, col);
            maxX = std::max(maxX, col);
            minY = std::min(minY, row);
            maxY = std::max(maxY, row);
        }
    }
    if (maxX < 0) return Selection{};      // combined away to nothing

    Selection out;
    out.x = in.x + minX;
    out.y = in.y + minY;
    out.w = maxX - minX + 1;
    out.h = maxY - minY + 1;
    out.mask.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h));

    bool solid = true;
    for (std::int32_t row = 0; row < out.h; ++row) {
        for (std::int32_t col = 0; col < out.w; ++col) {
            const std::uint8_t v =
                in.mask[static_cast<std::size_t>(row + minY) * static_cast<std::size_t>(in.w) +
                        static_cast<std::size_t>(col + minX)];
            out.mask[static_cast<std::size_t>(row) * static_cast<std::size_t>(out.w) +
                     static_cast<std::size_t>(col)] = v;
            solid = solid && v == 255;
        }
    }
    if (solid) out.mask.clear();
    return out;
}

/// Adds the horizontal span [a, b) to one row of coverage, weighted, with the
/// partial pixels at each end carrying their exact fraction.
void addSpan(std::vector<float>& row, double a, double b, float weight) {
    a = std::max(a, 0.0);
    b = std::min(b, static_cast<double>(row.size()));
    if (b <= a) return;

    const auto first = static_cast<std::int32_t>(std::floor(a));
    const auto last  = static_cast<std::int32_t>(std::ceil(b)) - 1;
    for (std::int32_t px = first; px <= last; ++px) {
        const double lo = std::max(a, static_cast<double>(px));
        const double hi = std::min(b, static_cast<double>(px) + 1.0);
        if (hi > lo) row[static_cast<std::size_t>(px)] += weight * static_cast<float>(hi - lo);
    }
}

}  // namespace

// -------------------------------------------------------------------- lasso

Selection lassoSelection(std::span<const Point> path, std::int32_t width,
                         std::int32_t height) {
    if (path.size() < 3 || width <= 0 || height <= 0) return Selection{};

    double loX = path[0].x, hiX = path[0].x, loY = path[0].y, hiY = path[0].y;
    for (const Point& p : path) {
        loX = std::min(loX, p.x); hiX = std::max(hiX, p.x);
        loY = std::min(loY, p.y); hiY = std::max(hiY, p.y);
    }

    // Clipped to the canvas: a lasso dragged off the edge selects the part with
    // pixels behind it and nothing else.
    const auto x0 = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(loX)));
    const auto y0 = std::max<std::int32_t>(0, static_cast<std::int32_t>(std::floor(loY)));
    const auto x1 = std::min<std::int32_t>(width,  static_cast<std::int32_t>(std::ceil(hiX)) + 1);
    const auto y1 = std::min<std::int32_t>(height, static_cast<std::int32_t>(std::ceil(hiY)) + 1);
    if (x0 >= x1 || y0 >= y1) return Selection{};

    Selection out;
    out.x = x0;
    out.y = y0;
    out.w = x1 - x0;
    out.h = y1 - y0;
    out.mask.assign(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h), 0);

    // x of the crossing, and which way the edge was going through it.
    std::vector<std::pair<double, int>> crossings;
    std::vector<float> acc(static_cast<std::size_t>(out.w));

    for (std::int32_t row = 0; row < out.h; ++row) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int sub = 0; sub < kSubRows; ++sub) {
            const double sy = static_cast<double>(y0 + row) +
                              (static_cast<double>(sub) + 0.5) / kSubRows;
            crossings.clear();
            for (std::size_t i = 0; i < path.size(); ++i) {
                const Point& a = path[i];
                const Point& b = path[(i + 1) % path.size()];   // implicitly closed
                // Half-open in y, so a vertex shared by two edges is counted
                // once — the classic source of a one-pixel bleed out of a
                // scanline fill.
                if (sy < std::min(a.y, b.y) || sy >= std::max(a.y, b.y)) continue;
                const double t = (sy - a.y) / (b.y - a.y);
                crossings.emplace_back(a.x + t * (b.x - a.x), b.y > a.y ? 1 : -1);
            }
            if (crossings.size() < 2) continue;
            std::sort(crossings.begin(), crossings.end());

            int winding = 0;
            for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
                winding += crossings[i].second;
                if (winding == 0) continue;
                addSpan(acc, crossings[i].first - x0, crossings[i + 1].first - x0,
                        1.0f / kSubRows);
            }
        }
        for (std::int32_t col = 0; col < out.w; ++col)
            out.mask[static_cast<std::size_t>(row) * static_cast<std::size_t>(out.w) +
                     static_cast<std::size_t>(col)] = toByte(acc[static_cast<std::size_t>(col)]);
    }
    return tighten(out);
}

// --------------------------------------------------------------- magic wand

Selection magicWandSelection(const Document& doc, std::int32_t x, std::int32_t y,
                             int tolerance) {
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return Selection{};

    const std::vector<StraightRgba8> composite = flatten(doc);
    if (composite.empty()) return Selection{};

    const std::vector<bool> region =
        floodRegion(composite, doc.width, doc.height, x, y, tolerance, nullptr);

    // The region's own box, grown by one so the softened boundary has
    // somewhere to land. Bounded rather than canvas-sized because a wand click
    // on a small area should not cost a pass over a 4000 x 4000 document.
    std::int32_t minX = doc.width, minY = doc.height, maxX = -1, maxY = -1;
    for (std::int32_t py = 0; py < doc.height; ++py) {
        for (std::int32_t px = 0; px < doc.width; ++px) {
            if (!region[static_cast<std::size_t>(py) *
                        static_cast<std::size_t>(doc.width) +
                        static_cast<std::size_t>(px)]) continue;
            minX = std::min(minX, px); maxX = std::max(maxX, px);
            minY = std::min(minY, py); maxY = std::max(maxY, py);
        }
    }
    if (maxX < 0) return Selection{};

    Selection out;
    out.x = std::max(0, minX - 1);
    out.y = std::max(0, minY - 1);
    out.w = std::min(doc.width  - 1, maxX + 1) - out.x + 1;
    out.h = std::min(doc.height - 1, maxY + 1) - out.y + 1;
    out.mask.assign(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h), 0);

    // Explicit -> bool: std::vector<bool> hands back a proxy reference, not a
    // bool, so without this the two returns have different types. libstdc++
    // converts and libc++ does not, which made this compile on Linux and fail
    // on macOS.
    const auto inside = [&](std::int32_t px, std::int32_t py) -> bool {
        if (px < 0 || py < 0 || px >= doc.width || py >= doc.height) return false;
        return region[static_cast<std::size_t>(py) * static_cast<std::size_t>(doc.width) +
                      static_cast<std::size_t>(px)];
    };

    // Solid inside, with a one-pixel ramp on the way OUT. The flood answers in
    // whole pixels, so pretending to know sub-pixel coverage of a region it
    // never measured would be a lie; what a soft rim honestly buys is a fill
    // that fades at the boundary instead of stepping.
    //
    // Scaled by sixteenths, so the rim can never reach 128 whatever the local
    // shape: `contains()` therefore agrees with `floodRegion` pixel for pixel,
    // and the wand and the bucket cannot part company at a corner.
    for (std::int32_t row = 0; row < out.h; ++row) {
        for (std::int32_t col = 0; col < out.w; ++col) {
            const std::int32_t px = out.x + col;
            const std::int32_t py = out.y + row;
            std::uint8_t v = 255;
            if (!inside(px, py)) {
                int near = 0;
                for (std::int32_t dy = -1; dy <= 1; ++dy)
                    for (std::int32_t dx = -1; dx <= 1; ++dx)
                        if ((dx != 0 || dy != 0) && inside(px + dx, py + dy)) ++near;
                v = static_cast<std::uint8_t>(near * 255 / 16);
            }
            out.mask[static_cast<std::size_t>(row) * static_cast<std::size_t>(out.w) +
                     static_cast<std::size_t>(col)] = v;
        }
    }
    return tighten(out);
}

// ----------------------------------------------------------------- modifiers

Selection combineSelections(const Selection& current, const Selection& next,
                            SelectMode mode) {
    if (mode == SelectMode::Replace) return next;
    if (current.empty())
        return mode == SelectMode::Add ? next : Selection{};
    if (next.empty())
        return mode == SelectMode::Intersect ? Selection{} : current;

    Selection out;
    if (mode == SelectMode::Add) {
        out.x = std::min(current.x, next.x);
        out.y = std::min(current.y, next.y);
        out.w = std::max(current.x + current.w, next.x + next.w) - out.x;
        out.h = std::max(current.y + current.h, next.y + next.h) - out.y;
    } else {
        // Subtracting and intersecting can only ever shrink what is already
        // selected, so there is no reason to walk outside it.
        out.x = current.x;
        out.y = current.y;
        out.w = current.w;
        out.h = current.h;
    }
    out.mask.assign(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h), 0);

    for (std::int32_t row = 0; row < out.h; ++row) {
        for (std::int32_t col = 0; col < out.w; ++col) {
            const std::int32_t px = out.x + col;
            const std::int32_t py = out.y + row;
            const int a = current.coverage(px, py);
            const int b = next.coverage(px, py);
            int v = 0;
            switch (mode) {
                case SelectMode::Add:       v = std::max(a, b); break;
                case SelectMode::Subtract:  v = a * (255 - b) / 255; break;
                case SelectMode::Intersect: v = std::min(a, b); break;
                case SelectMode::Replace:   break;      // handled above
            }
            out.mask[static_cast<std::size_t>(row) * static_cast<std::size_t>(out.w) +
                     static_cast<std::size_t>(col)] = static_cast<std::uint8_t>(v);
        }
    }
    return tighten(out);
}

}  // namespace sbl
