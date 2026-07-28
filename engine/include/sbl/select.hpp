// Building selections: lasso, magic wand, and the add/subtract/intersect
// modifiers (#18).
//
// Everything here RETURNS a Selection and changes nothing. The document owns
// which selection is current, and the caller decides whether the new one
// replaces it or is combined with it — a tool that quietly reached into the
// document would be a second place selection state lives.
#pragma once

#include <cstdint>
#include <span>

#include "sbl/canvas.hpp"

namespace sbl {

/// A lasso vertex, in canvas pixels. Sub-pixel deliberately: the anti-aliasing
/// is only worth having if the path feeding it has not been rounded first.
struct Point {
    double x = 0.0, y = 0.0;
};

/// What a newly drawn selection does to the one already there.
enum class SelectMode : std::uint8_t { Replace, Add, Subtract, Intersect };

/// Rasterises a freehand polygon, implicitly closed from the last point back to
/// the first, and clipped to the canvas.
///
/// Non-zero winding, not even-odd: a freehand lasso crosses itself constantly,
/// and the artist means "everything I went round", not "punch a hole wherever
/// my hand wobbled back over the line".
///
/// The edge is anti-aliased — exact horizontal coverage, four samples
/// vertically. Fewer than three points selects nothing.
[[nodiscard]] Selection lassoSelection(std::span<const Point> path,
                                       std::int32_t width, std::int32_t height);

/// The contiguous run of similar colour containing (x, y), as a selection.
///
/// Matched on the COMPOSITE, like the bucket fill and for the same reason: the
/// artist clicks what they can see. It runs the bucket's own `floodRegion`, so
/// the two tools cannot disagree about tolerance or about where a region ends.
///
/// `tolerance` is 0..255 per channel. The staircase the flood returns is
/// softened by one pixel, since "anti-aliased" for a tool whose answer is
/// discrete can only mean a ramp across the boundary.
[[nodiscard]] Selection magicWandSelection(const Document& doc, std::int32_t x,
                                           std::int32_t y, int tolerance);

/// Combines two selections. Coverage is combined per pixel — max, multiply by
/// the inverse, and min — so the anti-aliased edges survive being added to and
/// subtracted from rather than being rounded off at every step.
///
/// The result is trimmed to what it actually covers, and drops its mask when
/// what is left is a plain rectangle: two rectangles intersected are still a
/// rectangle, and should still take the fast path afterwards.
[[nodiscard]] Selection combineSelections(const Selection& current,
                                          const Selection& next, SelectMode mode);

}  // namespace sbl
