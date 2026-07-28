// Brushes, the input boundary, and the dab pipeline.
// SDL stops at InputSample — no SDL_ type appears past this point (D-003).
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sbl/canvas.hpp"
#include "sbl/input.hpp"

namespace sbl {

// ------------------------------------------------------------------- brushes

/// Round is the only shape in v1; the enum exists so the stamp path has
/// somewhere to land.
enum class ShapeId : std::uint8_t { Round, Stamp };

/// Index into the texture registry the app owns. Not a pointer — presets are
/// serialised, and the registry outlives none of them.
using TextureId = std::uint32_t;

/// Which properties pressure drives. Independently switchable: a marker keeps
/// a fixed size and varies only density; a lineart pen does the opposite.
struct PressureMapping {
    bool toSize     = true;
    bool toDensity  = false;
    bool toBlending = false;
    bool toTexture  = false;
};

struct BrushPreset {
    std::string id;                  // stable slug, used by preset files on disk
    std::string name;

    float size            = 10.0f;   // diameter in px at full pressure
    float minSizeRatio    = 0.0f;    // 0..1, size at zero pressure, as a fraction
    float density         = 1.0f;    // 0..1, paint strength of ONE dab
    float minDensityRatio = 0.0f;    // 0..1, density at zero pressure
    float spacingFactor   = 0.1f;    // fraction of diameter
    float hardness        = 1.0f;    // 0..1; 1 crisp pencil, 0 airbrush falloff

    ShapeId shape = ShapeId::Round;
    std::optional<TextureId> texture;
    float textureStrength = 0.0f;

    // Watercolour / smudge family only. Unused at Milestone 1.
    float blending    = 0.0f;
    float dilution    = 0.0f;
    float persistence = 0.0f;

    std::uint8_t    stabilizerLevel = 0;   // 0 = off .. 3 = high
    PressureMapping pressure;
    bool isEraser = false;                 // erases alpha instead of adding colour
};

[[nodiscard]] BrushPreset defaultPencil();
[[nodiscard]] BrushPreset defaultEraser();
[[nodiscard]] BrushPreset defaultOpaque();
[[nodiscard]] BrushPreset defaultAirbrush();
[[nodiscard]] BrushPreset defaultMarker();
[[nodiscard]] BrushPreset defaultWatercolour();
[[nodiscard]] BrushPreset defaultSmudge();

/// The built-in set, in the order PRD §11 gives for implementing them.
[[nodiscard]] std::vector<BrushPreset> defaultBrushes();

// ----------------------------------------------------------------------- dab

/// One stamp of the brush. A stroke is nothing but a stream of these.
struct Dab {
    double x = 0.0, y = 0.0;   // centre, sub-pixel
    float radius   = 0.0f;     // pixels, after pressure mapping
    float density  = 0.0f;     // 0..1, this dab's paint strength
    float hardness = 0.0f;     // 0..1, edge falloff
    float angle    = 0.0f;     // radians, from tilt/rotation; 0 when round
    PremulRgba8 colour{};      // density already folded in
    bool  erase = false;
};

/// Spacing rule — the single most important line in the engine.
[[nodiscard]] inline double spacingFor(float radius, float spacingFactor) noexcept {
    return std::max(1.0, static_cast<double>(radius) * 2.0 * spacingFactor);
}

// -------------------------------------------------------------------- stroke

/// Live state between pen-down and pen-up.
struct Stroke {
    /// Copied, not referenced: the artist may edit the preset mid-stroke.
    BrushPreset   preset;
    StraightRgba8 colour{};
    LayerId       target = NO_LAYER;

    std::vector<InputSample> samples;      // post-normalisation
    double       leftoverDistance = 0.0;   // dab spacing carry-over
    UndoRecord   pending;                  // copy-on-first-touch (D-006)
    TouchedTiles touched;

    // Watercolour and smudge only: colour picked up from the canvas.
    PremulRgba8 loadedColour{};
    float       loadedAmount = 0.0f;

    [[nodiscard]] bool active() const noexcept { return !samples.empty(); }
};

/// Where dabs land. The paint path takes the pieces it needs, never the whole
/// document — this keeps it free of lookups and unit-testable without one.
struct PaintTarget {
    Layer&        layer;
    UndoRecord&   undo;
    TouchedTiles& touched;
    std::int32_t  width  = 0;    // canvas bounds; dabs clip to this
    std::int32_t  height = 0;
    /// When set, paint lands only inside it. Empty optional means the whole
    /// canvas, which is what "no selection" has to mean.
    const Selection* selection = nullptr;
};

void beginStroke(Stroke& s, const BrushPreset& preset, StraightRgba8 colour,
                 LayerId target, std::size_t expectedSamples = 256);

/// Appends the dabs for the segment ending at `sample`. The first sample of a
/// stroke emits exactly one dab, so a click without movement is never a no-op
/// (US-02.3).
///
/// The leftover distance lives in the Stroke, not in a local: restarting the
/// walk at each sample is the bug that clumps paint at slow speeds and leaves
/// gaps at fast ones (US-03).
void appendSample(Stroke& s, const InputSample& sample, std::vector<Dab>& out);

/// Snapshots each tile on first touch (D-006), then blends. Allocates only
/// when a tile or a snapshot is created for the first time.
void applyDab(PaintTarget& t, const Dab& dab) noexcept;

/// Convenience for the whole segment: emit and apply. Returns the dab count.
std::size_t paintSample(Stroke& s, PaintTarget& t, const InputSample& sample,
                        std::vector<Dab>& scratch);

/// Removes every tile from the layer, recording the removal as one undoable
/// step (US-06). The background colour lives in the Document and is composited
/// underneath, so clearing pixels is what "fill with the background" means.
[[nodiscard]] UndoRecord clearLayer(Layer& layer);

/// Flood-fills the region of similar colour containing (x, y).
///
/// The region is found on the *composited* image, because that is what the
/// artist sees and clicks on, but the paint lands only on `target`. Filling
/// based on the target layer alone would make a fill inside line art on a
/// separate layer impossible, which is the single most common use of the tool.
///
/// `tolerance` is 0..255 per channel. Honours the document's selection.
[[nodiscard]] UndoRecord bucketFill(Document& doc, LayerId target,
                                    std::int32_t x, std::int32_t y,
                                    StraightRgba8 colour, int tolerance);

/// Fills the selection, or the whole layer when there is none.
[[nodiscard]] UndoRecord fillSelection(Document& doc, LayerId target,
                                       StraightRgba8 colour);

/// Move, scale and rotate, as one undoable action.
struct Transform {
    double dx = 0.0, dy = 0.0;    // canvas pixels
    double scaleX = 1.0, scaleY = 1.0;
    double angle = 0.0;           // radians, clockwise, about the region's centre
};

/// Lifts `source` off the layer and puts it back transformed.
///
/// The pixels are read into a buffer before anything is written, so a
/// destination that overlaps the source — which is the normal case for a small
/// nudge — samples the original rather than its own output.
///
/// Sampling is bilinear on PREMULTIPLIED values, which is the only space where
/// interpolating colour and alpha together is correct; doing it on straight
/// alpha darkens every soft edge it touches.
[[nodiscard]] UndoRecord transformRegion(Document& doc, LayerId target,
                                         const Selection& source,
                                         const Transform& transform);

}  // namespace sbl
