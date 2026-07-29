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

/// The same trick one width up (D-023). 64-bit throughout: 65535 * 65535 does
/// fit in 32 bits with the rounding term, but only just, and a proof nobody
/// re-checks is not worth the register on a 64-bit target.
constexpr std::uint16_t mul65535(std::uint64_t c, std::uint64_t a) noexcept {
    const std::uint64_t t = c * a + 32768u;
    return static_cast<std::uint16_t>((t + (t >> 16)) >> 16);
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

/// The only way to copy a mask (#48). Its tiles are move-only like every other
/// tile, so every copy is somebody typing this on purpose.
std::optional<LayerMask> cloneMask(const std::optional<LayerMask>& source) {
    if (!source.has_value()) return std::nullopt;
    LayerMask copy;
    copy.outside = source->outside;
    copy.enabled = source->enabled;
    copy.tiles.reserve(source->tiles.size());
    for (const auto& [key, tile] : source->tiles) copy.tiles.emplace(key, tile.clone());
    return copy;
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

StraightRgba16 PremulRgba16::unpremultiply() const noexcept {
    if (a == 0)     return StraightRgba16{};
    if (a == 65535) return StraightRgba16{r, g, b, 65535};
    const auto up = [this](std::uint16_t c) noexcept -> std::uint16_t {
        const std::uint64_t v = (static_cast<std::uint64_t>(c) * 65535u + a / 2u) / a;
        return static_cast<std::uint16_t>(std::min<std::uint64_t>(v, 65535u));
    };
    return StraightRgba16{up(r), up(g), up(b), a};
}

PremulRgba16 StraightRgba16::premultiply() const noexcept {
    if (a == 65535) return PremulRgba16{r, g, b, 65535};
    return PremulRgba16{mul65535(r, a), mul65535(g, a), mul65535(b, a), a};
}

PremulRgba16 over(PremulRgba16 src, PremulRgba16 dst) noexcept {
    if (src.a == 65535) return src;
    if (src.a == 0)     return dst;
    const std::uint32_t inv = 65535u - src.a;
    // This is the whole of the banding fix: a low-opacity dab laid over the
    // same pixel forty times rounds away 1/65535 of a step each time instead of
    // 1/255, so the passes accumulate instead of quantising into a plateau.
    return PremulRgba16{
        static_cast<std::uint16_t>(src.r + mul65535(dst.r, inv)),
        static_cast<std::uint16_t>(src.g + mul65535(dst.g, inv)),
        static_cast<std::uint16_t>(src.b + mul65535(dst.b, inv)),
        static_cast<std::uint16_t>(src.a + mul65535(dst.a, inv)),
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

PremulRgba16 blendOver(BlendMode mode, PremulRgba16 src, PremulRgba16 dst) noexcept {
    // Written out rather than shared with the 8-bit version through a template
    // over the channel type — D-023 asks for the second implementation first
    // and the generalisation only once there are two to generalise from. The
    // part that actually carries the maths, `blendChannel`, IS shared: it is
    // defined on 0..1 and has no channel width of its own.
    if (mode == BlendMode::Normal) return over(src, dst);
    if (src.a == 0) return dst;
    if (dst.a == 0) return src;

    const StraightRgba16 s = src.unpremultiply();
    const StraightRgba16 b = dst.unpremultiply();
    const float as = static_cast<float>(src.a) / 65535.0f;
    const float ab = static_cast<float>(dst.a) / 65535.0f;

    const auto channel = [&](std::uint16_t sc, std::uint16_t bc) -> std::uint16_t {
        const float cs = static_cast<float>(sc) / 65535.0f;
        const float cb = static_cast<float>(bc) / 65535.0f;
        const float co = as * (1.0f - ab) * cs
                       + as * ab * blendChannel(mode, cs, cb)
                       + (1.0f - as) * ab * cb;
        return static_cast<std::uint16_t>(
            std::lround(std::clamp(co, 0.0f, 1.0f) * 65535.0f));
    };

    const float ao = as + ab * (1.0f - as);
    return PremulRgba16{
        channel(s.r, b.r), channel(s.g, b.g), channel(s.b, b.b),
        static_cast<std::uint16_t>(std::lround(std::clamp(ao, 0.0f, 1.0f) * 65535.0f))};
}

// --------------------------------------------------------------------- tile

Tile::Tile(ColourDepth depth) : depth_(depth), stamp_(++g_tileStamp) {
    if (depth == ColourDepth::Bits16) px16_ = std::make_unique<PremulRgba16[]>(TILE_PIXELS);
    else                              px8_  = std::make_unique<PremulRgba8[]>(TILE_PIXELS);
}

Tile Tile::clone() const {
    Tile copy(depth_);
    if (px16_) std::memcpy(copy.px16_.get(), px16_.get(), byteSize());
    else       std::memcpy(copy.px8_.get(),  px8_.get(),  byteSize());
    return copy;
}

void Tile::fill(PremulRgba16 c) noexcept {
    stamp_ = ++g_tileStamp;
    if (px16_) std::fill_n(px16_.get(), TILE_PIXELS, c);
    else       std::fill_n(px8_.get(),  TILE_PIXELS, narrow(c));
}

bool Tile::isFullyTransparent() const noexcept {
    if (px16_) {
        for (int i = 0; i < TILE_PIXELS; ++i)
            if (px16_[i].a != 0) return false;
        return true;
    }
    for (int i = 0; i < TILE_PIXELS; ++i)
        if (px8_[i].a != 0) return false;
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
    return tiles.try_emplace(k, depth).first->second;
}

Tile* LayerMask::find(TileKey k) noexcept {
    const auto it = tiles.find(k);
    return it == tiles.end() ? nullptr : &it->second;
}

const Tile* LayerMask::find(TileKey k) const noexcept {
    const auto it = tiles.find(k);
    return it == tiles.end() ? nullptr : &it->second;
}

Tile& LayerMask::tileFor(TileKey k) {
    const auto [it, made] = tiles.try_emplace(k, ColourDepth::Bits8);
    // Filled with the coverage the rest of the mask already has. A tile that
    // arrived at zero would turn a whole 256-pixel square of a reveal-all mask
    // black the moment the brush crossed into it.
    if (made) it->second.fill(PremulRgba8{outside, outside, outside, 255});
    return it->second;
}

std::uint8_t maskCoverage(const LayerMask& mask, std::int32_t px,
                          std::int32_t py) noexcept {
    const TileKey key{tileIndex(px), tileIndex(py)};
    const Tile* tile = mask.find(key);
    if (tile == nullptr) return mask.outside;

    const int tx = px - key.first  * TILE_SIZE;
    const int ty = py - key.second * TILE_SIZE;
    // Mask tiles are made 8-bit and only ever made here, but a file some other
    // writer produced could hand us a wide one, and reading a null pointer is
    // the one failure this could have that costs the artist the whole session.
    const PremulRgba8* px8 = tile->pixels8();
    return px8 != nullptr
        ? px8[static_cast<std::size_t>(ty) * TILE_SIZE + static_cast<std::size_t>(tx)].r
        : narrowChannel(tile->pixel(tx, ty).r);
}

// ---------------------------------------------------------------------- undo

std::size_t UndoRecord::memoryBytes() const noexcept {
    // Asks each tile how big it is rather than assuming (D-023). This number is
    // shown to the artist in the status bar and the Edit menu, so at 16 bits it
    // has to say the truth — twice the bytes, and therefore about half the
    // history at the same budget — instead of quietly under-reporting by half.
    std::size_t n = label.capacity();
    for (const auto& s : tiles)
        if (s.before.has_value()) n += s.before->byteSize();
    if (structure && structure->state) {
        for (const auto& [key, tile] : structure->state->tiles) n += tile.byteSize();
        // A deleted layer takes its mask into the record with it, and a mask
        // tile is a quarter of a megabyte like any other (#48).
        if (structure->state->mask.has_value())
            for (const auto& [key, tile] : structure->state->mask->tiles)
                n += tile.byteSize();
    }
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

    // Read BEFORE the tiles move. One record can carry both halves — deleting a
    // mask does — and the tile half below may recreate the very mask the
    // property half is about to describe. Reading the properties afterwards
    // captures the state this call is halfway through building, and the redo
    // then restores a mask the artist deleted (#48).
    std::optional<LayerProps> propsBefore;
    if (rec.structure.has_value() && rec.structure->kind == LayerChange::Properties)
        if (const Layer* layer = doc.layerById(rec.structure->layer); layer != nullptr)
            propsBefore = propsOf(*layer);

    for (auto& snap : rec.tiles) {
        Layer* layer = doc.layerById(snap.layer);
        if (layer == nullptr) continue;   // layer went away; nothing to restore onto

        // Undoing "delete mask" runs this loop before the structural half that
        // recreates the mask, so the tiles have to be able to bring it back
        // themselves — otherwise the pixels the record is holding would be
        // dropped on the floor and the redo would restore an empty mask.
        if (snap.mask && !layer->mask.has_value()) {
            if (!snap.before.has_value()) continue;
            layer->mask.emplace();
        }
        TileMap& tiles = snap.mask ? layer->mask->tiles : layer->tiles;

        std::optional<Tile> current;
        if (const auto it = tiles.find(snap.key); it != tiles.end()) {
            current.emplace(std::move(it->second));
            tiles.erase(it);
        }
        if (snap.before.has_value())
            tiles.emplace(snap.key, std::move(*snap.before));

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
                if (layer != nullptr && propsBefore.has_value()) {
                    applyProps(*layer, d.props.value_or(LayerProps{}));
                    d.props = std::move(*propsBefore);
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

void mergeTileRecord(UndoRecord& into, UndoRecord&& next) {
    for (TileSnapshot& snap : next.tiles) {
        // The mask flag is part of the identity: one session can touch a tile
        // and the mask tile beside it, and they are different pixels.
        const bool known = std::any_of(
            into.tiles.begin(), into.tiles.end(), [&](const TileSnapshot& s) {
                return s.key == snap.key && s.mask == snap.mask;
            });
        // Already recorded means `into` holds the state from BEFORE the session,
        // and that is the one undo has to put back.
        if (!known) into.tiles.push_back(std::move(snap));
    }
}

void UndoStack::push(UndoRecord&& rec) {
    if (rec.empty()) return;              // a stroke that painted nothing costs no step
    done_.push_back(std::move(rec));
    undone_.clear();                      // US-04.5
    enforceBudget();
}

void UndoStack::setMemoryBudget(std::size_t bytes) noexcept {
    // A budget below one tile would evict on every stroke. Floor it at
    // something that can hold a few — of the LARGER kind, so the floor is a
    // floor at both depths rather than only at eight bits.
    budget_ = std::max<std::size_t>(bytes, tileBytes(ColourDepth::Bits16) * 4);
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
    l.id    = nextLayerId++;
    l.name  = std::move(name);
    l.depth = depth;               // the document is the authority on depth
    layers.push_back(std::move(l));
    return layers.back();
}

Document makeDocument(std::int32_t w, std::int32_t h, StraightRgba8 background,
                      ColourDepth depth) {
    Document doc;
    doc.width      = std::max(w, 1);
    doc.height     = std::max(h, 1);
    doc.background = background;
    doc.depth      = depth;        // set before addLayer, which copies it
    doc.activeLayer = doc.addLayer("Layer 1").id;
    return doc;
}

Document cloneDocument(const Document& doc) {
    Document copy;
    copy.width       = doc.width;
    copy.height      = doc.height;
    copy.dpi         = doc.dpi;
    copy.background  = doc.background;
    // Without this the autosave worker writes a manifest saying 8-bit over
    // tiles that are 16 — and the recovery file opens as a different painting
    // from the one that was lost. Tile::clone() keeps each tile's own depth, so
    // this line is what keeps the document's answer agreeing with theirs.
    copy.depth       = doc.depth;
    copy.activeLayer = doc.activeLayer;
    copy.nextLayerId = doc.nextLayerId;
    copy.selection   = doc.selection;
    // Without this the autosave worker writes a recovery file with the stored
    // selections missing, which is exactly the work #52 exists to stop losing.
    copy.storedSelections = doc.storedSelections;
    copy.path        = doc.path;
    copy.dirty       = doc.dirty;
    copy.vanishingPoints = doc.vanishingPoints;
    copy.warnings        = doc.warnings;
    // Undo history is deliberately not copied: it is not saved (D-011), and
    // copying it would double the memory this hand-off costs.

    copy.layers.reserve(doc.layers.size());
    for (const Layer& layer : doc.layers) {
        Layer out;
        out.id    = layer.id;
        out.kind  = layer.kind;
        out.depth = layer.depth;
        applyProps(out, propsOf(layer));
        // After applyProps, which creates an empty mask from the flags: this
        // is what puts the pixels in it. Without it the autosave worker writes
        // a recovery file whose masks hide nothing.
        out.mask = cloneMask(layer.mask);
        out.tiles.reserve(layer.tiles.size());
        for (const auto& [key, tile] : layer.tiles) out.tiles.emplace(key, tile.clone());
        copy.layers.push_back(std::move(out));
    }
    return copy;
}

// ----------------------------------------------------------- layer operations

LayerProps propsOf(const Layer& layer) {
    std::optional<MaskProps> mask;
    if (layer.mask.has_value()) mask = MaskProps{layer.mask->outside, layer.mask->enabled};
    return LayerProps{layer.name, layer.opacity, layer.blend, layer.visible,
                      layer.locked, layer.preserveOpacity, layer.clipToBelow,
                      layer.parent, layer.text, layer.linework, mask};
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
    layer.linework        = props.linework;
    // The flags move; the tiles stay where they are. An absent `mask` is the
    // one case that destroys pixels — it is how "delete mask" is expressed —
    // so `deleteLayerMask` snapshots them into the record first, and every
    // other caller gets `propsOf`'s copy of the current flags and changes
    // nothing.
    if (props.mask.has_value()) {
        if (!layer.mask.has_value()) layer.mask.emplace();
        layer.mask->outside = props.mask->outside;
        layer.mask->enabled = props.mask->enabled;
    } else {
        layer.mask.reset();
    }
    // Kind follows the source, in one place, so the two can never disagree: a
    // layer with words or curves in it is a text or linework layer and refuses
    // paint, and undoing a "rasterise" gives back that protection along with
    // the source. Folders are left alone — a folder has no pixels of its own to
    // protect. Text wins if a caller somehow sets both; a layer is one or the
    // other, and this is the one place that has to decide.
    if (layer.text.has_value())              layer.kind = LayerKind::Text;
    else if (layer.linework.has_value())     layer.kind = LayerKind::Linework;
    else if (layer.kind == LayerKind::Text ||
             layer.kind == LayerKind::Linework) layer.kind = LayerKind::Raster;
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
    layer.id    = doc.nextLayerId++;
    layer.name  = std::move(name);
    layer.depth = doc.depth;

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
    copy.name  = source->name + " copy";
    copy.kind  = source->kind;
    copy.depth = source->depth;
    // Tiles are move-only, so the copy is explicit — which is the point.
    for (const auto& [key, tile] : source->tiles) copy.tiles.emplace(key, tile.clone());
    copy.mask = cloneMask(source->mask);   // a duplicate that ignored the mask
                                           // would arrive showing what the
                                           // original hides

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
    // The upper layer's mask is baked in here, because the layer it belonged to
    // is about to stop existing and a merge that ignored it would put back on
    // screen exactly what the artist masked away (#48). The LOWER layer keeps
    // its own mask, which therefore also applies to what has just been merged
    // into it — the same answer Photoshop gives, and the only one that leaves
    // the mask editable at all.
    const LayerMask* srcMask =
        doc.layers[upper].mask.has_value() && doc.layers[upper].mask->enabled
            ? &*doc.layers[upper].mask : nullptr;

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

        // One lookup per tile, not per pixel: the mask is keyed the same way the
        // pixels are, so the tile that covers these 65'536 pixels is the tile at
        // the same key — and when there is none, `outside` covers all of them.
        const Tile* maskTile = srcMask != nullptr ? srcMask->find(key) : nullptr;
        const auto coverageAt = [&](int x, int y) -> float {
            if (srcMask == nullptr) return 1.0f;
            const std::uint8_t cov = [&] {
                if (maskTile == nullptr) return srcMask->outside;
                const PremulRgba8* mp = maskTile->pixels8();
                return mp != nullptr
                    ? mp[static_cast<std::size_t>(y) * TILE_SIZE +
                        static_cast<std::size_t>(x)].r
                    : narrowChannel(maskTile->pixel(x, y).r);
            }();
            return static_cast<float>(cov) / 255.0f;
        };

        Tile& dstTile = dstLayer.tileFor(key);
        if (srcTile.depth() == ColourDepth::Bits8 &&
            dstTile.depth() == ColourDepth::Bits8) {
            // The 8-bit path, untouched. Kept verbatim rather than routed
            // through the 16-bit one so that an 8-bit document merges to the
            // same bytes it always did, at the same speed (D-023).
            const PremulRgba8* src = srcTile.pixels8();
            PremulRgba8* dst = dstTile.pixels8();
            for (int i = 0; i < TILE_PIXELS; ++i) {
                PremulRgba8 s = src[i];
                if (s.a == 0) continue;
                // Opacity and coverage fold into ONE factor before rounding,
                // which is what `compositeLevel` does — two roundings here
                // would make a merged masked layer a level off the composite
                // it is supposed to have replaced exactly.
                const float factor = opacity * coverageAt(i % TILE_SIZE, i / TILE_SIZE);
                if (factor <= 0.0f) continue;
                if (factor < 1.0f) {
                    const auto scale = [&](std::uint8_t c) {
                        return static_cast<std::uint8_t>(
                            std::lround(static_cast<float>(c) * factor));
                    };
                    s = PremulRgba8{scale(s.r), scale(s.g), scale(s.b), scale(s.a)};
                }
                dst[i] = blendOver(mode, s, dst[i]);
            }
        } else {
            for (int y = 0; y < TILE_SIZE; ++y) {
                for (int x = 0; x < TILE_SIZE; ++x) {
                    PremulRgba16 s = srcTile.pixel(x, y);
                    if (s.a == 0) continue;
                    const float factor = opacity * coverageAt(x, y);
                    if (factor <= 0.0f) continue;
                    if (factor < 1.0f) {
                        const auto scale = [&](std::uint16_t c) {
                            return static_cast<std::uint16_t>(
                                std::lround(static_cast<float>(c) * factor));
                        };
                        s = PremulRgba16{scale(s.r), scale(s.g), scale(s.b), scale(s.a)};
                    }
                    dstTile.setPixel(x, y, blendOver(mode, s, dstTile.pixel(x, y)));
                }
            }
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

UndoRecord deleteLayerMask(Document& doc, LayerId id) {
    UndoRecord rec;
    Layer* layer = doc.layerById(id);
    if (layer == nullptr || !layer->mask.has_value()) return rec;

    LayerProps before = propsOf(*layer);
    // Moved, not cloned: the mask is being destroyed either way, so the record
    // can simply take the tiles — a delete that copied them would double the
    // memory of the one action most likely to involve a lot of them.
    rec.tiles.reserve(layer->mask->tiles.size());
    for (auto& [key, tile] : layer->mask->tiles)
        rec.tiles.push_back(TileSnapshot{id, key, std::move(tile), true});
    layer->mask.reset();

    rec.label = "Delete mask";
    // The property half is what says the mask existed at all; the tile half
    // above is what puts the pixels back into it. Undo runs the tiles first and
    // they recreate an empty mask to land in, so the two halves compose in
    // either direction (see `swapRecord`).
    rec.structure = LayerStructureDelta{LayerChange::Properties, id, indexOf(doc, id),
                                        std::nullopt, std::move(before)};
    return rec;
}

UndoRecord setLayerMask(Document& doc, LayerId id, LayerMask&& mask) {
    UndoRecord rec;
    Layer* layer = doc.layerById(id);
    if (layer == nullptr) return rec;

    LayerProps before = propsOf(*layer);

    // Moved, not cloned, exactly as `deleteLayerMask` does: these tiles are
    // being replaced either way, so the record may simply take them.
    if (layer->mask.has_value()) {
        rec.tiles.reserve(layer->mask->tiles.size() + mask.tiles.size());
        for (auto& [key, tile] : layer->mask->tiles)
            rec.tiles.push_back(TileSnapshot{id, key, std::move(tile), true});
    }
    // A null snapshot for every tile only the NEW mask has. Without these, undo
    // would put the old flags back over the new coverage — a mask that hides
    // what the artist never asked to hide, and no way to see why.
    for (const auto& entry : mask.tiles)
        if (!layer->mask.has_value() || !layer->mask->tiles.contains(entry.first))
            rec.tiles.push_back(TileSnapshot{id, entry.first, std::nullopt, true});

    layer->mask = std::move(mask);

    rec.label = "Set mask";
    rec.structure = LayerStructureDelta{LayerChange::Properties, id, indexOf(doc, id),
                                        std::nullopt, std::move(before)};
    return rec;
}

}  // namespace sbl
