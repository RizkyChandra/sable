#include "sbl/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "sbl/backend.hpp"

namespace sbl {
namespace {

/// Divide by 255 with round-to-nearest. The naive c * a / 255 loses a step at
/// every level and shows up as a one-off in every exported colour.
constexpr std::uint8_t mul255(std::uint32_t c, std::uint32_t a) noexcept {
    const std::uint32_t t = c * a + 128;
    return static_cast<std::uint8_t>((t + (t >> 8)) >> 8);
}

/// Makes `Tile::pixels()` the truth before host code moves tiles about.
///
/// The layer operations below read and move pixels without going through the
/// backend, so each of them has to say so — that is the rule D-025 puts in
/// place of an audit. A no-op on the CPU. The `std::expected` is dropped
/// deliberately: none of these callers can do anything useful with a failure,
/// and `PaintBackend::takeError` carries it to the app for them.
void ensureHostTiles(const Document& doc) {
    if (const auto ready = paintBackend().readback(doc); !ready.has_value()) {
        // Recorded on the backend; see PaintBackend::readback.
    }
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

/// Hard light: multiply where the source is dark, screen where it is bright.
/// Overlay is the same function with the operands swapped, so it lives here
/// once rather than being written out twice and drifting.
constexpr float hardLight(float cs, float cb) noexcept {
    return cs <= 0.5f ? 2.0f * cs * cb
                      : 1.0f - 2.0f * (1.0f - cs) * (1.0f - cb);
}

/// The blend functions, on straight-alpha channels in 0..1.
/// Formulae are the W3C compositing separable set, which is what Photoshop,
/// Krita and the PSD/ORA files we will import all agree on.
float blendChannel(BlendMode mode, float cs, float cb) noexcept {
    switch (mode) {
        case BlendMode::Normal:   return cs;
        case BlendMode::Multiply: return cs * cb;
        case BlendMode::Screen:   return cs + cb - cs * cb;
        case BlendMode::Add:      return std::min(1.0f, cs + cb);
        case BlendMode::Overlay:  return hardLight(cb, cs);
        case BlendMode::Darken:   return std::min(cs, cb);
        case BlendMode::Lighten:  return std::max(cs, cb);

        case BlendMode::ColourDodge:
            // The two guards are not tidiness: a black backdrop must stay
            // black however bright the source, and cs == 1 would divide by
            // zero and hand an infinity to the compositor.
            if (cb <= 0.0f) return 0.0f;
            if (cs >= 1.0f) return 1.0f;
            return std::min(1.0f, cb / (1.0f - cs));

        case BlendMode::ColourBurn:
            // Mirror of dodge, and the same two degenerate cases.
            if (cb >= 1.0f) return 1.0f;
            if (cs <= 0.0f) return 0.0f;
            return 1.0f - std::min(1.0f, (1.0f - cb) / cs);

        case BlendMode::HardLight: return hardLight(cs, cb);

        case BlendMode::SoftLight: {
            // W3C's soft light. The quartic below the quarter point is what
            // keeps the curve continuous where sqrt() would kink it, and a
            // kink in a shading mode shows up as a visible band.
            if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
            const float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb
                                        : std::sqrt(cb);
            return cb + (2.0f * cs - 1.0f) * (d - cb);
        }

        case BlendMode::Difference: return std::abs(cs - cb);
        case BlendMode::Exclusion:  return cs + cb - 2.0f * cs * cb;
    }
    return cs;
}

}  // namespace

std::string_view blendModeName(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Normal:      return "normal";
        case BlendMode::Multiply:    return "multiply";
        case BlendMode::Screen:      return "screen";
        case BlendMode::Add:         return "add";
        case BlendMode::Overlay:     return "overlay";
        case BlendMode::Darken:      return "darken";
        case BlendMode::Lighten:     return "lighten";
        case BlendMode::ColourDodge: return "colour-dodge";
        case BlendMode::ColourBurn:  return "colour-burn";
        case BlendMode::HardLight:   return "hard-light";
        case BlendMode::SoftLight:   return "soft-light";
        case BlendMode::Difference:  return "difference";
        case BlendMode::Exclusion:   return "exclusion";
    }
    return "normal";
}

BlendMode blendModeFromName(std::string_view name) noexcept {
    // Reading the table above backwards, rather than keeping a second copy of
    // it that can disagree with the first.
    for (const BlendMode mode : ALL_BLEND_MODES)
        if (name == blendModeName(mode)) return mode;
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
    stamp_ = ++g_tileStamp;
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
    // The tiles this is about to move around must be the ones the artist last
    // saw. A backend holding a newer copy somewhere else cannot be asked
    // afterwards — by then the tiles have swapped and there is nothing left to
    // put the pixels back into.
    ensureHostTiles(doc);

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
    copy.vanishingPoints = doc.vanishingPoints;
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
                      layer.parent, layer.text};
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
    layer.text            = props.text;
    // Kind follows the text, in one place, so the two can never disagree: a
    // layer with words in it is a text layer and refuses paint, and undoing a
    // "rasterise" gives back that protection along with the words. Folders are
    // left alone — a folder has no pixels of its own to protect.
    if (layer.text.has_value())              layer.kind = LayerKind::Text;
    else if (layer.kind == LayerKind::Text)  layer.kind = LayerKind::Raster;
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

    // The whole layer, tiles and all, goes into the undo record; undoing puts
    // it back. Anything a backend was still holding has to be in it.
    ensureHostTiles(doc);

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

    ensureHostTiles(doc);      // the clone below copies host pixels only

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

UndoRecord CpuBackend::mergeLayerDown(Document& doc, LayerId id) {
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
