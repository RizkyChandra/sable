// Vector icons, drawn with ImDrawList primitives.
//
// No icon font and no bundled artwork, for three reasons that all point the
// same way:
//
//   * D-010 — nothing recognisable as another application's icon set. Drawing
//     them from primitives makes them original by construction.
//   * D-003 — a paint application's dependency list should be boring. An icon
//     font is a binary asset plus font-atlas merging plus a licence to track.
//   * Interface scaling (PRD §6) — these are described in a 0..1 box and drawn
//     at whatever size they are asked for, so they stay sharp at 2.5x where a
//     bitmap would not.
//
// Every icon control keeps a tooltip. Icon-only buttons are worse for
// low-vision users than labelled ones, and with no screen reader available the
// tooltip is the only thing carrying the name.
#pragma once

#include "imgui.h"

enum class Icon {
    Brush, Eraser, Fill, Select, Lasso, Wand, Transform, Text, Linework, Gradient, Hand,
    Plus, Duplicate, Delete, Merge, Group, Ungroup, Raise, Lower,
    Eye, EyeClosed, Lock, Swap, Reset,
};

/// Draws into a square of `size` at `topLeft`.
void drawIcon(ImDrawList* draw, Icon icon, ImVec2 topLeft, float size, ImU32 colour);

/// A square button carrying an icon. `id` must be unique within the window.
bool iconButton(Icon icon, const char* id, const char* tooltip, float size = 0.0f);

/// A button that stays visibly held while `active`.
bool iconToggle(Icon icon, const char* id, bool active, const char* tooltip,
                float size = 0.0f);

/// Icon plus label, for the wider panels where the name should stay visible.
bool iconRadio(Icon icon, const char* label, bool active);
