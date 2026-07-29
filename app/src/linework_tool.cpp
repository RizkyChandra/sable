#include "linework_tool.hpp"

#include <algorithm>
#include <numbers>

#include "imgui.h"

namespace {

/// How far the pointer travels before a freehand stroke lays down another
/// control point, in canvas pixels.
///
/// The whole point of linework is a curve with few enough points to re-shape;
/// a control point per motion event would give the artist a hundred handles for
/// one line and nothing worth editing. The curve between them is Catmull-Rom,
/// so this is coarser than it looks.
constexpr double kPointSpacing = 14.0;

/// How close the pen has to lift to where the last control point sits before
/// the closing point is skipped, in canvas pixels. Below this the point would
/// add a handle and move nothing.
constexpr double kEndSnap = 1.0;

}  // namespace

const sbl::LineworkContent* LineworkTool::contentOf(const sbl::Document& doc) {
    const sbl::Layer* layer = doc.layerById(doc.activeLayer);
    if (layer == nullptr || !layer->linework.has_value()) return nullptr;
    return &*layer->linework;
}

sbl::LayerId LineworkTool::ensureLayer(sbl::Document& doc) {
    if (sbl::Layer* active = doc.layerById(doc.activeLayer);
        active != nullptr && active->linework.has_value())
        return active->id;

    sbl::UndoRecord rec = sbl::addLayerAbove(doc, doc.activeLayer, "Linework");
    rec.label = "New linework layer";
    const sbl::LayerId id = doc.activeLayer;      // addLayerAbove made it active

    sbl::Layer* layer = doc.layerById(id);
    if (layer == nullptr) return sbl::NO_LAYER;
    layer->linework.emplace();
    layer->kind = sbl::LayerKind::Linework;

    doc.undo.push(std::move(rec));
    doc.dirty = true;
    return id;
}

void LineworkTool::redraw(sbl::Document& doc) {
    sbl::Layer* layer = doc.layerById(layer_);
    if (layer == nullptr || !layer->linework.has_value()) return;

    sbl::UndoRecord rec = sbl::drawLineworkLayer(*layer, *layer->linework,
                                                 doc.width, doc.height);
    for (const sbl::TileSnapshot& snap : rec.tiles)
        changed_.emplace_back(layer_, snap.key);
    sbl::mergeTileRecord(session_, std::move(rec));
    doc.dirty = true;
}

void LineworkTool::press(sbl::Document& doc, double x, double y, float pressure,
                         LineworkAction action, double grabRadius) {
    release(doc);            // a press that arrives mid-gesture ends the old one

    // Insert and erase need a layer that already has curves; drawing is what
    // makes one.
    if (action != LineworkAction::Draw && contentOf(doc) == nullptr) return;

    layer_ = ensureLayer(doc);
    sbl::Layer* layer = doc.layerById(layer_);
    if (layer == nullptr || !layer->linework.has_value()) { layer_ = sbl::NO_LAYER; return; }

    session_      = sbl::UndoRecord{};
    sessionProps_ = sbl::propsOf(*layer);
    sbl::LineworkContent& content = *layer->linework;

    switch (action) {
        case LineworkAction::Erase: {
            const std::optional<sbl::PointRef> hit =
                sbl::nearestPoint(content, x, y, grabRadius);
            if (!hit.has_value()) { layer_ = sbl::NO_LAYER; return; }
            sbl::erasePoint(content, *hit);
            // Erasing can remove a whole stroke, and every index past it then
            // names a different curve. Cheaper to drop the selection than to
            // renumber it, and an artist who just deleted something is not
            // relying on what was selected before.
            selected_.clear();
            redraw(doc);
            release(doc);                     // a click, not a drag: done here
            return;
        }

        case LineworkAction::Select:
        case LineworkAction::SelectAdd: {
            if (selectionLayer_ != layer_) {
                selected_.clear();
                selectionLayer_ = layer_;
            }
            const std::optional<std::size_t> hit =
                sbl::nearestStroke(content, x, y, grabRadius);
            if (!hit.has_value()) {
                // A press on empty canvas clears the selection, which is how
                // every other selection in the program behaves.
                if (action == LineworkAction::Select) selected_.clear();
                layer_ = sbl::NO_LAYER;
                return;
            }
            if (action == LineworkAction::Select &&
                std::find(selected_.begin(), selected_.end(), *hit) == selected_.end())
                selected_.clear();
            if (std::find(selected_.begin(), selected_.end(), *hit) == selected_.end())
                selected_.push_back(*hit);

            moving_ = true;
            moveX_  = x;
            moveY_  = y;
            return;
        }

        case LineworkAction::Insert: {
            if (!sbl::insertPoint(content, x, y, grabRadius).has_value()) {
                layer_ = sbl::NO_LAYER;
                return;
            }
            redraw(doc);
            release(doc);
            return;
        }

        case LineworkAction::Draw:
            break;
    }

    // A point already under the pointer is the one to move. Re-shaping an
    // existing line has to be the same tool as drawing it, or every correction
    // costs a trip to the toolbar.
    if (const std::optional<sbl::PointRef> hit =
            sbl::nearestPoint(content, x, y, grabRadius);
        hit.has_value()) {
        held_     = hit;
        dragging_ = true;
        return;
    }

    // The stabiliser is per stroke, exactly as it is for the brush: the string
    // has to start slack at pen-down or the first sample drags in from wherever
    // the previous line ended.
    stabilizer_.setLevel(stabilizerLevel);
    stabilizer_.reset();
    lastRaw_ = sbl::InputSample{x, y, pressure};
    const sbl::InputSample first = stabilizer_.apply(lastRaw_);

    sbl::LineStroke stroke;
    stroke.width         = width;
    stroke.minWidthRatio = minWidthRatio;
    stroke.colour        = colour;
    stroke.points.push_back(sbl::LinePoint{first.x, first.y, pressure});
    content.strokes.push_back(std::move(stroke));
    held_    = sbl::PointRef{content.strokes.size() - 1, 0};
    drawing_ = true;
    redraw(doc);
}

void LineworkTool::drag(sbl::Document& doc, double x, double y, float pressure) {
    sbl::Layer* layer = doc.layerById(layer_);
    if (layer == nullptr || !layer->linework.has_value()) return;
    sbl::LineworkContent& content = *layer->linework;

    if (moving_) {
        // Whole strokes travel through the same `Transform` a selection of
        // pixels does, so "move by this much" has one meaning in the program.
        sbl::transformStrokes(content, selected_,
                              sbl::Transform{x - moveX_, y - moveY_, 1.0, 1.0, 0.0});
        moveX_ = x;
        moveY_ = y;
        moved_ = true;
        redraw(doc);
        return;
    }

    if (!held_.has_value() || held_->stroke >= content.strokes.size()) return;
    sbl::LineStroke& stroke = content.strokes[held_->stroke];

    if (dragging_) {
        if (held_->point >= stroke.points.size()) return;
        // Not stabilised: the artist is placing one point where they can see it
        // needs to go, and smoothing a deliberate placement is lag with nothing
        // bought for it.
        stroke.points[held_->point].x = x;
        stroke.points[held_->point].y = y;
        redraw(doc);
        return;
    }
    if (!drawing_ || stroke.points.empty()) return;

    // Freehand: the stabiliser first, exactly where it sits on the paint path,
    // then a new control point every kPointSpacing of SMOOTHED travel. Placing
    // points from the raw pointer would put a handle on every wobble of the
    // hand, which is the one thing a vector line exists to avoid.
    lastRaw_ = sbl::InputSample{x, y, pressure};
    const sbl::InputSample at = stabilizer_.apply(lastRaw_);
    // Only redrawn when a point lands: rasterising the whole curve per motion
    // event costs a pass over the tiles for a line that has not changed shape.
    if (sbl::appendFreehand(stroke, sbl::LinePoint{at.x, at.y, pressure}, kPointSpacing)) {
        held_->point = stroke.points.size() - 1;
        redraw(doc);
    }
}

void LineworkTool::endFreehand(sbl::Document& doc) {
    sbl::Layer* layer = doc.layerById(layer_);
    if (layer == nullptr || !layer->linework.has_value() || !held_.has_value()) return;
    sbl::LineworkContent& content = *layer->linework;
    if (held_->stroke >= content.strokes.size()) return;

    const sbl::InputSample end = stabilizer_.finish(lastRaw_);
    sbl::LineStroke& stroke = content.strokes[held_->stroke];
    if (sbl::appendFreehand(stroke, sbl::LinePoint{end.x, end.y, end.pressure}, kEndSnap))
        held_->point = stroke.points.size() - 1;
}

void LineworkTool::release(sbl::Document& doc) {
    // `moved_`, not `moving_`: a click that only picks a stroke has changed
    // nothing, and an undo step that restores what is already there is one the
    // artist has to press undo twice to get past.
    const bool wasBusy = dragging_ || drawing_ || moved_;
    if (drawing_) endFreehand(doc);
    dragging_ = false;
    drawing_  = false;
    moving_   = false;
    moved_    = false;
    held_.reset();
    if (layer_ == sbl::NO_LAYER) { session_ = sbl::UndoRecord{}; return; }

    if (wasBusy) redraw(doc);        // the last motion, at its final position

    if (!session_.tiles.empty() || wasBusy) {
        session_.label = "Linework";
        // The curves travel with the pixels, in one record: undoing has to put
        // back the shape as well as the line, or the next drag starts from
        // geometry that is not what is on the canvas.
        session_.structure = sbl::LayerStructureDelta{
            sbl::LayerChange::Properties, layer_, 0, std::nullopt, sessionProps_};
        doc.undo.push(std::move(session_));
    }
    session_ = sbl::UndoRecord{};
    layer_   = sbl::NO_LAYER;
}

std::vector<std::pair<sbl::LayerId, sbl::TileKey>> LineworkTool::takeChanged() {
    return std::exchange(changed_, {});
}

bool LineworkTool::beginEdit(sbl::Document& doc) {
    sbl::Layer* layer = doc.layerById(doc.activeLayer);
    if (layer == nullptr || !layer->linework.has_value()) return false;
    layer_        = layer->id;
    session_      = sbl::UndoRecord{};
    sessionProps_ = sbl::propsOf(*layer);
    return true;
}

// --------------------------------------------------------------------- panel

void LineworkTool::drawPanel(sbl::Document& doc) {
    // Drawn every frame the tool is active, which is what makes this the one
    // place that has to notice the artist changed layers.
    if (selectionLayer_ != doc.activeLayer) {
        selected_.clear();
        selectionLayer_ = doc.activeLayer;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##linewidth", &width, 0.5f, 64.0f, "%.1f px");
    ImGui::TextDisabled("line width");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##linetaper", &minWidthRatio, 0.0f, 1.0f, "%.2f");
    ImGui::TextDisabled("width at no pressure");

    int level = stabilizerLevel;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##linestab", &level, 0, 3, "stabilizer %d"))
        stabilizerLevel = static_cast<std::uint8_t>(std::clamp(level, 0, 3));

    ImGui::Checkbox("Select whole strokes", &selectMode);

    // The one thing that is not discoverable by trying it. Three gestures on
    // one button is a lot to remember and nothing to guess.
    if (selectMode) {
        ImGui::TextDisabled("click a line: select it");
        ImGui::TextDisabled("shift+click: add to the selection");
        ImGui::TextDisabled("drag: move what is selected");
    } else {
        ImGui::TextDisabled("drag: draw or move a point");
        ImGui::TextDisabled("ctrl+click: add a point");
        ImGui::TextDisabled("shift+click: remove one");
    }

    const sbl::LineworkContent* content = contentOf(doc);
    if (content == nullptr || selected_.empty()) return;
    // A stale index outlives an undo that removed the stroke it named.
    std::erase_if(selected_,
                  [&](std::size_t s) { return s >= content->strokes.size(); });
    if (selected_.empty()) return;

    ImGui::Separator();
    ImGui::Text("%zu selected", selected_.size());

    // Recolouring after the fact is the whole point of keeping the curves: the
    // stroke already stores its own colour, width and taper, so this is the
    // panel's values written over the selection's — one edit, one undo step
    // covering the curves and the line they were drawn as.
    if (ImGui::Button("Apply width and colour") && beginEdit(doc)) {
        sbl::Layer* layer = doc.layerById(layer_);
        for (const std::size_t s : selected_) {
            sbl::LineStroke& stroke = layer->linework->strokes[s];
            stroke.width         = width;
            stroke.minWidthRatio = minWidthRatio;
            stroke.colour        = colour;
        }
        redraw(doc);
        release(doc);
    }

    bool closed = content->strokes[selected_.front()].closed;
    if (ImGui::Checkbox("Closed", &closed) && beginEdit(doc)) {
        sbl::Layer* layer = doc.layerById(layer_);
        for (const std::size_t s : selected_) layer->linework->strokes[s].closed = closed;
        redraw(doc);
        release(doc);
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##linescale", &editScale_, 0.01f, 0.05f, 20.0f, "scale %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##linerotate", &editAngle_, 1.0f, -180.0f, 180.0f, "rotate %.0f");
    // Applied on the button rather than live, so dragging the slider does not
    // compound one transform onto the last one thirty times a second.
    if (ImGui::Button("Apply transform") && beginEdit(doc)) {
        constexpr double kRadians = std::numbers::pi / 180.0;
        sbl::transformStrokes(*doc.layerById(layer_)->linework, selected_,
                              sbl::Transform{0.0, 0.0, editScale_, editScale_,
                                             editAngle_ * kRadians});
        editScale_ = 1.0f;
        editAngle_ = 0.0f;
        redraw(doc);
        release(doc);
    }
}
