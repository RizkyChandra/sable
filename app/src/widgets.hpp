// Custom ImGui widgets. ImGui gives sliders, menus, windows and drawing
// primitives; the colour wheel and the curve editor are ours to write (D-002).
//
// These are the calibration point for how much widget work M3 and M4 need, so
// they are deliberately built on nothing but ImDrawList.
#pragma once

#include "sbl/input.hpp"

/// Hue ring plus a saturation/value square. `hsv` is three floats in 0..1.
/// Returns true while the artist is changing it.
bool colourWheel(const char* id, float hsv[3], float diameter);

/// Plots the curve and lets control points be dragged. `marker` in 0..1 draws
/// the current input on the curve (US-10.2); pass a negative value for none.
/// Returns true when a point moved.
bool pressureCurveEditor(const char* id, sbl::PressureCurve& curve, float size,
                         float marker = -1.0f);
