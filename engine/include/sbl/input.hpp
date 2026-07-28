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

}  // namespace sbl
