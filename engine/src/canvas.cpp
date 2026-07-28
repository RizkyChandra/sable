#include "sbl/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace sbl {
namespace {

/// Divide by 255 with round-to-nearest. The naive c * a / 255 loses a step at
/// every level and shows up as a one-off in every exported colour.
constexpr std::uint8_t mul255(std::uint32_t c, std::uint32_t a) noexcept {
    const std::uint32_t t = c * a + 128;
    return static_cast<std::uint8_t>((t + (t >> 8)) >> 8);
}

}  // namespace

// ------------------------------------------------------------------- colour

StraightRgba8 PremulRgba8::unpremultiply() const noexcept {
    if (a == 0) return StraightRgba8{};   // fully transparent carries no colour
    if (a == 255) return StraightRgba8{r, g, b, 255};
    const auto up = [this](std::uint8_t c) noexcept -> std::uint8_t {
        const std::uint32_t v = (static_cast<std::uint32_t>(c) * 255u + a / 2u) / a;
        return static_cast<std::uint8_t>(std::min<std::uint32_t>(v, 255u));
    };
    return StraightRgba8{up(r), up(g), up(b), a};
}

PremulRgba8 StraightRgba8::premultiply() const noexcept {
    if (a == 255) return PremulRgba8{r, g, b, 255};
    return PremulRgba8{mul255(r, a), mul255(g, a), mul255(b, a), a};
}

PremulRgba8 over(PremulRgba8 src, PremulRgba8 dst) noexcept {
    if (src.a == 255) return src;
    if (src.a == 0)   return dst;
    const std::uint32_t inv = 255u - src.a;
    return PremulRgba8{
        static_cast<std::uint8_t>(src.r + mul255(dst.r, inv)),
        static_cast<std::uint8_t>(src.g + mul255(dst.g, inv)),
        static_cast<std::uint8_t>(src.b + mul255(dst.b, inv)),
        static_cast<std::uint8_t>(src.a + mul255(dst.a, inv)),
    };
}

namespace {

/// The blend functions, on straight-alpha channels in 0..1.
float blendChannel(BlendMode mode, float cs, float cb) noexcept {
    switch (mode) {
        case BlendMode::Normal:   return cs;
        case BlendMode::Multiply: return cs * cb;
        case BlendMode::Screen:   return cs + cb - cs * cb;
        case BlendMode::Add:      return std::min(1.0f, cs + cb);
        case BlendMode::Overlay:
            // Hard-light with the operands swapped, which is what Overlay is.
            return cb <= 0.5f ? 2.0f * cs * cb
                              : 1.0f - 2.0f * (1.0f - cs) * (1.0f - cb);
    }
    return cs;
}

}  // namespace

std::string_view blendModeName(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Normal:   return "normal";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Screen:   return "screen";
        case BlendMode::Add:      return "add";
        case BlendMode::Overlay:  return "overlay";
    }
    return "normal";
}

BlendMode blendModeFromName(std::string_view name) noexcept {
    if (name == "multiply") return BlendMode::Multiply;
    if (name == "screen")   return BlendMode::Screen;
    if (name == "add")      return BlendMode::Add;
    if (name == "overlay")  return BlendMode::Overlay;
    return BlendMode::Normal;      // unknown modes degrade, they do not fail
}

PremulRgba8 blendOver(BlendMode mode, PremulRgba8 src, PremulRgba8 dst) noexcept {
    // Normal is the overwhelming majority of pixels and needs none of the
    // divides below.
    if (mode == BlendMode::Normal) return over(src, dst);
    if (src.a == 0) return dst;
    if (dst.a == 0) return src;    // nothing underneath to blend with

    const StraightRgba8 s = src.unpremultiply();
    const StraightRgba8 b = dst.unpremultiply();
    const float as = static_cast<float>(src.a) / 255.0f;
    const float ab = static_cast<float>(dst.a) / 255.0f;

    // W3C compositing: co = as*(1-ab)*Cs + as*ab*B(Cb,Cs) + (1-as)*ab*Cb,
    // which comes out premultiplied, and ao = as + ab*(1-as).
    const auto channel = [&](std::uint8_t sc, std::uint8_t bc) -> std::uint8_t {
        const float cs = static_cast<float>(sc) / 255.0f;
        const float cb = static_cast<float>(bc) / 255.0f;
        const float co = as * (1.0f - ab) * cs
                       + as * ab * blendChannel(mode, cs, cb)
                       + (1.0f - as) * ab * cb;
        return static_cast<std::uint8_t>(std::lround(std::clamp(co, 0.0f, 1.0f) * 255.0f));
    };

    const float ao = as + ab * (1.0f - as);
    return PremulRgba8{
        channel(s.r, b.r), channel(s.g, b.g), channel(s.b, b.b),
        static_cast<std::uint8_t>(std::lround(std::clamp(ao, 0.0f, 1.0f) * 255.0f))};
}

// --------------------------------------------------------------------- tile

Tile Tile::clone() const {
    Tile copy;
    std::memcpy(copy.px_.get(), px_.get(), TILE_BYTES);
    return copy;
}

void Tile::fill(PremulRgba8 c) noexcept {
    std::fill_n(px_.get(), TILE_PIXELS, c);
}

bool Tile::isFullyTransparent() const noexcept {
    for (int i = 0; i < TILE_PIXELS; ++i)
        if (px_[i].a != 0) return false;
    return true;
}

// -------------------------------------------------------------------- layer

Tile* Layer::find(TileKey k) noexcept {
    const auto it = tiles.find(k);
    return it == tiles.end() ? nullptr : &it->second;
}

const Tile* Layer::find(TileKey k) const noexcept {
    const auto it = tiles.find(k);
    return it == tiles.end() ? nullptr : &it->second;
}

Tile& Layer::tileFor(TileKey k) {
    return tiles.try_emplace(k).first->second;
}

// ---------------------------------------------------------------------- undo

std::size_t UndoRecord::memoryBytes() const noexcept {
    std::size_t n = label.capacity();
    for (const auto& s : tiles)
        if (s.before.has_value()) n += TILE_BYTES;
    if (structure && structure->state)
        n += structure->state->tiles.size() * TILE_BYTES;
    return n;
}

namespace {

/// Swaps the record's stored state with the document's current state, and
/// leaves the record holding what was replaced. Applying it twice is the
/// identity — which is exactly why undo and redo are the same call.
std::vector<std::pair<LayerId, TileKey>> swapRecord(Document& doc, UndoRecord& rec) {
    std::vector<std::pair<LayerId, TileKey>> changed;
    changed.reserve(rec.tiles.size());

    for (auto& snap : rec.tiles) {
        Layer* layer = doc.layerById(snap.layer);
        if (layer == nullptr) continue;   // layer went away; nothing to restore onto

        std::optional<Tile> current;
        if (const auto it = layer->tiles.find(snap.key); it != layer->tiles.end()) {
            current.emplace(std::move(it->second));
            layer->tiles.erase(it);
        }
        if (snap.before.has_value())
            layer->tiles.emplace(snap.key, std::move(*snap.before));

        snap.before = std::move(current);
        changed.emplace_back(snap.layer, snap.key);
    }

    if (rec.structure.has_value()) {
        auto& d = *rec.structure;

        switch (d.kind) {
            case LayerChange::Properties: {
                // Swap the scalars and leave the pixels alone. Moving the whole
                // Layer here would drag every tile with it, which is why an
                // opacity slider does not cost megabytes of history.
                Layer* layer = doc.layerById(d.layer);
                if (layer != nullptr) {
                    LayerProps current = propsOf(*layer);
                    applyProps(*layer, d.props.value_or(LayerProps{}));
                    d.props = std::move(current);
                }
                break;
            }

            case LayerChange::Reorder: {
                // The layer exists on both sides; only its position moves.
                const auto it = std::ranges::find(doc.layers, d.layer, &Layer::id);
                if (it == doc.layers.end()) break;
                const auto currentIndex = static_cast<std::size_t>(it - doc.layers.begin());
                Layer moved = std::move(*it);
                doc.layers.erase(it);
                const auto at = std::min(d.index, doc.layers.size());
                doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(at),
                                  std::move(moved));
                d.index = currentIndex;
                break;
            }

            case LayerChange::Existence: {
                std::optional<Layer> current;
                std::size_t currentIndex = d.index;

                const auto it = std::ranges::find(doc.layers, d.layer, &Layer::id);
                if (it != doc.layers.end()) {
                    currentIndex = static_cast<std::size_t>(it - doc.layers.begin());
                    current.emplace(std::move(*it));
                    doc.layers.erase(it);
                }
                if (d.state.has_value()) {
                    const auto at = std::min(d.index, doc.layers.size());
                    doc.layers.insert(doc.layers.begin() +
                                      static_cast<std::ptrdiff_t>(at),
                                      std::move(*d.state));
                }
                d.state = std::move(current);
                d.index = currentIndex;
                break;
            }
        }

        if (doc.layerById(doc.activeLayer) == nullptr)
            doc.activeLayer = doc.layers.empty() ? NO_LAYER : doc.layers.back().id;
    }

    return changed;
}

const std::string kNoLabel{};

}  // namespace

void UndoStack::push(UndoRecord&& rec) {
    if (rec.empty()) return;              // a stroke that painted nothing costs no step
    done_.push_back(std::move(rec));
    undone_.clear();                      // US-04.5
    enforceBudget();
}

void UndoStack::setMemoryBudget(std::size_t bytes) noexcept {
    // A budget below one tile would evict on every stroke. Floor it at
    // something that can hold a few.
    budget_ = std::max<std::size_t>(bytes, static_cast<std::size_t>(TILE_BYTES) * 4);
    enforceBudget();
}

void UndoStack::enforceBudget() noexcept {
    // Computed once and decremented, rather than re-summing the whole stack on
    // every iteration — a full-canvas fill can make records big enough that
    // the quadratic version is felt.
    std::size_t total = memoryBytes();
    if (total <= budget_) return;

    // Undo history goes first: the artist has already moved past it. Redo is
    // dropped only if that was not enough, because losing a redo the artist is
    // in the middle of stepping through is the more surprising failure.
    while (total > budget_ && done_.size() > 1) {
        total -= done_.front().memoryBytes();
        done_.erase(done_.begin());
        ++dropped_;
    }
    while (total > budget_ && !undone_.empty()) {
        total -= undone_.front().memoryBytes();
        undone_.erase(undone_.begin());
        ++dropped_;
    }
}

std::vector<std::pair<LayerId, TileKey>> UndoStack::undo(Document& doc) {
    if (done_.empty()) return {};         // US-04.4: a no-op, not a crash
    UndoRecord rec = std::move(done_.back());
    done_.pop_back();
    auto changed = swapRecord(doc, rec);
    undone_.push_back(std::move(rec));
    return changed;
}

std::vector<std::pair<LayerId, TileKey>> UndoStack::redo(Document& doc) {
    if (undone_.empty()) return {};
    UndoRecord rec = std::move(undone_.back());
    undone_.pop_back();
    auto changed = swapRecord(doc, rec);
    done_.push_back(std::move(rec));
    return changed;
}

void UndoStack::clear() noexcept {
    done_.clear();
    undone_.clear();
    dropped_ = 0;
}

std::size_t UndoStack::memoryBytes() const noexcept {
    std::size_t n = 0;
    for (const auto& r : done_)   n += r.memoryBytes();
    for (const auto& r : undone_) n += r.memoryBytes();
    return n;
}

const std::string& UndoStack::undoLabel() const noexcept {
    return done_.empty() ? kNoLabel : done_.back().label;
}

const std::string& UndoStack::redoLabel() const noexcept {
    return undone_.empty() ? kNoLabel : undone_.back().label;
}

// ------------------------------------------------------------------ document

Layer* Document::layerById(LayerId id) noexcept {
    if (id == NO_LAYER) return nullptr;
    const auto it = std::ranges::find(layers, id, &Layer::id);
    return it == layers.end() ? nullptr : &*it;
}

const Layer* Document::layerById(LayerId id) const noexcept {
    if (id == NO_LAYER) return nullptr;
    const auto it = std::ranges::find(layers, id, &Layer::id);
    return it == layers.end() ? nullptr : &*it;
}

Layer& Document::addLayer(std::string name) {
    Layer l;
    l.id   = nextLayerId++;
    l.name = std::move(name);
    layers.push_back(std::move(l));
    return layers.back();
}

Document makeDocument(std::int32_t w, std::int32_t h, StraightRgba8 background) {
    Document doc;
    doc.width      = std::max(w, 1);
    doc.height     = std::max(h, 1);
    doc.background = background;
    doc.activeLayer = doc.addLayer("Layer 1").id;
    return doc;
}

Document cloneDocument(const Document& doc) {
    Document copy;
    copy.width       = doc.width;
    copy.height      = doc.height;
    copy.dpi         = doc.dpi;
    copy.background  = doc.background;
    copy.activeLayer = doc.activeLayer;
    copy.nextLayerId = doc.nextLayerId;
    copy.selection   = doc.selection;
    copy.path        = doc.path;
    copy.dirty       = doc.dirty;
    // Undo history is deliberately not copied: it is not saved (D-011), and
    // copying it would double the memory this hand-off costs.

    copy.layers.reserve(doc.layers.size());
    for (const Layer& layer : doc.layers) {
        Layer out;
        out.id   = layer.id;
        out.kind = layer.kind;
        applyProps(out, propsOf(layer));
        out.tiles.reserve(layer.tiles.size());
        for (const auto& [key, tile] : layer.tiles) out.tiles.emplace(key, tile.clone());
        copy.layers.push_back(std::move(out));
    }
    return copy;
}

// ----------------------------------------------------------- layer operations

LayerProps propsOf(const Layer& layer) {
    return LayerProps{layer.name, layer.opacity, layer.blend, layer.visible,
                      layer.locked, layer.preserveOpacity, layer.clipToBelow,
                      layer.parent};
}

void applyProps(Layer& layer, const LayerProps& props) {
    layer.name            = props.name;
    layer.opacity         = props.opacity;
    layer.blend           = props.blend;
    layer.visible         = props.visible;
    layer.locked          = props.locked;
    layer.preserveOpacity = props.preserveOpacity;
    layer.clipToBelow     = props.clipToBelow;
    layer.parent          = props.parent;
}

namespace {

std::size_t indexOf(const Document& doc, LayerId id) {
    const auto it = std::ranges::find(doc.layers, id, &Layer::id);
    return it == doc.layers.end() ? doc.layers.size()
                                  : static_cast<std::size_t>(it - doc.layers.begin());
}

}  // namespace

UndoRecord addLayerAbove(Document& doc, LayerId reference, std::string name) {
    Layer layer;
    layer.id   = doc.nextLayerId++;
    layer.name = std::move(name);

    const std::size_t at = reference == NO_LAYER
        ? doc.layers.size()
        : std::min(indexOf(doc, reference) + 1, doc.layers.size());

    const LayerId newId = layer.id;
    doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(at),
                      std::move(layer));
    doc.activeLayer = newId;

    UndoRecord rec;
    rec.label = "New layer";
    // `state` empty: before this action the layer did not exist, so undoing
    // removes it.
    rec.structure = LayerStructureDelta{LayerChange::Existence, newId, at,
                                        std::nullopt, std::nullopt};
    return rec;
}

UndoRecord deleteLayer(Document& doc, LayerId id) {
    UndoRecord rec;
    const auto it = std::ranges::find(doc.layers, id, &Layer::id);
    // The last raster layer stays: a document with nothing to paint on is a
    // dead end the artist has no obvious way out of.
    if (it == doc.layers.end() || doc.layers.size() <= 1) return rec;

    const auto at = static_cast<std::size_t>(it - doc.layers.begin());
    rec.label = "Delete layer";
    rec.structure = LayerStructureDelta{LayerChange::Existence, id, at,
                                        std::move(*it), std::nullopt};
    doc.layers.erase(it);

    if (doc.layerById(doc.activeLayer) == nullptr)
        doc.activeLayer = doc.layers[std::min(at, doc.layers.size() - 1)].id;
    return rec;
}

UndoRecord duplicateLayer(Document& doc, LayerId id) {
    UndoRecord rec;
    const Layer* source = doc.layerById(id);
    if (source == nullptr) return rec;

    Layer copy;
    copy.id   = doc.nextLayerId++;
    copy.name = source->name + " copy";
    applyProps(copy, propsOf(*source));
    copy.name = source->name + " copy";
    copy.kind = source->kind;
    // Tiles are move-only, so the copy is explicit — which is the point.
    for (const auto& [key, tile] : source->tiles) copy.tiles.emplace(key, tile.clone());

    const std::size_t at = std::min(indexOf(doc, id) + 1, doc.layers.size());
    const LayerId newId = copy.id;
    doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(at),
                      std::move(copy));
    doc.activeLayer = newId;

    rec.label = "Duplicate layer";
    rec.structure = LayerStructureDelta{LayerChange::Existence, newId, at,
                                        std::nullopt, std::nullopt};
    return rec;
}

UndoRecord moveLayer(Document& doc, LayerId id, int delta) {
    UndoRecord rec;
    const std::size_t from = indexOf(doc, id);
    if (from >= doc.layers.size() || delta == 0) return rec;

    const auto target = static_cast<std::ptrdiff_t>(from) + delta;
    // A no-op at either end, rather than an error the caller has to guard.
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(doc.layers.size())) return rec;
    const auto to = static_cast<std::size_t>(target);

    Layer moved = std::move(doc.layers[from]);
    doc.layers.erase(doc.layers.begin() + static_cast<std::ptrdiff_t>(from));
    doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(to),
                      std::move(moved));

    rec.label = delta > 0 ? "Raise layer" : "Lower layer";
    // Reorder carries no Layer: it still exists, so the record only needs to
    // remember where it came from.
    rec.structure = LayerStructureDelta{LayerChange::Reorder, id, from,
                                        std::nullopt, std::nullopt};
    return rec;
}

UndoRecord mergeLayerDown(Document& doc, LayerId id) {
    UndoRecord rec;
    const std::size_t upper = indexOf(doc, id);
    if (upper == 0 || upper >= doc.layers.size()) return rec;   // nothing below

    const std::size_t lower = upper - 1;
    if (doc.layers[lower].kind != LayerKind::Raster ||
        doc.layers[upper].kind != LayerKind::Raster) return rec;

    rec.label = "Merge down";
    const LayerId lowerId = doc.layers[lower].id;
    const BlendMode mode  = doc.layers[upper].blend;
    const float opacity   = std::clamp(doc.layers[upper].opacity, 0.0f, 1.0f);

    // Snapshot every tile of the lower layer that the merge will touch, then
    // composite. One record carries both halves.
    for (const auto& [key, srcTile] : doc.layers[upper].tiles) {
        Layer& dstLayer = doc.layers[lower];
        TileSnapshot snap;
        snap.layer = lowerId;
        snap.key   = key;
        if (const Tile* existing = dstLayer.find(key); existing != nullptr)
            snap.before.emplace(existing->clone());
        rec.tiles.push_back(std::move(snap));

        Tile& dstTile = dstLayer.tileFor(key);
        const PremulRgba8* src = srcTile.pixels();
        PremulRgba8* dst = dstTile.pixels();
        for (int i = 0; i < TILE_PIXELS; ++i) {
            PremulRgba8 s = src[i];
            if (s.a == 0) continue;
            if (opacity < 1.0f) {
                const auto scale = [&](std::uint8_t c) {
                    return static_cast<std::uint8_t>(
                        std::lround(static_cast<float>(c) * opacity));
                };
                s = PremulRgba8{scale(s.r), scale(s.g), scale(s.b), scale(s.a)};
            }
            dst[i] = blendOver(mode, s, dst[i]);
        }
    }

    const auto it = doc.layers.begin() + static_cast<std::ptrdiff_t>(upper);
    rec.structure = LayerStructureDelta{LayerChange::Existence, id, upper,
                                        std::move(*it), std::nullopt};
    doc.layers.erase(it);
    doc.activeLayer = lowerId;
    return rec;
}

UndoRecord setLayerProps(Document& doc, LayerId id, const LayerProps& props) {
    UndoRecord rec;
    Layer* layer = doc.layerById(id);
    if (layer == nullptr) return rec;

    LayerProps before = propsOf(*layer);
    applyProps(*layer, props);

    rec.label = "Layer settings";
    rec.structure = LayerStructureDelta{LayerChange::Properties, id, indexOf(doc, id),
                                        std::nullopt, std::move(before)};
    return rec;
}

}  // namespace sbl
