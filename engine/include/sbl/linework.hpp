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
#include "sbl/paint.hpp"

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
///
/// A closed stroke walks the segment from the last point back to the first as
/// well, and does NOT repeat the start point at the end: the rasteriser takes
/// coverage with max, so a repeat would cost a stamp rather than show, but a
/// caller drawing the samples as a polyline would close the ring twice.
[[nodiscard]] std::vector<LinePoint> samplePoints(const LineStroke& stroke,
                                                  double stepPx = 1.0);

/// Adds `at` to a stroke being drawn freehand, once the pointer has travelled
/// `spacingPx` from the last control point. True when a point was added.
///
/// Here rather than in the tool because it is half of what makes a smoothed
/// freehand curve smooth — the stabiliser removes the wobble, and the spacing
/// decides how many handles the artist is left holding. The two are only
/// meaningful together, and together they are testable without a window.
bool appendFreehand(LineStroke& stroke, const LinePoint& at, double spacingPx);

/// The control point nearest (x, y), if one is within `within` canvas pixels.
/// Later strokes win a tie: they are the ones drawn on top, so they are the
/// ones the artist is pointing at.
[[nodiscard]] std::optional<PointRef> nearestPoint(const LineworkContent& content,
                                                   double x, double y, double within);

/// The stroke whose CURVE passes nearest (x, y), if one is within `within`
/// canvas pixels of it. The whole-stroke answer to `nearestPoint`: an artist
/// grabbing a line to move it aims at the line, not at a handle.
///
/// Later strokes win a tie, for the same reason as `nearestPoint`.
[[nodiscard]] std::optional<std::size_t> nearestStroke(const LineworkContent& content,
                                                       double x, double y, double within);

/// Moves, scales and rotates whole strokes about the centre of their own
/// bounding box, in place. Indices that name no stroke are ignored.
///
/// `Transform` is the one from sbl/paint.hpp: a second transform type would be
/// a second definition of what a rotation angle means, and the day they drift
/// the artist finds out by turning a selection the wrong way.
///
/// The width scales with the geometry. A curve scaled to twice the size that
/// kept a 4 px line is not the same drawing enlarged, it is a different one.
void transformStrokes(LineworkContent& content, const std::vector<std::size_t>& which,
                      const Transform& transform);

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
