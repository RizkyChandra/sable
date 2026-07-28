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

// --------------------------------------------------------- perspective ruler

bool PerspectiveRuler::usable() const noexcept {
    if (!enabled) return false;
    for (const VanishingPoint& p : points)
        if (p.enabled) return true;
    return false;
}

bool PerspectiveRuler::choose(double x, double y) noexcept {
    const double dx = x - anchorX_;
    const double dy = y - anchorY_;
    const double len = std::sqrt(dx * dx + dy * dy);
    // Canvas pixels of travel before the direction is trusted. Under a couple
    // of pixels the "direction" is hand tremor, and a guide picked from tremor
    // is one the artist cannot correct without lifting the pen.
    constexpr double kCommitDistance = 4.0;
    if (len < kCommitDistance) return false;

    double best = -1.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!points[i].enabled) continue;
        double ux = points[i].x - anchorX_;
        double uy = points[i].y - anchorY_;
        const double ulen = std::sqrt(ux * ux + uy * uy);
        if (ulen < 1e-9) continue;      // the stroke started on the point itself
        ux /= ulen;
        uy /= ulen;
        // |cos| rather than cos: a receding line is drawn away from the
        // vanishing point as often as towards it, and both lie on one guide.
        const double score = std::abs((dx * ux + dy * uy) / len);
        if (score > best) {
            best = score;
            chosen_ = static_cast<int>(i);
            dirX_ = ux;
            dirY_ = uy;
        }
    }
    return chosen_ >= 0;
}

InputSample PerspectiveRuler::apply(const InputSample& in) noexcept {
    if (!usable()) return in;

    if (!primed_) {
        anchorX_ = in.x;
        anchorY_ = in.y;
        primed_ = true;
        return in;
    }
    if (chosen_ < 0 && !choose(in.x, in.y)) return in;

    // Foot of the perpendicular onto the guide. Projecting rather than
    // extending means the artist still controls how far along the line the
    // stroke reaches, and can draw back down it.
    const double t = (in.x - anchorX_) * dirX_ + (in.y - anchorY_) * dirY_;
    InputSample out = in;              // pressure and tilt pass through
    out.x = anchorX_ + t * dirX_;
    out.y = anchorY_ + t * dirY_;
    return out;
}

// ------------------------------------------------------------ symmetry ruler

void SymmetryRuler::map(double x, double y, float angle,
                        std::vector<SymmetryImage>& out) const {
    out.clear();
    if (!active()) {
        out.push_back(SymmetryImage{x, y, angle});
        return;
    }

    const double rx = x - centreX;
    const double ry = y - centreY;
    const auto emit = [&](double px, double py, float a) {
        out.push_back(SymmetryImage{centreX + px, centreY + py, a});
    };

    if (radial > 1) {
        // Dihedral: n rotations, plus their mirrors when either axis is on.
        // Generating them this way is what keeps the set duplicate-free —
        // listing the flags separately alongside the rotations produces the
        // same transform twice for even n, and a doubled dab is a dark blotch.
        const int n = std::min(radial, kMaxRadial);
        const bool mirrored = vertical || horizontal;
        constexpr double kTau = 6.283185307179586476925286766559;
        for (int k = 0; k < n; ++k) {
            const double a = kTau * k / n;
            const double c = std::cos(a), s = std::sin(a);
            emit(rx * c - ry * s, rx * s + ry * c, angle + static_cast<float>(a));
            if (mirrored) {
                // Mirror in x first, then the same rotation. Reflection turns
                // a leaning dab the other way, or the mirror reads as a copy.
                const double mx = -rx;
                emit(mx * c - ry * s, mx * s + ry * c,
                     static_cast<float>(3.14159265358979323846 + a) - angle);
            }
        }
        return;
    }

    emit(rx, ry, angle);
    if (vertical)   emit(-rx, ry, static_cast<float>(3.14159265358979323846) - angle);
    if (horizontal) emit(rx, -ry, -angle);
    if (vertical && horizontal)
        emit(-rx, -ry, angle + static_cast<float>(3.14159265358979323846));
}

}  // namespace sbl
