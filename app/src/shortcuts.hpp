// Reassignable keyboard bindings.
//
// PRD §6 commits to this as a hard requirement rather than a convenience:
// with no AT-SPI and therefore no screen reader, the keyboard is the ONLY
// accessibility affordance Sable ships. Every action is reachable from it, and
// every binding can be changed — including by users who cannot use the
// defaults comfortably.
//
// D-101 is why the defaults are ours rather than copied: the SAI shortcut
// references this project started from contradicted each other, and one was a
// single artist's personal setup.
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "settings.hpp"

enum class Action {
    NewCanvas, OpenProject, Save, SaveAs, ExportPng, Quit,
    // #50. Reassignable like everything else here: Ctrl+Tab in particular is
    // taken by the window manager on some desktops, and an artist who cannot
    // reach their other document has lost the feature.
    CloseDocument, NextDocument, PreviousDocument,
    Undo, Redo, Copy, Paste, Clear, FillSelection, Deselect,
    FitToWindow, ActualSize, ZoomIn, ZoomOut,
    RotateLeft, RotateRight, ResetRotation,
    ToolBrush, ToolEraser, ToolFill, ToolSelect, ToolLasso, ToolWand,
    ToolTransform, ToolText, ToolLinework, ToolGradient,
    SizeDown, SizeUp, SwapColours, ResetColours,
    ToggleSymmetry, TogglePerspective,
    GettingStarted,
    Count,
};

struct Binding {
    SDL_Keycode key  = SDLK_UNKNOWN;
    SDL_Keymod  mods = SDL_KMOD_NONE;   // only Ctrl / Shift / Alt are considered

    [[nodiscard]] bool matches(SDL_Keycode pressed, SDL_Keymod held) const noexcept;
    [[nodiscard]] std::string label() const;
};

class Shortcuts {
public:
    Shortcuts();   // built-in defaults

    [[nodiscard]] const Binding& get(Action action) const;
    void set(Action action, Binding binding);
    void resetToDefaults();

    /// Which action a key press triggers, or Action::Count for none.
    [[nodiscard]] Action lookup(SDL_Keycode key, SDL_Keymod mods) const;

    /// A binding may only drive one action, or the artist gets silent
    /// surprises. Returns the action that already owns it, if any.
    [[nodiscard]] Action conflictWith(Action action, Binding binding) const;

    void load(const Settings& settings);
    void store(Settings& settings) const;

    [[nodiscard]] static const char* name(Action action);

private:
    std::vector<Binding> bindings_;
};
