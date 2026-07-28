// The input boundary: raw device values in, normalised samples out.
// SDL stops here — no SDL_ type appears past this point (D-003).
//
// The driver supplies raw input; Sable owns what it means (PRD §13).
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sbl {

/// The engine boundary type.
struct InputSample {
    double x = 0.0, y = 0.0;      // canvas pixels, sub-pixel, NOT screen pixels
    float pressure = 1.0f;        // 0..1 AFTER TabletProfile normalisation
    float tiltX    = 0.0f;        // radians; 0 if the device does not report it
    float tiltY    = 0.0f;
    float rotation = 0.0f;        // radians; 0 if unsupported
    std::uint64_t timestampMs = 0;
    bool fromMouse = false;       // true => pressure is synthetic
};

// ------------------------------------------------------------ pressure curve

/// Monotonic control points in 0..1, evaluated by monotone cubic
/// interpolation. Soft / Normal / Hard presets are just stored point sets.
///
/// Monotone rather than plain cubic on purpose: an ordinary spline through
/// artist-placed points overshoots, and an overshooting pressure curve means
/// pressing harder can make the mark *lighter*.
struct PressureCurve {
    std::vector<std::pair<float, float>> points;

    [[nodiscard]] float eval(float raw) const noexcept;
    /// Keeps points sorted and inside 0..1. Call after editing.
    void normalise();
};

[[nodiscard]] PressureCurve curveLinear();
[[nodiscard]] PressureCurve curveSoft();     // light touch reaches full sooner
[[nodiscard]] PressureCurve curveHard();     // needs a firm press

/// Owned per device, not per brush. Raw driver values are hardware; the curve
/// is the artist's calibration.
struct TabletProfile {
    std::string deviceKey;     // stable SDL_PenID plus the name SDL reports
    float rawMin    = 0.0f;    // deadzone: below this reads as no contact
    float rawMax    = 1.0f;    // clamp: above this reads as full pressure
    float smoothing = 0.0f;    // 0..1, EMA over pressure for noisy devices
    PressureCurve curve = curveLinear();
};

/// The running state the smoothing step needs. Settings live in the profile;
/// this is per-stroke and must be reset at pen-down.
class PressureFilter {
public:
    void reset() noexcept { primed_ = false; ema_ = 0.0f; }

    /// Normalisation order is FIXED: clamp to [rawMin, rawMax] -> rescale to
    /// 0..1 -> smoothing -> curve.eval().
    ///
    /// Applying the curve before the deadzone gives a device-dependent feel
    /// and is the likely cause of any "my pen jumps straight to full black"
    /// report. The order is asserted in a unit test because the bug it catches
    /// is very hard to diagnose from a user's description (US-09.7).
    [[nodiscard]] float apply(const TabletProfile& profile, float raw) noexcept;

    /// What the deadzone and rescale produced, before smoothing and the
    /// curve. The test pad shows both (US-10.1).
    [[nodiscard]] float lastRescaled() const noexcept { return rescaled_; }

private:
    float ema_      = 0.0f;
    float rescaled_ = 0.0f;
    bool  primed_   = false;
};

// ------------------------------------------------------------------ stabilizer

/// Pulled-string smoothing (D-103): the brush trails the cursor on a string of
/// fixed length, and is dragged along only once the cursor pulls it taut.
///
/// Chosen over a moving average because it holds corners — an average rounds
/// off a sharp V, which is exactly what line art cannot afford (US-03.3).
///
/// Positions only. Pressure response is untouched (US-11.6).
class Stabilizer {
public:
    /// 0 = off, 1 = low, 2 = medium, 3 = high. Off is a pure pass-through
    /// with no smoothing and no added latency (US-11.2).
    void setLevel(std::uint8_t level) noexcept;
    [[nodiscard]] std::uint8_t level() const noexcept { return level_; }
    [[nodiscard]] float stringLength() const noexcept { return length_; }

    void reset() noexcept { primed_ = false; }

    /// The smoothed sample to paint with.
    [[nodiscard]] InputSample apply(const InputSample& in) noexcept;

    /// At pen-up the string is released, so the stroke ends where the artist
    /// lifted rather than short of it (US-11.4). Returns the sample to paint
    /// last; call once, after the final apply().
    [[nodiscard]] InputSample finish(const InputSample& last) noexcept;

private:
    std::uint8_t level_  = 0;
    float        length_ = 0.0f;
    double       x_ = 0.0, y_ = 0.0;
    bool         primed_ = false;
};

// ---------------------------------------------------------------------- rulers
//
// A ruler is a function from a proposed sample to a constrained one — the same
// shape as the stabilizer, and it sits in the same place in the pipeline. The
// order is stabilizer first, then perspective: smoothing a projected point
// would drag it off its own guide line, so the projection has to come last.
//
// Symmetry is the exception. It does not move a sample, it multiplies the dabs
// the sample produces, so it applies one step further down — after the
// interpolator has decided where the dabs go.

/// A perspective guide, in canvas pixels.
///
/// Saved with the document: the geometry of a scene belongs to the drawing.
/// Whether the ruler is switched on does not, and is not saved.
struct VanishingPoint {
    double x = 0.0, y = 0.0;
    bool   enabled = true;
};

/// Constrains a stroke to the line through where it started and a vanishing
/// point, so every mark converges on the same horizon.
///
/// Which point is used is decided from the stroke's own opening direction
/// rather than from a mode the artist has to set: with two or three points on
/// the canvas, "the one I am drawing towards" is unambiguous and needs no UI.
class PerspectiveRuler {
public:
    std::vector<VanishingPoint> points;   ///< 1, 2 or 3 in practice
    bool enabled = false;

    /// Call at pen-down. The guide is anchored to the first sample.
    void reset() noexcept { primed_ = false; chosen_ = -1; }

    /// Which point the live stroke locked onto, or -1 before it has committed.
    [[nodiscard]] int chosen() const noexcept { return chosen_; }
    [[nodiscard]] bool usable() const noexcept;

    [[nodiscard]] InputSample apply(const InputSample& in) noexcept;

private:
    /// True once a guide has been picked. The first few pixels of a stroke are
    /// direction noise, so choosing from them would lock onto the wrong point
    /// and there is no undoing that mid-stroke.
    bool choose(double x, double y) noexcept;

    double anchorX_ = 0.0, anchorY_ = 0.0;
    double dirX_ = 1.0, dirY_ = 0.0;   // unit vector along the chosen guide
    int    chosen_ = -1;
    bool   primed_ = false;
};

/// One image of a dab under symmetry: where it lands and how it leans.
struct SymmetryImage {
    double x = 0.0, y = 0.0;
    float  angle = 0.0f;
};

/// Mirrors dabs about a centre. Positions and configuration are independent of
/// `enabled`, so switching the ruler off and on again keeps the guides where
/// the artist put them.
struct SymmetryRuler {
    double centreX = 0.0, centreY = 0.0;
    bool   vertical   = false;   ///< mirror across the vertical axis x = centreX
    bool   horizontal = false;   ///< mirror across the horizontal axis y = centreY
    int    radial     = 1;       ///< rotational copies about the centre; 1 = none
    bool   enabled    = false;

    /// More than this and the copies are closer together than a dab is wide,
    /// which costs paint time and shows nothing.
    static constexpr int kMaxRadial = 32;

    [[nodiscard]] bool active() const noexcept {
        return enabled && (vertical || horizontal || radial > 1);
    }

    /// Every image of (x, y), the ORIGINAL FIRST, into `out` (cleared).
    ///
    /// A point sitting exactly on an axis maps onto itself and is emitted
    /// twice, so the seam is painted twice. That is inherent to mirroring —
    /// the artist is drawing on the axis — and de-duplicating it would cost a
    /// comparison per dab pair for a case nobody notices.
    void map(double x, double y, float angle, std::vector<SymmetryImage>& out) const;
};

}  // namespace sbl
