#include "sbl/input.hpp"

#include <algorithm>
#include <cmath>

namespace sbl {
namespace {

/// Fritsch-Carlson slope limiting: the standard way to keep a cubic Hermite
/// interpolant monotonic. Without it the curve overshoots between control
/// points and pressing harder can produce a lighter mark.
float evalMonotone(const std::vector<std::pair<float, float>>& p, float t) noexcept {
    const std::size_t n = p.size();
    if (n == 0) return t;
    if (n == 1) return p[0].second;
    if (t <= p.front().first)  return p.front().second;
    if (t >= p.back().first)   return p.back().second;

    std::size_t i = 0;
    while (i + 2 < n && t > p[i + 1].first) ++i;

    const float x0 = p[i].first,     y0 = p[i].second;
    const float x1 = p[i + 1].first, y1 = p[i + 1].second;
    const float h = x1 - x0;
    if (h <= 0.0f) return y1;

    // Secant slopes either side of this interval.
    const float d = (y1 - y0) / h;
    const float dPrev = i > 0
        ? (y0 - p[i - 1].second) / std::max(x0 - p[i - 1].first, 1e-6f) : d;
    const float dNext = i + 2 < n
        ? (p[i + 2].second - y1) / std::max(p[i + 2].first - x1, 1e-6f) : d;

    // A slope of zero at any extremum is what actually enforces monotonicity.
    const auto tangent = [](float a, float b) {
        return (a * b <= 0.0f) ? 0.0f : (a + b) * 0.5f;
    };
    float m0 = tangent(dPrev, d);
    float m1 = tangent(d, dNext);

    if (d == 0.0f) {
        m0 = m1 = 0.0f;
    } else {
        const float a = m0 / d;
        const float b = m1 / d;
        const float s = a * a + b * b;
        if (s > 9.0f) {                       // outside the monotonicity region
            const float scale = 3.0f / std::sqrt(s);
            m0 = scale * a * d;
            m1 = scale * b * d;
        }
    }

    const float s  = (t - x0) / h;
    const float s2 = s * s;
    const float s3 = s2 * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * y0 + (s3 - 2.0f * s2 + s) * h * m0 +
           (-2.0f * s3 + 3.0f * s2) * y1     + (s3 - s2) * h * m1;
}

}  // namespace

// ------------------------------------------------------------ pressure curve

float PressureCurve::eval(float raw) const noexcept {
    return std::clamp(evalMonotone(points, std::clamp(raw, 0.0f, 1.0f)), 0.0f, 1.0f);
}

void PressureCurve::normalise() {
    for (auto& [x, y] : points) {
        x = std::clamp(x, 0.0f, 1.0f);
        y = std::clamp(y, 0.0f, 1.0f);
    }
    std::ranges::sort(points, {}, &std::pair<float, float>::first);
}

PressureCurve curveLinear() { return PressureCurve{{{0.0f, 0.0f}, {1.0f, 1.0f}}}; }

PressureCurve curveSoft() {
    // Above the diagonal: a light touch already produces a strong mark.
    return PressureCurve{{{0.0f, 0.0f}, {0.25f, 0.45f}, {0.6f, 0.82f}, {1.0f, 1.0f}}};
}

PressureCurve curveHard() {
    // Below the diagonal: full weight needs a firm press.
    return PressureCurve{{{0.0f, 0.0f}, {0.4f, 0.18f}, {0.75f, 0.55f}, {1.0f, 1.0f}}};
}

// ----------------------------------------------------------- pressure filter

float PressureFilter::apply(const TabletProfile& profile, float raw) noexcept {
    // 1. deadzone and clamp
    const float lo = std::clamp(profile.rawMin, 0.0f, 1.0f);
    const float hi = std::clamp(profile.rawMax, 0.0f, 1.0f);
    const float clamped = std::clamp(raw, lo, hi);

    // 2. rescale to 0..1
    const float span = hi - lo;
    rescaled_ = span > 1e-6f ? (clamped - lo) / span : (clamped >= hi ? 1.0f : 0.0f);

    // 3. smoothing (EMA), for devices that report noisy pressure
    float smoothed = rescaled_;
    const float alpha = std::clamp(profile.smoothing, 0.0f, 1.0f);
    if (alpha > 0.0f) {
        if (!primed_) { ema_ = rescaled_; primed_ = true; }
        // alpha 0 = no smoothing, alpha 1 = maximum lag but never frozen.
        ema_ = ema_ + (rescaled_ - ema_) * (1.0f - alpha * 0.95f);
        smoothed = ema_;
    } else {
        primed_ = false;
    }

    // 4. the artist's curve, last
    return profile.curve.eval(smoothed);
}

// ---------------------------------------------------------------- stabilizer

void Stabilizer::setLevel(std::uint8_t level) noexcept {
    level_ = std::min<std::uint8_t>(level, 3);
    // Canvas pixels of string. Tuned by feel, so leave the knob rather than
    // the constants — a number that suits a 1024 px sketch is wrong on 4000.
    constexpr float kLengths[] = {0.0f, 4.0f, 12.0f, 32.0f};
    length_ = kLengths[level_];
}

InputSample Stabilizer::apply(const InputSample& in) noexcept {
    if (level_ == 0 || length_ <= 0.0f) return in;   // US-11.2: no smoothing at all

    if (!primed_) {
        x_ = in.x;
        y_ = in.y;
        primed_ = true;
        return in;
    }

    const double dx = in.x - x_;
    const double dy = in.y - y_;
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist > length_) {
        // The string is taut: drag the brush to trail the cursor by exactly
        // the string length. Below that the brush does not move at all, which
        // is what removes the wobble and what lets a corner stay sharp.
        //
        // A longer string converges more slowly from the stroke's first
        // sample. That is latency, not wobble — do not "fix" it by damping the
        // drag, which only makes the lag worse. The steady-state test is the
        // one that means anything here.
        const double pull = (dist - length_) / dist;
        x_ += dx * pull;
        y_ += dy * pull;
    }

    InputSample out = in;      // pressure, tilt and timestamp pass through
    out.x = x_;
    out.y = y_;
    return out;
}

InputSample Stabilizer::finish(const InputSample& last) noexcept {
    // Release the string. Without this the stroke stops short of where the
    // artist lifted, by up to the full string length (US-11.4).
    x_ = last.x;
    y_ = last.y;
    primed_ = false;
    return last;
}

}  // namespace sbl
