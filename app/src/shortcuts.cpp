#include "shortcuts.hpp"

#include <array>

namespace {

/// Only these three modifiers take part. Left and right variants are folded
/// together, so a binding made with the right Ctrl still fires on the left.
constexpr SDL_Keymod kRelevant = SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT;

SDL_Keymod normalise(SDL_Keymod mods) {
    SDL_Keymod out = SDL_KMOD_NONE;
    if ((mods & SDL_KMOD_CTRL)  != 0) out = static_cast<SDL_Keymod>(out | SDL_KMOD_CTRL);
    if ((mods & SDL_KMOD_SHIFT) != 0) out = static_cast<SDL_Keymod>(out | SDL_KMOD_SHIFT);
    if ((mods & SDL_KMOD_ALT)   != 0) out = static_cast<SDL_Keymod>(out | SDL_KMOD_ALT);
    return out;
}

struct Entry {
    Action      action;
    const char* name;
    SDL_Keycode key;
    SDL_Keymod  mods;
};

/// Sable's own defaults, chosen from what Sable actually has (D-101). No SAI
/// binding is treated as authoritative.
constexpr std::array<Entry, static_cast<std::size_t>(Action::Count)> kDefaults{{
    {Action::NewCanvas,     "New canvas",     SDLK_N, SDL_KMOD_CTRL},
    {Action::OpenProject,   "Open project",   SDLK_O, SDL_KMOD_CTRL},
    {Action::Save,          "Save",           SDLK_S, SDL_KMOD_CTRL},
    {Action::SaveAs,        "Save as",        SDLK_S, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)},
    {Action::ExportPng,     "Export PNG",     SDLK_E, SDL_KMOD_CTRL},
    {Action::Quit,          "Quit",           SDLK_Q, SDL_KMOD_CTRL},
    {Action::Undo,          "Undo",           SDLK_Z, SDL_KMOD_CTRL},
    {Action::Redo,          "Redo",           SDLK_Y, SDL_KMOD_CTRL},
    {Action::Clear,         "Clear layer",    SDLK_DELETE, SDL_KMOD_NONE},
    {Action::FillSelection, "Fill selection", SDLK_BACKSPACE, SDL_KMOD_NONE},
    {Action::Deselect,      "Deselect",       SDLK_D, SDL_KMOD_CTRL},
    {Action::FitToWindow,   "Fit to window",  SDLK_0, SDL_KMOD_CTRL},
    {Action::ActualSize,    "Actual size",    SDLK_0, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT)},
    {Action::ZoomIn,        "Zoom in",        SDLK_EQUALS, SDL_KMOD_NONE},
    {Action::ZoomOut,       "Zoom out",       SDLK_MINUS,  SDL_KMOD_NONE},
    // Comma and full stop sit under the drawing hand's neighbours on a normal
    // layout, and the pair reads as "turn back / turn on" without a modifier.
    {Action::RotateLeft,    "Rotate left",    SDLK_COMMA,  SDL_KMOD_NONE},
    {Action::RotateRight,   "Rotate right",   SDLK_PERIOD, SDL_KMOD_NONE},
    // Beside fit-to-window and actual-size on Ctrl+0, because they are the
    // same gesture: put the view back where I can read it.
    {Action::ResetRotation, "Reset rotation", SDLK_0, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)},
    {Action::ToolBrush,     "Brush tool",     SDLK_B, SDL_KMOD_NONE},
    {Action::ToolEraser,    "Eraser tool",    SDLK_E, SDL_KMOD_NONE},
    {Action::ToolFill,      "Fill tool",      SDLK_G, SDL_KMOD_NONE},
    {Action::ToolSelect,    "Select tool",    SDLK_M, SDL_KMOD_NONE},
    {Action::ToolTransform, "Transform tool", SDLK_T, SDL_KMOD_NONE},
    {Action::SizeDown,      "Smaller brush",  SDLK_LEFTBRACKET,  SDL_KMOD_NONE},
    {Action::SizeUp,        "Larger brush",   SDLK_RIGHTBRACKET, SDL_KMOD_NONE},
    {Action::SwapColours,   "Swap colours",   SDLK_X, SDL_KMOD_NONE},
    {Action::ResetColours,  "Reset colours",  SDLK_C, SDL_KMOD_NONE},
    // Unmodified S and P: both letters were free, and a ruler is toggled
    // mid-drawing often enough that a modifier would be in the way.
    {Action::ToggleSymmetry,    "Symmetry ruler",    SDLK_S, SDL_KMOD_NONE},
    {Action::TogglePerspective, "Perspective ruler", SDLK_P, SDL_KMOD_NONE},
}};

std::string settingKey(Action action) {
    return std::string("key.") + Shortcuts::name(action);
}

}  // namespace

bool Binding::matches(SDL_Keycode pressed, SDL_Keymod held) const noexcept {
    if (key == SDLK_UNKNOWN) return false;      // unbound actions never fire
    return pressed == key && normalise(held) == normalise(mods);
}

std::string Binding::label() const {
    if (key == SDLK_UNKNOWN) return "(unbound)";
    std::string out;
    if ((mods & SDL_KMOD_CTRL)  != 0) out += "Ctrl+";
    if ((mods & SDL_KMOD_SHIFT) != 0) out += "Shift+";
    if ((mods & SDL_KMOD_ALT)   != 0) out += "Alt+";
    const char* named = SDL_GetKeyName(key);
    out += (named != nullptr && *named != '\0') ? named : "?";
    return out;
}

Shortcuts::Shortcuts() { resetToDefaults(); }

void Shortcuts::resetToDefaults() {
    bindings_.assign(static_cast<std::size_t>(Action::Count), Binding{});
    for (const Entry& entry : kDefaults)
        bindings_[static_cast<std::size_t>(entry.action)] = Binding{entry.key, entry.mods};
}

const Binding& Shortcuts::get(Action action) const {
    return bindings_[static_cast<std::size_t>(action)];
}

void Shortcuts::set(Action action, Binding binding) {
    binding.mods = normalise(binding.mods);
    // Taking a binding away from whoever held it, rather than leaving two
    // actions on one key and firing whichever comes first in the table.
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        if (i == static_cast<std::size_t>(action)) continue;
        if (bindings_[i].key == binding.key && normalise(bindings_[i].mods) == binding.mods)
            bindings_[i] = Binding{};
    }
    bindings_[static_cast<std::size_t>(action)] = binding;
}

Action Shortcuts::lookup(SDL_Keycode key, SDL_Keymod mods) const {
    for (std::size_t i = 0; i < bindings_.size(); ++i)
        if (bindings_[i].matches(key, mods)) return static_cast<Action>(i);
    return Action::Count;
}

Action Shortcuts::conflictWith(Action action, Binding binding) const {
    const SDL_Keymod wanted = normalise(binding.mods);
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        if (i == static_cast<std::size_t>(action)) continue;
        if (bindings_[i].key == binding.key && normalise(bindings_[i].mods) == wanted)
            return static_cast<Action>(i);
    }
    return Action::Count;
}

const char* Shortcuts::name(Action action) {
    for (const Entry& entry : kDefaults)
        if (entry.action == action) return entry.name;
    return "?";
}

void Shortcuts::load(const Settings& settings) {
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const auto action = static_cast<Action>(i);
        const std::string stored = settings.getString(settingKey(action), "");
        if (stored.empty()) continue;

        // "<mods>:<keycode>", both decimal. Deliberately not the key NAME —
        // names change with the keyboard layout, keycodes do not.
        const std::size_t colon = stored.find(':');
        if (colon == std::string::npos) continue;
        Binding binding;
        binding.mods = static_cast<SDL_Keymod>(std::strtoul(stored.c_str(), nullptr, 10));
        binding.key  = static_cast<SDL_Keycode>(
            std::strtoul(stored.c_str() + colon + 1, nullptr, 10));
        bindings_[i] = binding;
    }
}

void Shortcuts::store(Settings& settings) const {
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const Binding& binding = bindings_[i];
        settings.set(settingKey(static_cast<Action>(i)),
                     std::to_string(static_cast<unsigned>(normalise(binding.mods) & kRelevant)) +
                     ":" + std::to_string(static_cast<unsigned>(binding.key)));
    }
}
