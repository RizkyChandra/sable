// The on-canvas linework tool (#17): draw a curve, then re-shape it.
//
// Everything about where a curve goes, which point the artist grabbed and what
// the line looks like belongs to the engine (`sbl/linework.hpp`) and is tested
// headlessly there. What is left here is the part only a window has: which
// button went down, and what a drag means.
//
// The curve is drawn INTO the layer, not over it — same reason as the text tool
// (#1). The only overlay is the control points themselves, which are not part
// of the picture and must never be exported.
#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "sbl/canvas.hpp"
#include "sbl/linework.hpp"

/// What a press means. The app decides from the modifier keys; the tool does
/// not know what a modifier is.
enum class LineworkAction { Draw, Insert, Erase };

class LineworkTool {
public:
    /// The curves on the active layer, or null when it is not a linework layer.
    /// What the canvas draws its handles from.
    [[nodiscard]] static const sbl::LineworkContent* contentOf(const sbl::Document& doc);

    /// Starts a stroke, grabs a control point, or adds or removes one.
    ///
    /// `grabRadius` is in CANVAS pixels — the app divides a screen distance by
    /// the zoom, so the target stays the same size under the artist's hand
    /// however far in they are.
    ///
    /// Creates a linework layer when the active one is not, and pushes its own
    /// undo step for that, exactly as the text tool does: an artist who picks
    /// the tool and draws should get linework, not a refusal.
    void press(sbl::Document& doc, double x, double y, float pressure,
               LineworkAction action, double grabRadius);

    /// Extends the stroke being drawn, or moves the point being dragged.
    void drag(sbl::Document& doc, double x, double y, float pressure);

    /// Ends the stroke or the drag, pushing ONE undo step for the geometry and
    /// the pixels alike. Safe to call when nothing is happening.
    void release(sbl::Document& doc);

    [[nodiscard]] bool busy() const noexcept { return dragging_ || drawing_; }

    /// The point being dragged, for the canvas to draw differently.
    [[nodiscard]] std::optional<sbl::PointRef> held() const noexcept { return held_; }

    /// Tiles whose pixels moved since the last call, for the texture cache.
    [[nodiscard]] std::vector<std::pair<sbl::LayerId, sbl::TileKey>> takeChanged();

    /// Width, taper and colour of the NEXT stroke. The tool panel's business,
    /// and the colour panel's — a stroke copies them when it is created, so
    /// changing one afterwards does not reach back into finished lines.
    float width         = 4.0f;
    float minWidthRatio = 0.15f;
    sbl::StraightRgba8 colour{0, 0, 0, 255};

private:
    /// Creates a linework layer above the active one, with its own undo step.
    /// NO_LAYER when there is nowhere to put one.
    sbl::LayerId ensureLayer(sbl::Document& doc);
    /// Re-rasterises the layer from its own curves and folds the record into
    /// the session.
    void redraw(sbl::Document& doc);

    sbl::LayerId layer_ = sbl::NO_LAYER;
    bool dragging_ = false;             // moving an existing control point
    bool drawing_  = false;             // laying down a new stroke
    std::optional<sbl::PointRef> held_;

    sbl::UndoRecord session_;           // accumulates the whole gesture
    sbl::LayerProps sessionProps_;      // the layer before the gesture started
    std::vector<std::pair<sbl::LayerId, sbl::TileKey>> changed_;
};
