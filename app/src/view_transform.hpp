// The screen <-> canvas mapping: pan, zoom and rotation.
//
// Split out of canvas_view.hpp because it is the one part of the viewport that
// needs no window: the engine tests link no SDL (D-003), and a transform that
// silently loses the rotation on one path is exactly the bug that has to be
// provable by a round-trip test rather than by eye. At small angles a forgotten
// rotation still looks nearly right.
#pragma once

#include <cmath>

/// Pan is the screen position of canvas pixel (0, 0); zoom scales about that
/// origin and rotation turns about it. Screen y grows downwards, so a positive
/// angle reads as clockwise.
///
/// The canvas the artist sees turning is really the whole mapping turning about
/// the origin — the rotate action re-anchors pan afterwards so the point under
/// the cursor stays put, the same trick zoom already uses.
struct View {
    double panX     = 0.0;
    double panY     = 0.0;
    float  zoom     = 1.0f;
    double rotation = 0.0;   ///< radians, in (-pi, pi]
};

[[nodiscard]] inline double toScreenX(const View& v, double cx, double cy) noexcept {
    return v.panX + (cx * std::cos(v.rotation) - cy * std::sin(v.rotation)) * v.zoom;
}
[[nodiscard]] inline double toScreenY(const View& v, double cx, double cy) noexcept {
    return v.panY + (cx * std::sin(v.rotation) + cy * std::cos(v.rotation)) * v.zoom;
}

/// The full inverse: unpan, unscale, then unrotate. Both screen coordinates are
/// needed for either canvas coordinate once the view is turned, which is why
/// these take a point rather than an axis.
[[nodiscard]] inline double toCanvasX(const View& v, double sx, double sy) noexcept {
    const double dx = (sx - v.panX) / v.zoom;
    const double dy = (sy - v.panY) / v.zoom;
    return dx * std::cos(v.rotation) + dy * std::sin(v.rotation);
}
[[nodiscard]] inline double toCanvasY(const View& v, double sx, double sy) noexcept {
    const double dx = (sx - v.panX) / v.zoom;
    const double dy = (sy - v.panY) / v.zoom;
    return -dx * std::sin(v.rotation) + dy * std::cos(v.rotation);
}

/// Moves pan so canvas point (cx, cy) lands on screen point (sx, sy).
inline void anchorAt(View& v, double cx, double cy, double sx, double sy) noexcept {
    const View unpanned{0.0, 0.0, v.zoom, v.rotation};
    v.panX = sx - toScreenX(unpanned, cx, cy);
    v.panY = sy - toScreenY(unpanned, cx, cy);
}

/// Turns the view by `delta` radians about a screen point, so the canvas pixel
/// there stays under the cursor. Passing `-v.rotation` is the reset.
inline void rotateAbout(View& v, double sx, double sy, double delta) noexcept {
    const double cx = toCanvasX(v, sx, sy);
    const double cy = toCanvasY(v, sx, sy);
    // remainder folds to (-pi, pi], so the status readout says -15 after one
    // step left rather than 345, and repeated turns cannot drift off to 1e9.
    v.rotation = std::remainder(v.rotation + delta, 2.0 * 3.14159265358979323846);
    anchorAt(v, cx, cy, sx, sy);
}

[[nodiscard]] inline double rotationDegrees(const View& v) noexcept {
    return v.rotation * (180.0 / 3.14159265358979323846);
}
