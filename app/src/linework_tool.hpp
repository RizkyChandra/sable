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
#include "sbl/input.hpp"
#include "sbl/linework.hpp"

/// What a press means. The app decides from the modifier keys; the tool does
/// not know what a modifier is.
///
/// `Select` grabs a whole stroke and `SelectAdd` adds one to what is already
/// selected. They are their own actions rather than another modifier on Draw
/// because Ctrl, Shift and Alt are all spoken for, and a fourth meaning on a
/// key that already has one is a gesture nobody finds.
enum class LineworkAction { Draw, Insert, Erase, Select, SelectAdd };

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

    /// The width, taper, colour and closedness of the selected strokes, and
    /// what to transform them by. Everything the tool panel does that is not a
    /// canvas gesture.
    void drawPanel(sbl::Document& doc);

    [[nodiscard]] bool busy() const noexcept { return dragging_ || drawing_ || moving_; }

    /// The point being dragged, for the canvas to draw differently.
    [[nodiscard]] std::optional<sbl::PointRef> held() const noexcept { return held_; }

    /// Which strokes are selected, for the canvas to draw their handles
    /// differently. Indices into the active layer's `LineworkContent`.
    [[nodiscard]] const std::vector<std::size_t>& selection() const noexcept {
        return selected_;
    }

    /// Tiles whose pixels moved since the last call, for the texture cache.
    [[nodiscard]] std::vector<std::pair<sbl::LayerId, sbl::TileKey>> takeChanged();

    /// Width, taper and colour of the NEXT stroke. The tool panel's business,
    /// and the colour panel's — a stroke copies them when it is created, so
    /// changing one afterwards does not reach back into finished lines. What
    /// changes a finished line is applying these to a selection, which is an
    /// edit and gets its own undo step.
    float width         = 4.0f;
    float minWidthRatio = 0.15f;
    sbl::StraightRgba8 colour{0, 0, 0, 255};

    /// 0..3, the same scale and the same `Stabilizer` the brush uses, so a
    /// level means the same amount of smoothing whichever tool is holding it.
    /// Kept per tool because a line and a brush stroke want different amounts.
    std::uint8_t stabilizerLevel = 0;

    /// Whether a press grabs a whole stroke instead of drawing. The app asks
    /// this to decide which action a press is; the tool never reads a key.
    bool selectMode = false;

private:
    /// Creates a linework layer above the active one, with its own undo step.
    /// NO_LAYER when there is nowhere to put one.
    sbl::LayerId ensureLayer(sbl::Document& doc);
    /// Re-rasterises the layer from its own curves and folds the record into
    /// the session.
    void redraw(sbl::Document& doc);
    /// Opens a gesture-less edit — a panel change — on the active linework
    /// layer. The caller mutates the curves, then calls `release`, which pushes
    /// the one undo step covering the curves and the pixels alike. False when
    /// there is no linework layer to edit.
    bool beginEdit(sbl::Document& doc);
    /// Puts the last control point where the pen actually lifted. Without it a
    /// stabilised stroke stops short by up to the string length plus a point
    /// spacing, which on a short line is most of the line.
    void endFreehand(sbl::Document& doc);

    sbl::LayerId layer_ = sbl::NO_LAYER;
    bool dragging_ = false;             // moving an existing control point
    bool drawing_  = false;             // laying down a new stroke
    bool moving_   = false;             // dragging whole selected strokes
    bool moved_    = false;             // ...and it has actually gone somewhere
    std::optional<sbl::PointRef> held_;
    std::vector<std::size_t> selected_;
    /// Which layer `selected_` indexes. Selecting is not part of the drawing,
    /// so it does not travel with the document — but the indices mean nothing
    /// on another layer's curves, and would move a stroke nobody picked.
    sbl::LayerId selectionLayer_ = sbl::NO_LAYER;

    sbl::Stabilizer stabilizer_;        // positions only, while drawing
    sbl::InputSample lastRaw_;          // where the pointer really is
    double moveX_ = 0.0, moveY_ = 0.0;  // last position of a whole-stroke drag
    float editScale_ = 1.0f;            // panel transform, until Apply
    float editAngle_ = 0.0f;            // degrees

    sbl::UndoRecord session_;           // accumulates the whole gesture
    sbl::LayerProps sessionProps_;      // the layer before the gesture started
    std::vector<std::pair<sbl::LayerId, sbl::TileKey>> changed_;
};
