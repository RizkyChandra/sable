// Linework layers (#17): editable curves, rasterised into ordinary tiles.
//
// SAI's signature feature, and the same bargain D-026 struck for text — the
// pixels are what renders, the curves are what edits. `LineworkContent` lives
// in sbl/canvas.hpp because it is document data; everything that turns it into
// pixels, or answers a question about its shape, is here.
//
// Nothing in this header knows what a mouse is. Which point the artist grabbed
// is a geometry question, so it is answered here and unit-tested headlessly;
// the app is left with the part only a window can do.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "sbl/canvas.hpp"

namespace sbl {

/// Which control point, in a LineworkContent. Indices rather than pointers:
/// the vectors move when a point is inserted, and a stale pointer into one is
/// the kind of bug that only shows up on the second edit.
struct PointRef {
    std::size_t stroke = 0;
    std::size_t point  = 0;

    friend bool operator==(const PointRef&, const PointRef&) = default;
};

/// Replaces every pixel of `layer` with `content` drawn, and returns the record
/// that undoes it.
///
/// The old tiles are MOVED into the record rather than copied, so re-drawing
/// while a control point is being dragged costs no allocation beyond the new
/// line. Exactly `drawTextLayer`'s shape, for exactly its reasons: writes host
/// tiles directly, so call `PaintBackend::readback` first if a backend may be
/// holding the layer's current pixels.
///
/// Each stroke is accumulated as coverage first and composited once, so a
/// semi-transparent line does not come out darker where the curve doubles back
/// on itself — which is the visible difference between a drawn line and a
/// painted one.
[[nodiscard]] UndoRecord drawLineworkLayer(Layer& layer, const LineworkContent& content,
                                           std::int32_t docWidth, std::int32_t docHeight);

/// Points along a stroke, in canvas pixels, at roughly one per `stepPx`.
///
/// What the rasteriser walks, and what the app draws the on-canvas preview
/// from — one definition of where the curve goes, so the line the artist sees
/// while dragging is the line that gets rasterised.
[[nodiscard]] std::vector<LinePoint> samplePoints(const LineStroke& stroke,
                                                  double stepPx = 1.0);

/// The control point nearest (x, y), if one is within `within` canvas pixels.
/// Later strokes win a tie: they are the ones drawn on top, so they are the
/// ones the artist is pointing at.
[[nodiscard]] std::optional<PointRef> nearestPoint(const LineworkContent& content,
                                                   double x, double y, double within);

/// Splits the segment nearest (x, y) and returns the new point, or nothing when
/// no curve passes within `within` canvas pixels.
///
/// Inserted ON the curve rather than at the click: an added control point that
/// moves the line before it has been dragged is one the artist has to undo.
std::optional<PointRef> insertPoint(LineworkContent& content, double x, double y,
                                    double within);

/// Removes a control point. A stroke left with fewer than two points is removed
/// with it — a curve through one point is a dot the artist cannot see the shape
/// of and cannot get rid of.
///
/// False when the reference names nothing, so a caller need not check first.
bool erasePoint(LineworkContent& content, PointRef ref);

}  // namespace sbl
