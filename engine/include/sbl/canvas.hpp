// Colour, tiles, layers, the document, and the undo stack.
// See docs/DATA-MODEL.md — this file is that document in compilable form.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// For VanishingPoint. The input boundary depends on nothing, so this is a
// one-way edge, not a tangle.
#include "sbl/input.hpp"

namespace sbl {

// ------------------------------------------------------------------- colour
// D-004: premultiplied and straight alpha are DIFFERENT TYPES. No implicit
// conversion, no constructor taking the other — the compiler has to reject a
// mix-up or the split earns nothing.

struct StraightRgba8;

/// Channels already multiplied by alpha. Everything inside the engine.
/// 50% red is {128, 0, 0, 128} — not {255, 0, 0, 128}.
struct PremulRgba8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
    [[nodiscard]] StraightRgba8 unpremultiply() const noexcept;
    friend bool operator==(const PremulRgba8&, const PremulRgba8&) = default;
};

/// What the user picks, and what PNG files contain. Boundaries only.
struct StraightRgba8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
    [[nodiscard]] PremulRgba8 premultiply() const noexcept;
    friend bool operator==(const StraightRgba8&, const StraightRgba8&) = default;
};

/// dst = src + dst * (1 - src.a), per channel, no per-pixel divide.
[[nodiscard]] PremulRgba8 over(PremulRgba8 src, PremulRgba8 dst) noexcept;

// --------------------------------------------------------------------- tile
// D-005: the unit of allocation, of redraw, of undo, and of texture upload.

inline constexpr int TILE_SIZE   = 256;
inline constexpr int TILE_PIXELS = TILE_SIZE * TILE_SIZE;
inline constexpr int TILE_BYTES  = TILE_PIXELS * 4;   // 262'144

/// Ticks once for every handle to a tile's pixels that could write to them.
///
/// Process-wide rather than a counter inside each Tile, and never reset: undo
/// destroys a tile and builds another, often at the same address, so anything
/// caching a tile's contents elsewhere (D-025's GPU arena) needs a value that
/// two different sets of pixels can never share. Not atomic — tiles belong to
/// the thread that owns the document, which is the same discipline
/// `cloneDocument` already relies on for the autosave hand-off.
inline std::uint64_t g_tileStamp = 0;

/// Premultiplied RGBA8, row-major, no padding.
/// Heap-allocated deliberately: 256 KiB must never land on the stack, and
/// moving a Tile must stay a pointer move rather than a memcpy.
class Tile {
public:
    Tile() : px_(std::make_unique<PremulRgba8[]>(TILE_PIXELS)), stamp_(++g_tileStamp) {}
    Tile(Tile&&) noexcept            = default;   // move-only, by design
    Tile& operator=(Tile&&) noexcept = default;
    Tile(const Tile&)                = delete;    // copy must be explicit
    Tile& operator=(const Tile&)     = delete;

    /// The only way to copy 256 KiB. Make it a thing you have to type.
    [[nodiscard]] Tile clone() const;

    /// Non-const, so handing it out is indistinguishable from writing through
    /// it. That is the whole of the invalidation story for a backend that
    /// keeps its own copy: it cannot be told about a write it did not make, so
    /// instead nobody can obtain the means to write without moving the stamp.
    [[nodiscard]] PremulRgba8* pixels() noexcept {
        stamp_ = ++g_tileStamp;
        return px_.get();
    }
    [[nodiscard]] const PremulRgba8* pixels() const noexcept { return px_.get(); }

    [[nodiscard]] PremulRgba8 pixel(int x, int y) const noexcept {
        return px_[static_cast<std::size_t>(y) * TILE_SIZE + x];
    }
    void setPixel(int x, int y, PremulRgba8 c) noexcept {
        stamp_ = ++g_tileStamp;
        px_[static_cast<std::size_t>(y) * TILE_SIZE + x] = c;
    }

    /// What the host pixels are, as a value. Equal stamps mean equal contents;
    /// unequal stamps mean no more than "assume they differ". Measured cost of
    /// keeping it: none — a 4-megapixel flood fill times the same either way.
    [[nodiscard]] std::uint64_t stamp() const noexcept { return stamp_; }

    void fill(PremulRgba8 c) noexcept;
    [[nodiscard]] bool isFullyTransparent() const noexcept;

private:
    std::unique_ptr<PremulRgba8[]> px_;
    std::uint64_t stamp_ = 0;
};

// -------------------------------------------------------------------- layer

/// Tile coordinates, not pixels. Negative once the canvas can be resized from
/// the top or left — never assume tx >= 0.
using TileKey = std::pair<std::int32_t, std::int32_t>;

/// std::unordered_map has no hash for std::pair. Supply one; do not reach for
/// boost::hash_combine or a new dependency for four lines.
struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const noexcept {
        return std::hash<std::uint64_t>{}(
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.first)) << 32) |
             static_cast<std::uint32_t>(k.second));
    }
};

using TileMap      = std::unordered_map<TileKey, Tile, TileKeyHash>;
using TouchedTiles = std::unordered_set<TileKey, TileKeyHash>;

/// Which tile a canvas pixel falls in. Floor division — C++ truncates toward
/// zero, which is wrong for negative coordinates.
[[nodiscard]] constexpr std::int32_t tileIndex(std::int32_t px) noexcept {
    return px >= 0 ? px / TILE_SIZE : -((-px + TILE_SIZE - 1) / TILE_SIZE);
}

/// Separable modes only — each channel is blended independently. The
/// non-separable HSL modes (Hue, Saturation, Colour, Luminosity) need the
/// whole pixel at once and do not fit `blendChannel`; they are deliberately
/// absent rather than approximated per channel, which would be wrong in a way
/// that looks nearly right.
///
/// Append only. The order is what the layer panel's dropdown indexes into,
/// and reordering would silently change every artist's layer.
enum class BlendMode : std::uint8_t {
    Normal, Multiply, Screen, Add, Overlay,
    Darken, Lighten, ColourDodge, ColourBurn, HardLight, SoftLight,
    Difference, Exclusion,
};
enum class LayerKind : std::uint8_t { Raster, Folder, Text, Linework };

// ---------------------------------------------------------------------- text
// Issue #20. A Text layer holds ordinary tiles like any other, rasterised from
// the TextContent below; the text is kept beside them so it can be edited
// again. The PIXELS are what renders, everywhere — which is why a font that has
// been uninstalled since the file was saved cannot change a finished drawing,
// and why the compositor needed no new case (D-002's #1 bug).
//
// This lives here rather than in sbl/text.hpp because it is document data: the
// rasteriser depends on the document, so the document must not depend on it.

enum class TextAlign : std::uint8_t { Left, Centre, Right };

struct TextContent {
    /// LF between lines. Nothing wraps it — a text box with automatic wrapping
    /// is a different feature, and one an artist can do without far longer than
    /// they can do without any text at all.
    std::string utf8;
    std::string fontPath;              // the file the glyphs came from
    std::string fontName;              // family name: readable in the manifest,
                                       // and what a fallback search matches on
    float     sizePx      = 48.0f;
    float     lineSpacing = 1.2f;      // multiple of the font's own line height
    TextAlign align       = TextAlign::Left;
    /// Canvas pixels. The anchor of the FIRST baseline: x moves with alignment,
    /// y is the baseline itself, not the top of the glyphs.
    double x = 0.0, y = 0.0;
    StraightRgba8 colour{0, 0, 0, 255};

    friend bool operator==(const TextContent&, const TextContent&) = default;
};

// ------------------------------------------------------------------ linework
// Issue #17, and the same bargain D-026 struck for text: a Linework layer holds
// ordinary tiles, rasterised from the curves below, and the curves are kept
// beside them so they can be re-shaped afterwards. The PIXELS are what renders,
// on screen and in every export alike — which is why `compositeRect` needed no
// new case and there is no second, screen-only path for #1 to come back through.
//
// Here rather than in sbl/linework.hpp for the same reason as TextContent: this
// is document data, and the rasteriser depends on the document, so the document
// must not depend on the rasteriser.

/// One control point. Pressure is the artist's, recorded at the moment the
/// point was laid down and editable afterwards like the position — that is the
/// whole difference between linework and a stroke of paint.
struct LinePoint {
    double x = 0.0, y = 0.0;      // canvas pixels
    float  pressure = 1.0f;       // 0..1, scales the width at this point

    friend bool operator==(const LinePoint&, const LinePoint&) = default;
};

/// One editable curve.
///
/// The curve passes THROUGH its control points (a centripetal Catmull-Rom
/// spline), so there are no off-curve handles to store, to draw, or to explain.
/// An artist drags the point they can see and the line goes there.
struct LineStroke {
    std::vector<LinePoint> points;
    StraightRgba8 colour{0, 0, 0, 255};
    float width         = 4.0f;    // diameter in canvas pixels at full pressure
    float minWidthRatio = 0.15f;   // width at zero pressure, as a fraction of it

    friend bool operator==(const LineStroke&, const LineStroke&) = default;
};

struct LineworkContent {
    std::vector<LineStroke> strokes;

    friend bool operator==(const LineworkContent&, const LineworkContent&) = default;
};

/// Every mode, in enum order. One list so a new mode cannot be added to the
/// enum and forgotten by the name mapping, the dropdown, or the tests.
inline constexpr std::array<BlendMode, 13> ALL_BLEND_MODES{
    BlendMode::Normal,      BlendMode::Multiply,  BlendMode::Screen,
    BlendMode::Add,         BlendMode::Overlay,   BlendMode::Darken,
    BlendMode::Lighten,     BlendMode::ColourDodge, BlendMode::ColourBurn,
    BlendMode::HardLight,   BlendMode::SoftLight, BlendMode::Difference,
    BlendMode::Exclusion,
};

[[nodiscard]] std::string_view blendModeName(BlendMode) noexcept;
[[nodiscard]] BlendMode blendModeFromName(std::string_view) noexcept;

/// Composites `src` onto `dst` through a blend mode.
///
/// Every mode but Normal is defined on straight-alpha colour, so this
/// unpremultiplies, blends, and re-premultiplies in ONE place. Done inline
/// at each call site it drifts, and inconsistent unpremultiply is the other
/// classic source of dark fringes.
[[nodiscard]] PremulRgba8 blendOver(BlendMode mode, PremulRgba8 src,
                                    PremulRgba8 dst) noexcept;

using LayerId = std::uint32_t;                 // stable; never reused in a document
inline constexpr LayerId NO_LAYER = 0;

struct Layer {
    LayerId     id   = NO_LAYER;
    LayerKind   kind = LayerKind::Raster;
    std::string name;

    /// Sparse (D-005). Raster layers only; a Folder keeps this empty.
    /// An absent entry means fully transparent — not a buffer of zeros.
    TileMap tiles;

    float     opacity = 1.0f;          // 0..1, applied at composite time
    BlendMode blend   = BlendMode::Normal;
    bool visible         = true;
    bool locked          = false;      // protect from painting
    bool preserveOpacity = false;      // paint only where alpha > 0
    bool clipToBelow     = false;      // clipping group
    std::optional<LayerId> parent;     // folder membership
    /// Set on a LayerKind::Text layer, and on nothing else. Its tiles are
    /// generated from this, so painting on such a layer is refused rather than
    /// silently thrown away by the next keystroke.
    std::optional<TextContent> text;
    /// Set on a LayerKind::Linework layer, and on nothing else. Same bargain as
    /// `text` above: the tiles are generated from these curves, so paint on such
    /// a layer is refused rather than lost the next time one is dragged.
    std::optional<LineworkContent> linework;
    // Draw order is the Document's vector order, not a field here.

    /// Null when the tile has never been painted. Do not cache the result
    /// across an undo or a layer operation — erase invalidates it.
    [[nodiscard]] Tile*       find(TileKey k) noexcept;
    [[nodiscard]] const Tile* find(TileKey k) const noexcept;
    /// Allocates on first touch.
    [[nodiscard]] Tile&       tileFor(TileKey k);
};

// ----------------------------------------------------------------- selection

/// A bounding rectangle, optionally carrying a per-pixel coverage mask for the
/// shapes a rectangle cannot say: lasso, magic wand, and whatever the
/// add/subtract/intersect modifiers built out of them (#18).
///
/// Ask it questions rather than reading its fields. An EMPTY `mask` means "all
/// of the rectangle, fully covered" — that is the fast path, and the reason a
/// plain marquee still costs four comparisons per pixel instead of a megabyte
/// of 255s and a cache miss.
///
/// See `sbl/select.hpp` for the things that build one.
struct Selection {
    std::int32_t x = 0, y = 0, w = 0, h = 0;

    /// Empty, or exactly w * h bytes, row-major from (x, y). Nothing else is a
    /// valid state; `coverage()` would read past a short one.
    ///
    /// The `{}` is load-bearing: it keeps `Selection{x, y, w, h}` a warning-free
    /// aggregate initialisation, which is how every existing call site and test
    /// builds one.
    std::vector<std::uint8_t> mask{};

    [[nodiscard]] bool empty() const noexcept { return w <= 0 || h <= 0; }

    /// 0 outside, 255 fully inside, and the values between are the
    /// anti-aliased edge. Writers scale what they lay down by this rather than
    /// treating the boundary as in-or-out, which is what stops a lasso fill
    /// coming out with a staircase along its edge.
    [[nodiscard]] std::uint8_t coverage(std::int32_t px,
                                        std::int32_t py) const noexcept {
        if (px < x || py < y || px >= x + w || py >= y + h) return 0;
        if (mask.empty()) return 255;
        return mask[static_cast<std::size_t>(py - y) * static_cast<std::size_t>(w) +
                    static_cast<std::size_t>(px - x)];
    }

    /// The yes-or-no question every caller already asks, and still the whole
    /// interface for anything that cannot express a fraction. Half coverage
    /// counts as inside, so a shape's edge pixels belong to it; for a
    /// rectangle the answer is exactly what it always was.
    [[nodiscard]] bool contains(std::int32_t px, std::int32_t py) const noexcept {
        return coverage(px, py) >= 128;
    }

    friend bool operator==(const Selection&, const Selection&) = default;
};

// ---------------------------------------------------------------------- undo
// D-006: copy-on-first-touch tile snapshots. One record per stroke.

struct TileSnapshot {
    LayerId layer = NO_LAYER;
    TileKey key{};
    /// Empty means the tile did not exist. Undoing the first stroke on a tile
    /// must REMOVE it, not fill it with zeros, or the sparse map stops being
    /// sparse.
    std::optional<Tile> before;
};

/// A layer's settings, with no pixels attached. Separated so that changing an
/// opacity slider does not have to snapshot several megabytes of tiles.
struct LayerProps {
    std::string name;
    float     opacity = 1.0f;
    BlendMode blend   = BlendMode::Normal;
    bool visible         = true;
    bool locked          = false;
    bool preserveOpacity = false;
    bool clipToBelow     = false;
    std::optional<LayerId> parent;
    /// Editing text is a property change, not a pixel change: carrying it here
    /// means one text edit is ONE undo step covering both the words and the
    /// glyphs they were drawn as, through the machinery that already exists.
    std::optional<TextContent> text;
    /// The curves, for the same reason: dragging a control point is one undo
    /// step covering both the geometry and the line it was drawn as.
    std::optional<LineworkContent> linework;
};

[[nodiscard]] LayerProps propsOf(const Layer&);
void applyProps(Layer&, const LayerProps&);

/// What kind of structural change a record carries. Reorder cannot be
/// expressed as "the previous state" the way the other two can — the layer
/// exists both before and after, so the record stores only where it belonged.
enum class LayerChange : std::uint8_t { Existence, Reorder, Properties };

/// Everything about a layer except its pixels: creation, deletion, reorder,
/// and property edits. Like TileSnapshot it stores only the *previous* state
/// and swaps on the way past, so undo and redo are one implementation.
struct LayerStructureDelta {
    LayerChange kind  = LayerChange::Existence;
    LayerId     layer = NO_LAYER;
    std::size_t index = 0;               // position in Document::layers

    /// Existence only. Empty means the layer did not exist before, so applying
    /// the record removes it — that is what makes "add a layer" undoable.
    std::optional<Layer> state;          // moved, never copied — Layer is move-only
    /// Properties only.
    std::optional<LayerProps> props;
};

struct UndoRecord {
    std::string               label;     // "Pencil", "Merge layers"
    std::vector<TileSnapshot> tiles;     // pixel changes
    std::optional<LayerStructureDelta> structure;

    [[nodiscard]] bool empty() const noexcept {
        return tiles.empty() && !structure.has_value();
    }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;
};

struct Document;

/// One stack for pixel and structural changes both, or Ctrl+Z will surprise
/// the artist.
class UndoStack {
public:
    /// Clears the redo stack (US-04.5). An empty record is dropped rather than
    /// pushed, so a stroke that painted nothing costs no step.
    void push(UndoRecord&& rec);

    [[nodiscard]] bool canUndo() const noexcept { return !done_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !undone_.empty(); }

    /// No-ops when the matching stack is empty — never an error, never a crash
    /// (US-04.4). Both swap current state into the opposite stack, so the two
    /// directions are one implementation. Returns the tiles whose pixels moved,
    /// so the caller can invalidate exactly those textures (US-04.8).
    std::vector<std::pair<LayerId, TileKey>> undo(Document& doc);
    std::vector<std::pair<LayerId, TileKey>> redo(Document& doc);

    void clear() noexcept;
    [[nodiscard]] std::size_t size()        const noexcept { return done_.size(); }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

    // --- D-102: a fixed budget, oldest dropped first, and a count the UI can
    // show. An unbounded history is the version that eventually swaps a
    // laptop to death mid-drawing, which is the opposite of "modest hardware
    // is the target".

    /// Default budget. A typical stroke touches a handful of tiles, so this
    /// holds hundreds of them — comfortably more than the 50 steps US-04.3
    /// requires — while capping the pathological case of repeated
    /// full-canvas operations on a large document.
    static constexpr std::size_t kDefaultBudgetBytes = 256u * 1024u * 1024u;

    void setMemoryBudget(std::size_t bytes) noexcept;
    [[nodiscard]] std::size_t memoryBudget() const noexcept { return budget_; }

    /// How many records have been dropped to stay inside the budget. Nonzero
    /// means undo will not reach all the way back, and the artist is told —
    /// silently shortening someone's history is the part that feels like a bug.
    [[nodiscard]] std::size_t droppedRecords() const noexcept { return dropped_; }

    [[nodiscard]] const std::string& undoLabel() const noexcept;
    [[nodiscard]] const std::string& redoLabel() const noexcept;

private:
    /// Drops the oldest history until the budget is met. One record always
    /// survives: a stack that evicts down to nothing turns Ctrl+Z into a
    /// no-op exactly when a big operation has made it most valuable.
    void enforceBudget() noexcept;

    std::vector<UndoRecord> done_;
    std::vector<UndoRecord> undone_;
    std::size_t budget_  = kDefaultBudgetBytes;
    std::size_t dropped_ = 0;
};

// ------------------------------------------------------------------ document

struct Document {
    std::int32_t  width  = 0;
    std::int32_t  height = 0;
    std::uint32_t dpi    = 72;
    StraightRgba8 background{255, 255, 255, 255};

    std::vector<Layer> layers;             // index 0 = bottom
    LayerId  activeLayer = NO_LAYER;
    LayerId  nextLayerId = 1;              // never reused within a document

    UndoStack undo;
    std::optional<Selection> selection;    // Milestone 3; empty = whole canvas
    std::filesystem::path path;            // empty until first save
    bool dirty = false;

    /// Perspective guides, in canvas pixels. Document state, unlike the ruler
    /// that uses them: a scene's horizon is part of the drawing and has to come
    /// back with it, while "is the ruler on" is a preference.
    std::vector<VanishingPoint> vanishingPoints;

    /// What an importer had to drop or could not read, worded for the artist
    /// (#40). Empty for anything Sable wrote itself.
    ///
    /// This is the whole of the engine's channel to the UI: the engine has no
    /// SDL and no ImGui (D-003), so it cannot show anything — it can only leave
    /// the sentence where the app will find it. Silently handing back a
    /// different picture from the one in the file is the failure this exists to
    /// prevent, and stderr is nowhere for an application started from a desktop
    /// icon.
    std::vector<std::string> warnings;

    [[nodiscard]] Layer*       layerById(LayerId id) noexcept;
    [[nodiscard]] const Layer* layerById(LayerId id) const noexcept;
    [[nodiscard]] Layer*       active() noexcept { return layerById(activeLayer); }

    Layer& addLayer(std::string name);
};

/// A blank document with one raster layer, ready to paint on.
[[nodiscard]] Document makeDocument(std::int32_t w, std::int32_t h,
                                    StraightRgba8 background);

/// A deep, independent copy — every tile cloned, no undo history.
///
/// This is the hand-off for the background save (D-013, and the threading note
/// in DATA-MODEL.md). The worker must own its data outright: C++ gives us
/// nothing that stops a worker reaching into the live document, so the
/// discipline has to be explicit, and "copy it all" is the version that cannot
/// be got subtly wrong under a deadline.
///
/// Copies host tiles, and only host tiles. Call `PaintBackend::readback` first
/// if a backend may be holding pixels somewhere else — the worker thread has no
/// device context and cannot fetch them itself.
[[nodiscard]] Document cloneDocument(const Document& doc);

// ----------------------------------------------------------- layer operations
//
// Each returns the UndoRecord for what it did, for the caller to push. They do
// not touch the undo stack themselves — the app decides whether an action is
// worth a history entry, and tests can call them without one.

[[nodiscard]] UndoRecord addLayerAbove(Document&, LayerId reference, std::string name);
[[nodiscard]] UndoRecord deleteLayer(Document&, LayerId);
[[nodiscard]] UndoRecord duplicateLayer(Document&, LayerId);
/// delta -1 moves down the stack, +1 up. A no-op at either end.
[[nodiscard]] UndoRecord moveLayer(Document&, LayerId, int delta);
/// Composites `id` into the layer beneath it and removes `id`. One record:
/// pixel snapshots for the lower layer, a structural delete for the upper.
[[nodiscard]] UndoRecord mergeLayerDown(Document&, LayerId);
[[nodiscard]] UndoRecord setLayerProps(Document&, LayerId, const LayerProps&);

}  // namespace sbl
