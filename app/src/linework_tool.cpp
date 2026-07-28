#include "linework_tool.hpp"

#include <cmath>

namespace {

/// How far the pointer travels before a freehand stroke lays down another
/// control point, in canvas pixels.
///
/// The whole point of linework is a curve with few enough points to re-shape;
/// a control point per motion event would give the artist a hundred handles for
/// one line and nothing worth editing. The curve between them is Catmull-Rom,
/// so this is coarser than it looks.
constexpr double kPointSpacing = 14.0;

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
            redraw(doc);
            release(doc);                     // a click, not a drag: done here
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

    sbl::LineStroke stroke;
    stroke.width         = width;
    stroke.minWidthRatio = minWidthRatio;
    stroke.colour        = colour;
    stroke.points.push_back(sbl::LinePoint{x, y, pressure});
    content.strokes.push_back(std::move(stroke));
    held_    = sbl::PointRef{content.strokes.size() - 1, 0};
    drawing_ = true;
    redraw(doc);
}

void LineworkTool::drag(sbl::Document& doc, double x, double y, float pressure) {
    sbl::Layer* layer = doc.layerById(layer_);
    if (layer == nullptr || !layer->linework.has_value() || !held_.has_value()) return;
    sbl::LineworkContent& content = *layer->linework;
    if (held_->stroke >= content.strokes.size()) return;
    std::vector<sbl::LinePoint>& points = content.strokes[held_->stroke].points;

    if (dragging_) {
        if (held_->point >= points.size()) return;
        points[held_->point].x = x;
        points[held_->point].y = y;
        redraw(doc);
        return;
    }
    if (!drawing_ || points.empty()) return;

    // Freehand: a new control point every kPointSpacing, and the line is only
    // redrawn when one lands. Redrawing per motion event would rasterise the
    // whole curve dozens of times for a line that has not changed shape.
    const sbl::LinePoint& last = points.back();
    if (std::hypot(x - last.x, y - last.y) < kPointSpacing) return;
    points.push_back(sbl::LinePoint{x, y, pressure});
    held_->point = points.size() - 1;
    redraw(doc);
}

void LineworkTool::release(sbl::Document& doc) {
    const bool wasBusy = dragging_ || drawing_;
    dragging_ = false;
    drawing_  = false;
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
