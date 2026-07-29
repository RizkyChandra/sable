// Sable — Milestones 1 and 2.
//
// M1: a window, a canvas, a brush, undo, and PNG export.
// M2: pen pressure drives size and density, a stabilizer, calibration, colour.
//
// The loop is event-driven, not a game loop (D-008). It blocks on
// SDL_WaitEvent and redraws only when there is something to redraw, so the
// application costs nothing while the artist thinks (US-14).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <optional>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imgui.h"
// DockBuilder is imgui_internal.h, not the public API — it has no stability
// promise. Only the default-layout code uses it, so an upstream change breaks
// the initial arrangement and nothing else. Accepted as part of D-016.
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "canvas_view.hpp"
#include "icons.hpp"
#include "sbl/backend.hpp"
#include "sbl/canvas.hpp"
#include "sbl/format.hpp"
#include "sbl/gpu.hpp"
#include "sbl/io.hpp"
#include "sbl/paint.hpp"
#include "sbl/project.hpp"
#include "sbl/select.hpp"
#include "settings.hpp"
#include "shortcuts.hpp"
#include "linework_tool.hpp"
#include "text_tool.hpp"
#include "widgets.hpp"

namespace {

constexpr int   kDefaultCanvas = 1024;
constexpr float kZoomStep      = 1.15f;
constexpr float kLeftPanelBase  = 200.0f;
constexpr float kRightPanelBase = 250.0f;
constexpr float kDegToRad      = 3.14159265358979323846f / 180.0f;

/// What to do once the artist answers the "discard unsaved work?" prompt.
enum class Pending { None, NewCanvas, Quit, Open, Import };

enum class Tool { Brush, Eraser, Fill, Select, Lasso, Wand, Transform, Text, Linework,
                  Gradient };

/// True for the three tools that build a selection, which share the modifier
/// keys, the mode setting, and the rule that a click with no drag deselects.
[[nodiscard]] constexpr bool selectsRegion(Tool t) noexcept {
    return t == Tool::Select || t == Tool::Lasso || t == Tool::Wand;
}

/// How often a recovery copy is written while the document is dirty (D-013).
constexpr std::uint64_t kAutosaveIntervalMs = 120'000;

/// What `App::draggingGuide` holds when it is not an index into the document's
/// vanishing points.
constexpr int kNoGuide       = -1;
constexpr int kSymmetryGuide = -2;

struct NewCanvasForm {
    int  width  = kDefaultCanvas;
    int  height = kDefaultCanvas;
    bool transparent = false;
    /// D-023. Off by default and not remembered between documents: it doubles
    /// the memory and halves the undo history, so it should be a thing the
    /// artist chooses for a drawing rather than a setting they turned on once
    /// and stopped seeing.
    bool wideColour = false;
};

/// SDL delivers axis changes and motion as SEPARATE events. There is no single
/// event carrying both position and pressure, so the app keeps the latest axis
/// values per pen and snapshots them when a motion event arrives.
struct PenAxisState {
    float pressure = 0.0f;
    float tiltX = 0.0f, tiltY = 0.0f;
    float rotation = 0.0f;
    float distance = 0.0f;
};

struct App {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    CanvasView*   canvas   = nullptr;

    sbl::Document doc;
    View          view;

    std::vector<sbl::BrushPreset> brushes = sbl::defaultBrushes();
    std::size_t brushIndex = 0;
    sbl::BrushPreset eraser = sbl::defaultEraser();
    Tool tool = Tool::Brush;
    int  fillTolerance = 16;

    // Selection drag, in canvas coordinates.
    bool   selecting = false;
    double selectAnchorX = 0.0, selectAnchorY = 0.0;
    int    wandTolerance = 24;

    /// The modifier the panel is set to, and the one this drag is actually
    /// using — the modifier keys are read once at pen-down, so letting go of
    /// Shift halfway through a lasso does not change what the loop means.
    sbl::SelectMode selectMode = sbl::SelectMode::Replace;
    sbl::SelectMode dragMode   = sbl::SelectMode::Replace;
    /// What was selected when the drag began. Add and subtract are computed
    /// against this every motion event, so dragging back and forth does not
    /// accumulate.
    std::optional<sbl::Selection> selectBase;
    std::vector<sbl::Point> lassoPath;

    /// The selection boundary in canvas coordinates, cached because walking a
    /// megapixel mask at every redraw to draw the same outline is exactly the
    /// sort of thing modest hardware notices. Rebuilt when the selection it was
    /// built from stops matching the document's.
    ///
    /// ponytail: the staleness check compares the whole mask, so a very large
    /// selection costs a memcmp per frame — far less than the rebuild it
    /// avoids. Give Selection a change counter if that ever shows up in a
    /// profile.
    struct Ants {
        sbl::Selection from;
        std::vector<std::array<double, 4>> segments;   // x0, y0, x1, y1
    } ants;

    sbl::StraightRgba8 foreground{0, 0, 0, 255};
    sbl::StraightRgba8 background{255, 255, 255, 255};
    float foregroundHsv[3]{0.0f, 0.0f, 0.0f};

    std::vector<float> sizePresets{2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f};

    sbl::Stroke           stroke;
    std::vector<sbl::Dab> scratch;
    bool painting = false;

    // --- gradient (#49). The drag anchor is in canvas coordinates, so the
    // preview follows the art rather than the screen when the view is rotated
    // or zoomed mid-drag.
    sbl::GradientShape gradientShape = sbl::GradientShape::Linear;
    bool   gradientToTransparent = false;
    bool   gradientDither = true;
    bool   gradientDragging = false;
    /// Whether the record on top of the undo stack is this drag's preview, and
    /// therefore ours to take back off before drawing the next one.
    bool   gradientPreviewed = false;
    double gradientAnchorX = 0.0, gradientAnchorY = 0.0;

    TextTool text;
    LineworkTool linework;

    // --- tablet
    std::unordered_map<SDL_PenID, PenAxisState> penAxes;
    sbl::TabletProfile  profile;              // see D-015: one profile, all pens
    sbl::PressureFilter pressureFilter;
    sbl::Stabilizer     stabilizer;

    // --- rulers. The vanishing points themselves live in the document, which
    // is what saves them; `perspective.points` is a per-stroke snapshot of it.
    sbl::PerspectiveRuler perspective;
    sbl::SymmetryRuler    symmetry;
    std::vector<sbl::SymmetryImage> mirrors;   // scratch, one dab's images
    int draggingGuide = kNoGuide;

    SDL_PenID activePen = 0;
    bool      penSeen   = false;
    bool      lastFromMouse = true;
    float     lastRawPressure  = 0.0f;
    float     lastNormPressure = 0.0f;
    float     lastTiltX = 0.0f, lastTiltY = 0.0f;
    double    lastCanvasX = 0.0, lastCanvasY = 0.0;
    int       motionThisFrame = 0;
    int       motionLastFrame = 0;
    int       lastPenButton = 0;

    bool   panning    = false;
    bool   spaceHeld  = false;
    double panAnchorX = 0.0;
    double panAnchorY = 0.0;

    SDL_FRect viewport{};
    bool      running = true;
    int       framesToSettle = 0;   // ImGui sometimes needs a second frame

    bool showTestPad     = false;
    bool showCalibration = false;
    bool showShortcuts   = false;
    bool resetLayout     = false;
    bool lightTheme      = false;
    bool appliedLight    = false;
    sbl::Transform pendingTransform;   // edited live, applied as one step
    int undoBudgetMb = 256;            // D-102, mirrored into the document
    char presetName[48] = "My brush";

    // --- the GPU backend (D-021: opt in, CPU is the default and the reference)
    std::unique_ptr<sbl::PaintBackend> gpu;
    std::string gpuWhy = "not tried yet";
    bool useGpu = false;

    /// Things the artist has to be told but must not be interrupted by: what an
    /// import dropped (#40), why the GPU was refused (#15). The status bar
    /// shows them in the same amber as the dropped-undo notice.
    ///
    /// Replaced, never queued. The newest thing worth saying is the one on
    /// screen, and a queue would mean a stale message outliving what caused it.
    std::vector<std::string> notices;

    Shortcuts shortcuts;
    Action    rebinding = Action::Count;   // which row is waiting for a key
    /// Interface scale, for low-vision users (PRD §6).
    float uiScale = 1.0f;
    float appliedScale = 0.0f;

    // --- background recovery (D-013)
    std::thread       autosaveWorker;
    std::atomic<bool> autosaveBusy{false};
    std::uint64_t     lastAutosaveMs = 0;
    std::mutex        recoveryMutex;
    std::string       lastRecoveryFile;
    std::vector<sbl::RecoveryEntry> offeredRecoveries;
    bool openRecovery = false;
    bool openAbout    = false;

    /// Which dialog the pending save/open belongs to.
    enum class FileAction { ExportImage, SaveProject, OpenProject, ImportDocument }
        fileAction = FileAction::ExportImage;

    // The filters of the dialog currently open. SDL_DialogFileFilter holds
    // pointers rather than copies, so the strings have to outlive the dialog —
    // and the chosen index is what decides the extension of a bare filename.
    std::vector<sbl::DialogFilter>    filterList;
    std::vector<SDL_DialogFileFilter> filterViews;
    int                               chosenFilter = -1;

    // Dialog results arrive on SDL's thread, not ours.
    std::mutex  dialogMutex;
    std::string dialogPath;
    bool        dialogReady = false;

    NewCanvasForm form;
    Pending       pending       = Pending::None;
    /// A Pending::Open that already knows its file — a dropped path. Empty
    /// means the artist still has to choose one in the dialog.
    std::string   pendingOpenPath;
    bool          openNew       = false;
    bool          openDiscard   = false;
    bool          openOverwrite = false;
    std::string   overwritePath;
    std::string   errorMessage;
    bool          openError = false;
};

[[nodiscard]] sbl::BrushPreset& activeBrush(App& app) {
    if (app.tool == Tool::Eraser) return app.eraser;
    return app.brushes[std::min(app.brushIndex, app.brushes.size() - 1)];
}

[[nodiscard]] bool paintingTool(const App& app) {
    return app.tool == Tool::Brush || app.tool == Tool::Eraser;
}

/// Panels scale with the interface, or raising the scale for a low-vision
/// user just clips the controls instead of enlarging them.
[[nodiscard]] float leftPanel(const App& app)  { return kLeftPanelBase  * app.uiScale; }
[[nodiscard]] float rightPanel(const App& app) { return kRightPanelBase * app.uiScale; }

[[nodiscard]] const char* toolName(const App& app) {
    switch (app.tool) {
        case Tool::Brush:  return "brush";
        case Tool::Eraser: return "eraser";
        case Tool::Fill:   return "fill";
        case Tool::Select: return "select";
        case Tool::Lasso:  return "lasso";
        case Tool::Wand:   return "wand";
        case Tool::Transform: return "transform";
        case Tool::Text:   return "text";
        case Tool::Linework: return "linework";
        case Tool::Gradient: return "gradient";
    }
    return "brush";
}

// -------------------------------------------------------------------- rulers

/// The symmetry axes start down the middle, which is where a mirrored figure
/// is drawn from. Not saved with the document — the ruler is a way of working,
/// not part of the picture.
void centreSymmetry(App& app) {
    app.symmetry.centreX = app.doc.width  * 0.5;
    app.symmetry.centreY = app.doc.height * 0.5;
}

/// Lays out `count` vanishing points as a starting arrangement.
///
/// Deliberately just inside the canvas rather than at the far-off positions a
/// finished perspective usually wants: a point the artist can see is a point
/// they can drag, and dragging it out is easier than hunting for one parked
/// three canvas widths off screen.
void setVanishingPoints(App& app, int count) {
    const double w = app.doc.width, h = app.doc.height;
    app.doc.vanishingPoints.clear();
    if (count >= 1) app.doc.vanishingPoints.push_back({w * 0.05, h * 0.5, true});
    if (count >= 2) app.doc.vanishingPoints.push_back({w * 0.95, h * 0.5, true});
    // The third is vertical: the one that makes a building lean away.
    if (count >= 3) app.doc.vanishingPoints.push_back({w * 0.5, h * 0.95, true});
    // One-point perspective has one point, and it belongs in the middle.
    if (count == 1) app.doc.vanishingPoints[0].x = w * 0.5;
    app.doc.dirty = true;
}

// --------------------------------------------------------------- persistence

void applySettings(App& app, const Settings& settings) {
    // Every brush keeps its own size and stabilizer level (US-11.5, US-12.3).
    for (sbl::BrushPreset& brush : app.brushes) {
        brush.size = settings.getFloat("brush." + brush.id + ".size", brush.size);
        brush.stabilizerLevel = static_cast<std::uint8_t>(std::clamp(
            settings.getInt("brush." + brush.id + ".stabilizer", 0), 0, 3));
    }
    app.eraser.size = settings.getFloat("eraser.size", app.eraser.size);
    app.eraser.stabilizerLevel = static_cast<std::uint8_t>(
        std::clamp(settings.getInt("eraser.stabilizer", 0), 0, 3));
    app.brushIndex = static_cast<std::size_t>(std::clamp(
        settings.getInt("brush.active", 0), 0, static_cast<int>(app.brushes.size()) - 1));
    app.fillTolerance = std::clamp(settings.getInt("fill.tolerance", 16), 0, 255);
    // Its own level, like every brush: a line wants more smoothing than a
    // sketch does, and one shared number would have the artist re-setting it
    // every time they switch.
    app.linework.stabilizerLevel = static_cast<std::uint8_t>(
        std::clamp(settings.getInt("linework.stabilizer", 0), 0, 3));

    const std::string presets = settings.getString("brush.sizePresets", "");
    if (!presets.empty()) {
        std::vector<float> parsed;
        std::size_t start = 0;
        while (start < presets.size()) {
            const std::size_t comma = presets.find(',', start);
            const std::string token = presets.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            const float value = std::strtof(token.c_str(), nullptr);
            if (value > 0.0f) parsed.push_back(value);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!parsed.empty()) app.sizePresets = std::move(parsed);
    }

    app.foreground.r = static_cast<std::uint8_t>(settings.getInt("colour.r", 0));
    app.foreground.g = static_cast<std::uint8_t>(settings.getInt("colour.g", 0));
    app.foreground.b = static_cast<std::uint8_t>(settings.getInt("colour.b", 0));
    app.profile = readProfile(settings, "tablet");
    app.uiScale = std::clamp(settings.getFloat("ui.scale", 1.0f), 0.75f, 2.5f);
    app.lightTheme = settings.getInt("ui.lightTheme", 0) != 0;
    app.undoBudgetMb = std::clamp(settings.getInt("undo.budgetMb", 256), 16, 2048);
    // D-021: off unless the artist turned it on, on this machine, last time.
    app.useGpu = settings.getInt("render.gpu", 0) != 0;
    app.shortcuts.load(settings);

    // Ruler configuration is a preference; the vanishing points are not, and
    // come back with the document instead.
    app.symmetry.enabled    = settings.getInt("ruler.symmetry.on", 0) != 0;
    app.symmetry.vertical   = settings.getInt("ruler.symmetry.vertical", 1) != 0;
    app.symmetry.horizontal = settings.getInt("ruler.symmetry.horizontal", 0) != 0;
    app.symmetry.radial     = std::clamp(settings.getInt("ruler.symmetry.radial", 1),
                                         1, sbl::SymmetryRuler::kMaxRadial);
    app.perspective.enabled = settings.getInt("ruler.perspective.on", 0) != 0;

    // Custom presets are appended after the built-ins, which always come first
    // so a new Sable version can add one without renumbering the artist's.
    const int customCount = std::clamp(settings.getInt("brush.customCount", 0), 0, 64);
    for (int i = 0; i < customCount; ++i) {
        const std::string key = "brush.custom" + std::to_string(i) + ".";
        sbl::BrushPreset preset;
        preset.id   = settings.getString(key + "id", "");
        preset.name = settings.getString(key + "name", "");
        if (preset.id.empty() || preset.name.empty()) continue;
        preset.size            = settings.getFloat(key + "size", 10.0f);
        preset.minSizeRatio    = settings.getFloat(key + "minSize", 0.0f);
        preset.density         = settings.getFloat(key + "density", 1.0f);
        preset.minDensityRatio = settings.getFloat(key + "minDensity", 0.0f);
        preset.spacingFactor   = settings.getFloat(key + "spacing", 0.1f);
        preset.hardness        = settings.getFloat(key + "hardness", 1.0f);
        preset.blending        = settings.getFloat(key + "blending", 0.0f);
        preset.dilution        = settings.getFloat(key + "dilution", 0.0f);
        preset.persistence     = settings.getFloat(key + "persistence", 0.0f);
        preset.stabilizerLevel = static_cast<std::uint8_t>(
            std::clamp(settings.getInt(key + "stabilizer", 0), 0, 3));
        preset.pressure.toSize    = settings.getInt(key + "toSize", 1) != 0;
        preset.pressure.toDensity = settings.getInt(key + "toDensity", 0) != 0;
        app.brushes.push_back(std::move(preset));
    }
}

void collectSettings(const App& app, Settings& settings) {
    for (const sbl::BrushPreset& brush : app.brushes) {
        settings.setFloat("brush." + brush.id + ".size", brush.size);
        settings.setInt("brush." + brush.id + ".stabilizer", brush.stabilizerLevel);
    }
    settings.setFloat("eraser.size", app.eraser.size);
    settings.setInt("eraser.stabilizer", app.eraser.stabilizerLevel);
    settings.setInt("brush.active", static_cast<int>(app.brushIndex));
    settings.setInt("fill.tolerance", app.fillTolerance);
    settings.setInt("linework.stabilizer", app.linework.stabilizerLevel);

    std::string presets;
    for (std::size_t i = 0; i < app.sizePresets.size(); ++i) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%g", static_cast<double>(app.sizePresets[i]));
        presets += buffer;
        if (i + 1 < app.sizePresets.size()) presets += ",";
    }
    settings.set("brush.sizePresets", presets);

    settings.setInt("colour.r", app.foreground.r);
    settings.setInt("colour.g", app.foreground.g);
    settings.setInt("colour.b", app.foreground.b);
    storeProfile(settings, "tablet", app.profile);
    settings.setFloat("ui.scale", app.uiScale);
    settings.setInt("ui.lightTheme", app.lightTheme ? 1 : 0);
    settings.setInt("undo.budgetMb", app.undoBudgetMb);
    settings.setInt("render.gpu", app.useGpu ? 1 : 0);
    app.shortcuts.store(settings);

    settings.setInt("ruler.symmetry.on",         app.symmetry.enabled ? 1 : 0);
    settings.setInt("ruler.symmetry.vertical",   app.symmetry.vertical ? 1 : 0);
    settings.setInt("ruler.symmetry.horizontal", app.symmetry.horizontal ? 1 : 0);
    settings.setInt("ruler.symmetry.radial",     app.symmetry.radial);
    settings.setInt("ruler.perspective.on",      app.perspective.enabled ? 1 : 0);

    const std::size_t builtIn = sbl::defaultBrushes().size();
    int custom = 0;
    for (std::size_t i = builtIn; i < app.brushes.size(); ++i, ++custom) {
        const sbl::BrushPreset& preset = app.brushes[i];
        const std::string key = "brush.custom" + std::to_string(custom) + ".";
        settings.set(key + "id", preset.id);
        settings.set(key + "name", preset.name);
        settings.setFloat(key + "size", preset.size);
        settings.setFloat(key + "minSize", preset.minSizeRatio);
        settings.setFloat(key + "density", preset.density);
        settings.setFloat(key + "minDensity", preset.minDensityRatio);
        settings.setFloat(key + "spacing", preset.spacingFactor);
        settings.setFloat(key + "hardness", preset.hardness);
        settings.setFloat(key + "blending", preset.blending);
        settings.setFloat(key + "dilution", preset.dilution);
        settings.setFloat(key + "persistence", preset.persistence);
        settings.setInt(key + "stabilizer", preset.stabilizerLevel);
        settings.setInt(key + "toSize", preset.pressure.toSize ? 1 : 0);
        settings.setInt(key + "toDensity", preset.pressure.toDensity ? 1 : 0);
    }
    settings.setInt("brush.customCount", custom);
}

// --------------------------------------------------------------- document ops

/// The undo budget lives on the stack, and every fresh Document brings a fresh
/// stack — so it has to be reapplied rather than set once at startup.
void applyUndoBudget(App& app) {
    app.doc.undo.setMemoryBudget(static_cast<std::size_t>(app.undoBudgetMb) *
                                 1024u * 1024u);
}

void showError(App& app, const sbl::Error& error);

/// Installs or removes the GPU backend, and tells the truth about which one is
/// running afterwards.
///
/// D-021 wants a runtime switch so that "the colours are wrong" is one toggle
/// away from saying which side is wrong. The device is built on first use and
/// then kept: creating one costs tens of milliseconds, and a switch the artist
/// flicks twice should not cost that twice.
///
/// Never mid-stroke: the backend holds a batch of dabs for the layer it is
/// painting, and swapping it out from under that would drop them.
void applyGpuMode(App& app) {
    if (app.painting) return;

    // D-023, #21: the GPU backend is 8-bit RGBA end to end and declines a
    // 16-bit document tile by tile. It would still be CORRECT — every declined
    // operation lands on the CPU — but the artist would be looking at a ticked
    // menu item, a status bar reading "GPU", and CPU performance. Say it
    // instead, once, and untick the box.
    if (app.useGpu && app.doc.depth != sbl::ColourDepth::Bits8) {
        app.useGpu = false;
        app.notices = {"Staying on the CPU: this document is 16-bit, and the "
                       "GPU backend paints 8-bit only."};
    }

    if (app.useGpu && app.gpu == nullptr) {
        app.gpu = sbl::makeGpuBackend(&app.gpuWhy);
        if (app.gpu == nullptr) {
            app.useGpu = false;                       // no device: stay on the CPU
            // #15: the fallback has to be visible. An artist who ticked the box
            // and is still on the CPU would otherwise blame the machine for
            // performance they were never getting.
            app.notices = {"Staying on the CPU: " + app.gpuWhy};
        }
        SDL_Log("paint backend: %s", app.gpuWhy.c_str());
    }
    if (!app.useGpu && app.gpu != nullptr) {
        // The host tiles have to be the truth again before the backend that
        // holds them stops being consulted.
        if (const auto ready = app.gpu->readback(app.doc); !ready.has_value())
            showError(app, ready.error());
    }
    sbl::setPaintBackend(app.useGpu ? app.gpu.get() : nullptr);
    if (app.canvas != nullptr) app.canvas->markAllDirty();
}

void resetDocument(App& app, std::int32_t w, std::int32_t h, bool transparent,
                   sbl::ColourDepth depth = sbl::ColourDepth::Bits8) {
    // Nothing may outlive the document it was editing — an import warning
    // included, since it describes a document that no longer exists.
    app.text.finish(app.window, app.doc);
    app.notices.clear();
    app.canvas->releaseAll();
    app.doc = sbl::makeDocument(
        w, h, transparent ? sbl::StraightRgba8{0, 0, 0, 0}
                          : sbl::StraightRgba8{255, 255, 255, 255},
        depth);
    // The GPU cannot paint 16-bit tiles yet, so a new 16-bit document has to
    // put the toggle back where the artist can see it rather than leave the
    // View menu ticked over a backend that is silently declining every dab.
    applyGpuMode(app);
    applyUndoBudget(app);
    centreSymmetry(app);
    fitToViewport(app.view, app.doc, app.viewport);
    if (app.view.zoom > 1.0f) zoomToActualSize(app.view, app.doc, app.viewport);
}

/// Applies the tile changes an undo or redo reported. A tile that no longer
/// exists has its texture released outright (US-04.8).
void syncTextures(App& app,
                  const std::vector<std::pair<sbl::LayerId, sbl::TileKey>>& changed) {
    for (const auto& [layerId, key] : changed) {
        const sbl::Layer* layer = app.doc.layerById(layerId);
        if (layer != nullptr && layer->find(key) != nullptr) app.canvas->markDirty(key);
        else                                                 app.canvas->release(key);
    }
}

void doUndo(App& app) {
    if (!app.doc.undo.canUndo()) return;
    syncTextures(app, app.doc.undo.undo(app.doc));
    app.doc.dirty = true;
}

void doRedo(App& app) {
    if (!app.doc.undo.canRedo()) return;
    syncTextures(app, app.doc.undo.redo(app.doc));
    app.doc.dirty = true;
}

void doClear(App& app) {
    sbl::Layer* layer = app.doc.active();
    if (layer == nullptr || layer->tiles.empty()) return;   // harmless when empty
    sbl::UndoRecord rec = sbl::clearLayer(*layer);
    for (const auto& snap : rec.tiles) app.canvas->release(snap.key);
    app.doc.undo.push(std::move(rec));
    app.doc.dirty = true;
}

void showError(App& app, const sbl::Error& error) {
    // Never terminate on a file path (D-012). Show it and carry on.
    app.errorMessage = error.detail;
    app.openError = true;
}

/// Marks every tile of every layer for re-upload. Used after anything that
/// replaces the document wholesale.
void refreshAllTextures(App& app) {
    app.canvas->releaseAll();
}

void doSaveProject(App& app, const std::filesystem::path& path) {
    // Through the registry, so Save and Export share one dispatch.
    if (const auto result = sbl::exportDocument(app.doc, path); !result.has_value()) {
        showError(app, result.error());
        return;
    }
    app.doc.path  = path;
    app.doc.dirty = false;

    // The artist has a real file now, so the recovery copy is no longer the
    // only thing standing between them and losing work.
    const std::lock_guard lock(app.recoveryMutex);
    if (!app.lastRecoveryFile.empty()) {
        sbl::clearRecovery(app.lastRecoveryFile);
        app.lastRecoveryFile.clear();
    }
}

/// Open and Import are the same code path: the registry picks the reader, and
/// it is what decides whether the document keeps a path to save back to.
void doOpenDocument(App& app, const std::filesystem::path& path) {
    app.text.finish(app.window, app.doc);
    auto loaded = sbl::importDocument(path);
    if (!loaded.has_value()) {
        showError(app, loaded.error());
        return;
    }
    refreshAllTextures(app);
    app.doc = std::move(*loaded);
    // #40: once, here, for every format — the registry is the one front door,
    // so no importer can be added that forgets to be listened to. Assigned
    // rather than appended, so opening a clean file clears the last file's.
    app.notices = app.doc.warnings;
    for (const std::string& warning : app.notices)
        SDL_Log("%s: %s", path.filename().string().c_str(), warning.c_str());
    applyGpuMode(app);         // a 16-bit file arriving turns the GPU back off
    applyUndoBudget(app);
    centreSymmetry(app);
    fitToViewport(app.view, app.doc, app.viewport);
}

/// Writes a recovery copy on a worker thread.
///
/// The worker owns a full clone and touches nothing reachable from the live
/// Document — no pointers, no references, not even "just reading" the layer
/// vector while the main thread might reallocate it. In C++ nothing in the
/// type system enforces that, so the discipline is the hand-off itself.
void maybeAutosave(App& app, std::uint64_t nowMs) {
    if (!app.doc.dirty || app.autosaveBusy.load()) return;
    if (nowMs - app.lastAutosaveMs < kAutosaveIntervalMs) return;
    app.lastAutosaveMs = nowMs;

    if (app.autosaveWorker.joinable()) app.autosaveWorker.join();

    // The worker gets host pixels or it gets nothing: it has no device
    // context, so a backend holding tiles elsewhere must put them back first.
    // Skipping one autosave beats writing a file of stale art.
    if (const auto ready = sbl::paintBackend().readback(app.doc); !ready.has_value()) {
        showError(app, ready.error());
        return;
    }
    sbl::Document snapshot = sbl::cloneDocument(app.doc);   // main thread
    const std::filesystem::path original = app.doc.path;
    app.autosaveBusy.store(true);

    app.autosaveWorker = std::thread(
        [&app, snapshot = std::move(snapshot), original]() mutable {
            const auto written = sbl::writeRecovery(snapshot, original);
            {
                const std::lock_guard lock(app.recoveryMutex);
                if (written.has_value()) {
                    // Replace the previous copy rather than accumulating one
                    // every two minutes for the whole session.
                    if (!app.lastRecoveryFile.empty() &&
                        app.lastRecoveryFile != written->string())
                        sbl::clearRecovery(app.lastRecoveryFile);
                    app.lastRecoveryFile = written->string();
                }
            }
            app.autosaveBusy.store(false);
            // Report back through the event loop, not by mutating state here.
            SDL_Event wake{};
            wake.type = SDL_EVENT_USER;
            SDL_PushEvent(&wake);
        });
}

void doExport(App& app, const std::string& path) {
    if (const auto result = sbl::exportDocument(app.doc, path); !result.has_value())
        showError(app, result.error());
    // US-07.5: exporting changes nothing about the document, dirty flag included.
}

// ------------------------------------------------------------------- painting

void syncColourFromHsv(App& app) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(app.foregroundHsv[0], app.foregroundHsv[1],
                                app.foregroundHsv[2], r, g, b);
    app.foreground.r = static_cast<std::uint8_t>(std::lround(r * 255.0f));
    app.foreground.g = static_cast<std::uint8_t>(std::lround(g * 255.0f));
    app.foreground.b = static_cast<std::uint8_t>(std::lround(b * 255.0f));
    app.foreground.a = 255;
}

void syncHsvFromColour(App& app) {
    ImGui::ColorConvertRGBtoHSV(app.foreground.r / 255.0f, app.foreground.g / 255.0f,
                                app.foreground.b / 255.0f, app.foregroundHsv[0],
                                app.foregroundHsv[1], app.foregroundHsv[2]);
}

void beginPaint(App& app) {
    sbl::Layer* layer = app.doc.active();
    if (layer == nullptr) return;

    // Size changes apply to the next stroke, never mid-stroke (US-12.5) —
    // which beginStroke already guarantees by copying the preset.
    sbl::beginStroke(app.stroke, activeBrush(app), app.foreground, layer->id);
    app.stabilizer.setLevel(activeBrush(app).stabilizerLevel);
    app.stabilizer.reset();
    // A snapshot for the stroke, so dragging a guide mid-stroke cannot bend a
    // line that has already committed to it. The document keeps the originals.
    app.perspective.points = app.doc.vanishingPoints;
    app.perspective.reset();
    app.pressureFilter.reset();
    app.painting = true;
    SDL_CaptureMouse(true);      // so a release outside the window still arrives
}

/// Stamps every mirrored image of the dabs just produced, and marks the tiles
/// of originals and images alike.
///
/// The images go into the SAME PaintTarget the stroke is already using, so
/// they land in the one pending UndoRecord: symmetry multiplies dabs, never
/// undo steps. Live, per sample — an artist has to see the mirror while they
/// draw, not discover it at pen-up.
void applyRulerImages(App& app, sbl::PaintTarget& target) {
    const bool mirroring = app.symmetry.active();
    for (const sbl::Dab& dab : app.scratch) {
        app.canvas->markDabArea(dab.x, dab.y, dab.radius, app.doc);
        if (!mirroring) continue;

        app.symmetry.map(dab.x, dab.y, dab.angle, app.mirrors);
        // Index 0 is the original, which paintSample has already applied.
        for (std::size_t i = 1; i < app.mirrors.size(); ++i) {
            sbl::Dab image = dab;
            image.x     = app.mirrors[i].x;
            image.y     = app.mirrors[i].y;
            image.angle = app.mirrors[i].angle;
            sbl::applyDab(target, image);
            app.canvas->markDabArea(image.x, image.y, image.radius, app.doc);
        }
    }
}

void paintWith(App& app, sbl::InputSample sample) {
    sbl::Layer* layer = app.doc.active();
    if (layer == nullptr) return;

    app.lastCanvasX = sample.x;
    app.lastCanvasY = sample.y;

    // Positions only — the stabilizer never touches pressure (US-11.6).
    sample = app.stabilizer.apply(sample);
    // The ruler sees the SMOOTHED point. The other order smooths a point that
    // was already on the guide and drags it back off, so the two would fight.
    sample = app.perspective.apply(sample);

    const sbl::Selection* selection =
        app.doc.selection.has_value() && !app.doc.selection->empty()
            ? &*app.doc.selection : nullptr;
    sbl::PaintTarget target{*layer, app.stroke.pending, app.stroke.touched,
                            app.doc.width, app.doc.height, selection};
    sbl::paintSample(app.stroke, target, sample, app.scratch);
    applyRulerImages(app, target);
}

sbl::InputSample mouseSample(App& app, double sx, double sy) {
    sbl::InputSample sample;
    sample.x = toCanvasX(app.view, sx, sy);
    sample.y = toCanvasY(app.view, sx, sy);
    sample.pressure    = 1.0f;          // mouse: full pressure, synthetic
    sample.fromMouse   = true;
    sample.timestampMs = SDL_GetTicks();

    app.lastFromMouse    = true;
    app.lastRawPressure  = 1.0f;
    app.lastNormPressure = 1.0f;
    return sample;
}

sbl::InputSample penSample(App& app, SDL_PenID which, float sx, float sy) {
    const PenAxisState& axes = app.penAxes[which];

    sbl::InputSample sample;
    sample.x = toCanvasX(app.view, sx, sy);
    sample.y = toCanvasY(app.view, sx, sy);
    // The fixed normalisation order lives in the engine, not here: deadzone,
    // rescale, smoothing, then the artist's curve (US-09.7).
    sample.pressure = app.pressureFilter.apply(app.profile, axes.pressure);
    sample.tiltX    = axes.tiltX * kDegToRad;
    sample.tiltY    = axes.tiltY * kDegToRad;
    sample.rotation = axes.rotation * kDegToRad;
    sample.fromMouse   = false;
    sample.timestampMs = SDL_GetTicks();

    app.lastFromMouse    = false;
    app.lastRawPressure  = axes.pressure;
    app.lastNormPressure = sample.pressure;
    app.lastTiltX = axes.tiltX;
    app.lastTiltY = axes.tiltY;
    return sample;
}

void endPaint(App& app) {
    if (!app.painting) return;
    app.painting = false;
    SDL_CaptureMouse(false);

    // Release the string so the stroke ends where the artist lifted, not short
    // of it (US-11.4).
    if (!app.stroke.samples.empty() && activeBrush(app).stabilizerLevel > 0) {
        sbl::Layer* layer = app.doc.active();
        if (layer != nullptr) {
            const sbl::InputSample last = app.perspective.apply(
                app.stabilizer.finish(app.stroke.samples.back()));
            sbl::PaintTarget target{*layer, app.stroke.pending, app.stroke.touched,
                                    app.doc.width, app.doc.height};
            sbl::paintSample(app.stroke, target, last, app.scratch);
            applyRulerImages(app, target);
        }
    }

    if (!app.stroke.pending.empty()) {
        app.doc.undo.push(std::move(app.stroke.pending));
        app.doc.dirty = true;
    }
    app.stroke.samples.clear();

    // The stroke, not the dab, is where a backend failure is worth reporting:
    // the paint path records it and this is the one place that asks. Silence
    // here is what "noexcept and silently wrong" used to look like.
    if (const auto failed = sbl::paintBackend().takeError(); failed.has_value())
        showError(app, *failed);
}

void doFill(App& app, double sx, double sy) {
    const auto x = static_cast<std::int32_t>(std::floor(toCanvasX(app.view, sx, sy)));
    const auto y = static_cast<std::int32_t>(std::floor(toCanvasY(app.view, sx, sy)));

    sbl::UndoRecord rec =
        sbl::bucketFill(app.doc, app.doc.activeLayer, x, y, app.foreground,
                        app.fillTolerance);
    if (rec.empty()) return;

    for (const auto& snap : rec.tiles) app.canvas->markDirty(snap.key);
    app.doc.undo.push(std::move(rec));
    app.doc.dirty = true;
}

void beginGradient(App& app, double sx, double sy) {
    app.gradientDragging  = true;
    app.gradientPreviewed = false;
    app.gradientAnchorX   = toCanvasX(app.view, sx, sy);
    app.gradientAnchorY   = toCanvasY(app.view, sx, sy);
}

/// Draws the gradient the drag currently describes, replacing the one the
/// previous motion event drew.
///
/// The preview goes THROUGH the undo stack rather than beside it: taking the
/// last preview back restores exactly the pixels the next one has to be
/// computed from, and pushing clears the redo entry that just made. So what the
/// artist is looking at is always a real, finished edit — there is nothing to
/// commit on release, and nothing to lose if the app dies mid-drag. The
/// alternative, a second copy of the layer held beside the document, is a
/// second thing that has to be kept in step with undo, autosave and the
/// texture cache.
///
/// ponytail: every motion event re-draws the whole selection and re-uploads
/// every tile it touched. A 1024 x 1024 canvas is a millisecond or two, which
/// is what the self-test drives; if a very large canvas ever drags heavily,
/// throttle this to one preview per frame before reaching for anything cleverer.
void previewGradient(App& app, double sx, double sy) {
    if (!app.gradientDragging) return;
    if (app.gradientPreviewed) {
        syncTextures(app, app.doc.undo.undo(app.doc));
        app.gradientPreviewed = false;
    }

    sbl::Gradient g;
    g.shape  = app.gradientShape;
    g.x0     = app.gradientAnchorX;
    g.y0     = app.gradientAnchorY;
    g.x1     = toCanvasX(app.view, sx, sy);
    g.y1     = toCanvasY(app.view, sx, sy);
    g.from   = app.foreground;
    g.to     = app.gradientToTransparent
                   ? sbl::StraightRgba8{0, 0, 0, 0} : app.background;
    g.dither = app.gradientDither;

    sbl::UndoRecord rec = sbl::gradientFill(app.doc, app.doc.activeLayer, g);
    if (rec.empty()) return;                  // a drag too short to have an axis

    for (const auto& snap : rec.tiles) app.canvas->markDirty(snap.key);
    app.doc.undo.push(std::move(rec));
    app.gradientPreviewed = true;
    app.doc.dirty = true;

    if (const auto failed = sbl::paintBackend().takeError(); failed.has_value())
        showError(app, *failed);
}

void endGradient(App& app, double sx, double sy) {
    if (!app.gradientDragging) return;
    previewGradient(app, sx, sy);
    app.gradientDragging  = false;
    app.gradientPreviewed = false;
}

/// Shift adds, Alt subtracts, both intersect — the modifiers every other
/// painting application uses, over whatever the panel is set to. Read once, at
/// pen-down, so a drag means one thing from start to finish.
[[nodiscard]] sbl::SelectMode modeForDrag(const App& app) {
    const SDL_Keymod mods = SDL_GetModState();
    const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
    const bool alt   = (mods & SDL_KMOD_ALT) != 0;
    if (shift && alt) return sbl::SelectMode::Intersect;
    if (shift)        return sbl::SelectMode::Add;
    if (alt)          return sbl::SelectMode::Subtract;
    return app.selectMode;
}

/// Puts a freshly drawn shape into the document, combined with what the drag
/// started from. One place, so the marquee, the lasso and the wand cannot
/// disagree about what Shift means.
void commitSelection(App& app, const sbl::Selection& shape) {
    const sbl::Selection base =
        app.selectBase.has_value() ? *app.selectBase : sbl::Selection{};
    sbl::Selection combined = sbl::combineSelections(base, shape, app.dragMode);
    if (combined.empty()) app.doc.selection.reset();
    else                  app.doc.selection = std::move(combined);
}

void beginSelectDrag(App& app, double sx, double sy) {
    app.selecting     = true;
    app.dragMode      = modeForDrag(app);
    app.selectBase    = app.doc.selection;
    app.selectAnchorX = toCanvasX(app.view, sx, sy);
    app.selectAnchorY = toCanvasY(app.view, sx, sy);
    app.lassoPath.clear();
    app.lassoPath.push_back(sbl::Point{app.selectAnchorX, app.selectAnchorY});
}

void updateSelection(App& app, double sx, double sy) {
    const double cx = toCanvasX(app.view, sx, sy);
    const double cy = toCanvasY(app.view, sx, sy);

    sbl::Selection selection;
    selection.x = static_cast<std::int32_t>(std::floor(std::min(app.selectAnchorX, cx)));
    selection.y = static_cast<std::int32_t>(std::floor(std::min(app.selectAnchorY, cy)));
    selection.w = static_cast<std::int32_t>(std::abs(cx - app.selectAnchorX));
    selection.h = static_cast<std::int32_t>(std::abs(cy - app.selectAnchorY));

    // Clamped to the canvas, so a drag off the edge does not make a selection
    // that reaches somewhere no pixel exists.
    const std::int32_t x1 = std::min(app.doc.width,  selection.x + selection.w);
    const std::int32_t y1 = std::min(app.doc.height, selection.y + selection.h);
    selection.x = std::max(0, selection.x);
    selection.y = std::max(0, selection.y);
    selection.w = std::max(0, x1 - selection.x);
    selection.h = std::max(0, y1 - selection.y);
    commitSelection(app, selection);
}

/// Appends to the freehand path. Sub-pixel steps are dropped: the pointer
/// reports far more motion than a polygon edge needs, and the lasso's cost is
/// per vertex on every scanline.
void extendLasso(App& app, double sx, double sy) {
    const sbl::Point p{toCanvasX(app.view, sx, sy), toCanvasY(app.view, sx, sy)};
    if (!app.lassoPath.empty()) {
        const sbl::Point& last = app.lassoPath.back();
        if (std::abs(p.x - last.x) < 1.0 && std::abs(p.y - last.y) < 1.0) return;
    }
    app.lassoPath.push_back(p);
}

void endSelectDrag(App& app, double sx, double sy) {
    if (!app.selecting) return;
    app.selecting = false;
    // The lasso is rasterised once, at pen-up: doing it live would re-scan the
    // whole loop on every motion event, for a shape that is not finished yet.
    if (app.tool == Tool::Lasso) {
        extendLasso(app, sx, sy);
        commitSelection(app, sbl::lassoSelection(app.lassoPath, app.doc.width,
                                                 app.doc.height));
        app.lassoPath.clear();
    }
    app.selectBase.reset();
    // A click with no drag clears the selection rather than leaving a
    // zero-sized one that silently blocks painting.
    if (app.doc.selection.has_value() && app.doc.selection->empty())
        app.doc.selection.reset();
}

/// The pointer moved while a selection is being drawn. Pen and mouse both.
void dragSelection(App& app, double sx, double sy) {
    if (app.tool == Tool::Lasso) extendLasso(app, sx, sy);
    else                        updateSelection(app, sx, sy);
}

void wandAt(App& app, double sx, double sy) {
    const auto x = static_cast<std::int32_t>(std::floor(toCanvasX(app.view, sx, sy)));
    const auto y = static_cast<std::int32_t>(std::floor(toCanvasY(app.view, sx, sy)));

    app.dragMode   = modeForDrag(app);
    app.selectBase = app.doc.selection;
    commitSelection(app, sbl::magicWandSelection(app.doc, x, y, app.wandTolerance));
    app.selectBase.reset();
}

void pickColourAt(App& app, double sx, double sy) {
    const auto x = static_cast<std::int32_t>(std::floor(toCanvasX(app.view, sx, sy)));
    const auto y = static_cast<std::int32_t>(std::floor(toCanvasY(app.view, sx, sy)));
    if (x < 0 || y < 0 || x >= app.doc.width || y >= app.doc.height) return;

    const sbl::StraightRgba8 picked = sbl::pickColour(app.doc, x, y);
    app.foreground = sbl::StraightRgba8{picked.r, picked.g, picked.b, 255};
    syncHsvFromColour(app);
}

/// Which guide handle is under a screen point, or kNoGuide.
///
/// Hit-tested in SCREEN space through toScreenX/Y rather than by comparing
/// canvas distances: the grab area has to stay the same size under the pointer
/// at any zoom, and it has to be in the right place once the canvas is turned.
[[nodiscard]] int guideAt(const App& app, double sx, double sy) {
    const double reach = 10.0 * app.uiScale;
    const auto grabbed = [&](double cx, double cy) {
        const double dx = toScreenX(app.view, cx, cy) - sx;
        const double dy = toScreenY(app.view, cx, cy) - sy;
        return dx * dx + dy * dy <= reach * reach;
    };
    // Vanishing points first: they are the smaller, fiddlier target.
    if (app.perspective.enabled) {
        for (std::size_t i = 0; i < app.doc.vanishingPoints.size(); ++i)
            if (grabbed(app.doc.vanishingPoints[i].x, app.doc.vanishingPoints[i].y))
                return static_cast<int>(i);
    }
    if (app.symmetry.enabled && grabbed(app.symmetry.centreX, app.symmetry.centreY))
        return kSymmetryGuide;
    return kNoGuide;
}

void moveGuide(App& app, double sx, double sy) {
    const double cx = toCanvasX(app.view, sx, sy);
    const double cy = toCanvasY(app.view, sx, sy);
    if (app.draggingGuide == kSymmetryGuide) {
        app.symmetry.centreX = cx;
        app.symmetry.centreY = cy;      // not document state, so nothing dirties
    } else if (app.draggingGuide >= 0 &&
               static_cast<std::size_t>(app.draggingGuide) <
                   app.doc.vanishingPoints.size()) {
        app.doc.vanishingPoints[app.draggingGuide].x = cx;
        app.doc.vanishingPoints[app.draggingGuide].y = cy;
        app.doc.dirty = true;           // it is saved, so moving it is a change
    }
}

/// Puts a fresh text where the artist pointed, ending whatever was being typed.
///
/// A click always starts a NEW text. Reopening an existing one is selecting its
/// layer and pressing the tool's key — a rule with no hit-testing in it, and so
/// with no invisible boxes for a click to land just outside of.
void placeText(App& app, double sx, double sy) {
    app.text.finish(app.window, app.doc);
    app.text.begin(app.window, toCanvasX(app.view, sx, sy),
                   toCanvasY(app.view, sx, sy), app.foreground);
}


// ------------------------------------------------------------------ linework
// #17. Everything about where a curve goes and which point is under the cursor
// belongs to the engine; what is left here is what the modifier keys mean and
// where the pointer is in canvas coordinates.

/// How close counts as "on" a control point, in CANVAS pixels.
///
/// Divided by the zoom so the target stays the same size under the artist's
/// hand however far in they are — a fixed canvas distance would be untouchable
/// at 10% and would swallow the whole line at 800%.
[[nodiscard]] double lineworkGrab(const App& app) {
    return 9.0 * static_cast<double>(app.uiScale) /
           std::max(static_cast<double>(app.view.zoom), 0.01);
}

/// Ctrl adds a control point on the curve, Shift takes one away, and a plain
/// press draws or drags. Alt is left alone: it is the colour picker everywhere
/// else, and a tool that redefines it is a tool that surprises people.
///
/// In whole-stroke mode all three are one gesture on the line itself, with
/// Shift adding to the selection — the same key that extends a selection
/// everywhere else in the program.
[[nodiscard]] LineworkAction lineworkAction(const App& app) {
    const SDL_Keymod mods = SDL_GetModState();
    if (app.linework.selectMode)
        return (mods & SDL_KMOD_SHIFT) != 0 ? LineworkAction::SelectAdd
                                            : LineworkAction::Select;
    if ((mods & SDL_KMOD_CTRL)  != 0) return LineworkAction::Insert;
    if ((mods & SDL_KMOD_SHIFT) != 0) return LineworkAction::Erase;
    return LineworkAction::Draw;
}

void lineworkPress(App& app, double sx, double sy, float pressure) {
    app.linework.colour = app.foreground;
    app.linework.press(app.doc, toCanvasX(app.view, sx, sy),
                       toCanvasY(app.view, sx, sy), pressure, lineworkAction(app),
                       lineworkGrab(app));
    syncTextures(app, app.linework.takeChanged());
}

void lineworkDrag(App& app, double sx, double sy, float pressure) {
    if (!app.linework.busy()) return;
    app.linework.drag(app.doc, toCanvasX(app.view, sx, sy),
                      toCanvasY(app.view, sx, sy), pressure);
    syncTextures(app, app.linework.takeChanged());
}

void lineworkRelease(App& app) {
    app.linework.release(app.doc);
    syncTextures(app, app.linework.takeChanged());
}

/// Selects the text tool, resuming the active layer's text if it has any.
void chooseTextTool(App& app) {
    app.tool = Tool::Text;
    const sbl::Layer* layer = app.doc.layerById(app.doc.activeLayer);
    if (layer != nullptr && layer->text.has_value())
        app.text.resume(app.window, app.doc, layer->id);
}

void stepSizePreset(App& app, int direction) {
    if (app.sizePresets.empty()) return;
    sbl::BrushPreset& brush = activeBrush(app);

    // Land on the neighbouring preset, whichever one the current free-form
    // size sits between — so [ and ] work after dragging the slider too.
    std::size_t nearest = 0;
    float best = std::abs(app.sizePresets[0] - brush.size);
    for (std::size_t i = 1; i < app.sizePresets.size(); ++i) {
        const float d = std::abs(app.sizePresets[i] - brush.size);
        if (d < best) { best = d; nearest = i; }
    }
    const auto next = static_cast<int>(nearest) + direction;
    brush.size = app.sizePresets[static_cast<std::size_t>(
        std::clamp(next, 0, static_cast<int>(app.sizePresets.size()) - 1))];
}

// -------------------------------------------------------------------- dialogs

void SDLCALL onSaveChosen(void* userdata, const char* const* filelist, int filter) {
    App* app = static_cast<App*>(userdata);
    if (filelist == nullptr || filelist[0] == nullptr) return;   // error or cancelled

    {
        const std::lock_guard lock(app->dialogMutex);
        app->dialogPath   = filelist[0];
        app->chosenFilter = filter;
        app->dialogReady  = true;
    }
    // The loop is blocked in SDL_WaitEvent. Wake it.
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
}

/// Opens the native dialog with the filters the registry supplies, so a new
/// importer or exporter appears here without a line changing.
///
/// D-002: SDL3's native file dialog, never an ImGui file browser.
void showFileDialog(App& app, sbl::FormatUse use, bool native, bool saving) {
    app.filterList = sbl::dialogFilters(use, native);
    app.filterViews.clear();
    for (const sbl::DialogFilter& filter : app.filterList)
        app.filterViews.push_back(
            SDL_DialogFileFilter{filter.label.c_str(), filter.pattern.c_str()});
    app.chosenFilter = -1;

    const int count = static_cast<int>(app.filterViews.size());
    if (saving)
        SDL_ShowSaveFileDialog(onSaveChosen, &app, app.window,
                               app.filterViews.data(), count, nullptr);
    else
        SDL_ShowOpenFileDialog(onSaveChosen, &app, app.window,
                               app.filterViews.data(), count, nullptr, false);
}

void askForExportPath(App& app) {
    app.fileAction = App::FileAction::ExportImage;
    showFileDialog(app, sbl::FormatUse::Write, false, true);
}

void askForSavePath(App& app) {
    app.fileAction = App::FileAction::SaveProject;
    showFileDialog(app, sbl::FormatUse::Write, true, true);
}

void askForOpenPath(App& app) {
    app.fileAction = App::FileAction::OpenProject;
    showFileDialog(app, sbl::FormatUse::Read, true, false);
}

void askForImportPath(App& app) {
    app.fileAction = App::FileAction::ImportDocument;
    showFileDialog(app, sbl::FormatUse::Read, false, false);
}

/// Carries out a Pending::Open once nothing stands in its way. A path that
/// arrived from outside the application — a drop, a command-line argument —
/// opens straight away; without one the artist picks the file.
void openPending(App& app) {
    if (app.pendingOpenPath.empty()) { askForOpenPath(app); return; }
    const std::filesystem::path path = std::move(app.pendingOpenPath);
    app.pendingOpenPath.clear();
    doOpenDocument(app, path);
}

/// Ctrl+S: straight to the known path, or the dialog if there is not one yet.
void doSave(App& app) {
    if (app.doc.path.empty()) askForSavePath(app);
    else                      doSaveProject(app, app.doc.path);
}

/// The extension of the filter the artist picked, for when they typed a bare
/// name. A platform that does not report the choice gives -1, and the first
/// filter is the sensible default.
std::string chosenExtension(const App& app) {
    if (app.filterList.empty()) return {};
    const std::size_t index =
        app.chosenFilter >= 0 && static_cast<std::size_t>(app.chosenFilter) < app.filterList.size()
            ? static_cast<std::size_t>(app.chosenFilter) : 0;
    const std::string& pattern = app.filterList[index].pattern;
    return "." + pattern.substr(0, pattern.find(';'));   // "jpg;jpeg" -> ".jpg"
}

void pumpDialog(App& app) {
    std::string path;
    {
        const std::lock_guard lock(app.dialogMutex);
        if (!app.dialogReady) return;
        app.dialogReady = false;
        path = std::move(app.dialogPath);
        app.dialogPath.clear();
    }
    if (path.empty()) return;

    if (app.fileAction == App::FileAction::OpenProject ||
        app.fileAction == App::FileAction::ImportDocument) {
        doOpenDocument(app, path);
        return;
    }

    const std::string extension = chosenExtension(app);
    if (!extension.empty() && !path.ends_with(extension)) path += extension;

    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {   // US-07.6
        app.overwritePath = std::move(path);
        app.openOverwrite = true;
    } else if (app.fileAction == App::FileAction::SaveProject) {
        doSaveProject(app, path);
    } else {
        doExport(app, path);
    }
}

/// Requests an action that would lose unsaved work, prompting first (US-01.3).
///
/// `path` belongs to Pending::Open and names a file chosen outside the
/// application. Defaulting it also clears any stale one, so a drop the artist
/// cancelled cannot be opened later by an unrelated Ctrl+O.
void requestPending(App& app, Pending what, std::string path = {}) {
    app.pending = what;
    app.pendingOpenPath = std::move(path);
    if (app.doc.dirty) { app.openDiscard = true; return; }
    if (what == Pending::NewCanvas)   app.openNew = true;
    else if (what == Pending::Open)   openPending(app);
    else if (what == Pending::Import) askForImportPath(app);
    else                              app.running = false;
    app.pending = Pending::None;
}

// ----------------------------------------------------------------------- input

bool overCanvas(const App& app, float x, float y) {
    return x >= app.viewport.x && y >= app.viewport.y &&
           x < app.viewport.x + app.viewport.w && y < app.viewport.y + app.viewport.h;
}

/// Rotation changes the mapping only — no dirty flag, no undo entry, no tile
/// upload, the same guarantee US-05.5 makes for pan and zoom.
void rotateView(App& app, double delta) {
    rotateAbout(app.view, app.viewport.x + app.viewport.w * 0.5,
                app.viewport.y + app.viewport.h * 0.5, delta);
}

void handleKey(App& app, const SDL_KeyboardEvent& key) {
    if (key.key == SDLK_SPACE) { app.spaceHeld = key.down; return; }
    if (!key.down) return;

    // Keyboard-only placement (PRD §6): with the text tool chosen and nothing
    // being typed yet, Enter starts a text in the middle of what is on screen.
    // Without this the tool would need a pointing device to begin at all.
    if (app.tool == Tool::Text && !app.text.active() &&
        (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER)) {
        placeText(app, app.viewport.x + app.viewport.w * 0.5,
                       app.viewport.y + app.viewport.h * 0.5);
        return;
    }

    // Capturing a new binding takes priority over triggering the old one.
    if (app.rebinding != Action::Count) {
        if (key.key == SDLK_ESCAPE) { app.rebinding = Action::Count; return; }
        // A bare modifier is not a binding; wait for the real key.
        if (key.key == SDLK_LCTRL || key.key == SDLK_RCTRL ||
            key.key == SDLK_LSHIFT || key.key == SDLK_RSHIFT ||
            key.key == SDLK_LALT || key.key == SDLK_RALT) return;
        app.shortcuts.set(app.rebinding, Binding{key.key, static_cast<SDL_Keymod>(key.mod)});
        app.rebinding = Action::Count;
        return;
    }

    switch (app.shortcuts.lookup(key.key, static_cast<SDL_Keymod>(key.mod))) {
        case Action::NewCanvas:   requestPending(app, Pending::NewCanvas); break;
        case Action::OpenProject: requestPending(app, Pending::Open); break;
        case Action::Save:        doSave(app); break;
        case Action::SaveAs:      askForSavePath(app); break;
        case Action::ExportPng:   askForExportPath(app); break;
        case Action::Quit:        requestPending(app, Pending::Quit); break;

        case Action::Undo:  doUndo(app); break;
        case Action::Redo:  doRedo(app); break;
        case Action::Clear: doClear(app); break;
        case Action::FillSelection: {
            sbl::UndoRecord rec =
                sbl::fillSelection(app.doc, app.doc.activeLayer, app.foreground);
            if (!rec.empty()) {
                for (const auto& snap : rec.tiles) app.canvas->markDirty(snap.key);
                app.doc.undo.push(std::move(rec));
                app.doc.dirty = true;
            }
            break;
        }
        case Action::Deselect: app.doc.selection.reset(); break;

        case Action::FitToWindow: fitToViewport(app.view, app.doc, app.viewport); break;
        case Action::ActualSize:  zoomToActualSize(app.view, app.doc, app.viewport); break;
        case Action::ZoomIn:
        case Action::ZoomOut: {
            const float factor =
                app.shortcuts.lookup(key.key, static_cast<SDL_Keymod>(key.mod)) ==
                    Action::ZoomIn ? kZoomStep : 1.0f / kZoomStep;
            zoomAbout(app.view, app.viewport.x + app.viewport.w * 0.5f,
                      app.viewport.y + app.viewport.h * 0.5f, factor);
            break;
        }
        // Turning about the middle of the viewport, not the canvas origin:
        // whatever the artist is looking at stays where they are looking.
        case Action::RotateLeft:  rotateView(app, -kRotateStep); break;
        case Action::RotateRight: rotateView(app, +kRotateStep); break;
        case Action::ResetRotation: rotateView(app, -app.view.rotation); break;

        // Leaving the text tool commits what was typed. A tool change is an
        // unambiguous "I have finished with this", and the alternative — a
        // session left running under the brush — loses the text on the first
        // stroke that lands on its layer. Every tool that is not Text has to
        // say so, so a tool added later cannot quietly skip it.
        case Action::ToolBrush:  app.text.finish(app.window, app.doc); app.tool = Tool::Brush;  break;
        case Action::ToolLasso:  app.text.finish(app.window, app.doc); app.tool = Tool::Lasso;  break;
        case Action::ToolWand:   app.text.finish(app.window, app.doc); app.tool = Tool::Wand;   break;
        case Action::ToolEraser: app.text.finish(app.window, app.doc); app.tool = Tool::Eraser; break;
        case Action::ToolFill:   app.text.finish(app.window, app.doc); app.tool = Tool::Fill;   break;
        case Action::ToolSelect: app.text.finish(app.window, app.doc); app.tool = Tool::Select; break;
        case Action::ToolTransform:
            app.text.finish(app.window, app.doc);
            app.tool = Tool::Transform;
            break;
        case Action::ToolText: chooseTextTool(app); break;
        case Action::ToolLinework:
            app.text.finish(app.window, app.doc);
            app.tool = Tool::Linework;
            break;
        case Action::ToolGradient:
            app.text.finish(app.window, app.doc);
            app.tool = Tool::Gradient;
            break;

        case Action::SizeDown: stepSizePreset(app, -1); break;
        case Action::SizeUp:   stepSizePreset(app, +1); break;
        case Action::SwapColours:
            std::swap(app.foreground, app.background);
            syncHsvFromColour(app);
            break;
        case Action::ResetColours:
            app.foreground = sbl::StraightRgba8{0, 0, 0, 255};
            app.background = sbl::StraightRgba8{255, 255, 255, 255};
            syncHsvFromColour(app);
            break;

        // Toggling only. The axes and the points stay exactly where they were,
        // so a ruler can be switched off to check the drawing and back on.
        case Action::ToggleSymmetry:
            app.symmetry.enabled = !app.symmetry.enabled;
            break;
        case Action::TogglePerspective:
            app.perspective.enabled = !app.perspective.enabled;
            break;

        case Action::Count: break;   // not bound to anything
    }
}

void handleEvent(App& app, const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
    app.framesToSettle = 2;

    const ImGuiIO& io = ImGui::GetIO();

    switch (e.type) {
        case SDL_EVENT_QUIT:
            requestPending(app, Pending::Quit);
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (e.window.windowID == SDL_GetWindowID(app.window))
                requestPending(app, Pending::Quit);
            break;

        // D-002 cost #2, answered: characters reach the canvas through SDL's
        // text-input events, never through an ImGui field. TEXT_EDITING is the
        // half-finished one an input method is still choosing — the whole
        // reason this tool is not a ImGui::InputText.
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_TEXT_EDITING:
            if (!io.WantCaptureKeyboard &&
                app.text.handleEvent(e, app.doc, app.window))
                syncTextures(app, app.text.takeChanged());
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (io.WantCaptureKeyboard) break;
            // A live text session owns the keyboard: `b` types a b rather than
            // switching to the brush and abandoning the sentence.
            if (app.text.handleEvent(e, app.doc, app.window)) {
                syncTextures(app, app.text.takeChanged());
                break;
            }
            handleKey(app, e.key);
            break;

        case SDL_EVENT_DROP_FILE:
            // A drop is an open like any other, so it goes through the same
            // unsaved-work prompt. SDL owns e.drop.data only for this event,
            // hence the copy — the path may sit in `pending` for many frames
            // while the artist decides.
            if (e.drop.data != nullptr)
                requestPending(app, Pending::Open, e.drop.data);
            break;

        // ------------------------------------------------------------- pen
        case SDL_EVENT_PEN_PROXIMITY_IN:
            app.penSeen   = true;
            app.activePen = e.pproximity.which;
            break;

        case SDL_EVENT_PEN_AXIS: {
            // Axis changes arrive separately from motion. Record and wait.
            app.penSeen = true;
            app.activePen = e.paxis.which;
            PenAxisState& axes = app.penAxes[e.paxis.which];
            switch (e.paxis.axis) {
                case SDL_PEN_AXIS_PRESSURE: axes.pressure = e.paxis.value; break;
                case SDL_PEN_AXIS_XTILT:    axes.tiltX    = e.paxis.value; break;
                case SDL_PEN_AXIS_YTILT:    axes.tiltY    = e.paxis.value; break;
                case SDL_PEN_AXIS_ROTATION: axes.rotation = e.paxis.value; break;
                case SDL_PEN_AXIS_DISTANCE: axes.distance = e.paxis.value; break;
                default: break;
            }
            break;
        }

        case SDL_EVENT_PEN_DOWN:
            app.penSeen = true;
            app.activePen = e.ptouch.which;
            if (io.WantCaptureMouse || !overCanvas(app, e.ptouch.x, e.ptouch.y)) break;
            // The eraser end is a different tool, not a modifier (US-08.6).
            if (e.ptouch.eraser) app.tool = Tool::Eraser;
            else if (app.tool == Tool::Eraser) app.tool = Tool::Brush;
            // Landing on a guide handle moves it instead of painting.
            app.draggingGuide = guideAt(app, e.ptouch.x, e.ptouch.y);
            if (app.draggingGuide != kNoGuide) break;
            // Text and the selection tools all go through the same helpers
            // the mouse uses. A tablet is the target input device, so a lasso
            // reachable only with a mouse would be one most artists never use.
            if (app.tool == Tool::Text) {
                placeText(app, e.ptouch.x, e.ptouch.y);
                syncTextures(app, app.text.takeChanged());
                break;
            }
            if (app.tool == Tool::Linework) {
                lineworkPress(app, e.ptouch.x, e.ptouch.y,
                              penSample(app, e.ptouch.which,
                                        e.ptouch.x, e.ptouch.y).pressure);
                break;
            }
            if (app.tool == Tool::Wand) {
                wandAt(app, e.ptouch.x, e.ptouch.y);
                break;
            }
            if (app.tool == Tool::Gradient) {
                beginGradient(app, e.ptouch.x, e.ptouch.y);
                break;
            }
            if (selectsRegion(app.tool)) {
                beginSelectDrag(app, e.ptouch.x, e.ptouch.y);
                break;
            }
            beginPaint(app);
            paintWith(app, penSample(app, e.ptouch.which, e.ptouch.x, e.ptouch.y));
            break;

        case SDL_EVENT_PEN_MOTION:
            app.activePen = e.pmotion.which;
            ++app.motionThisFrame;
            // Hovering without contact does not paint (US-08.8) — the pen only
            // paints between PEN_DOWN and PEN_UP.
            if (app.draggingGuide != kNoGuide)
                moveGuide(app, e.pmotion.x, e.pmotion.y);
            else if (app.selecting)
                dragSelection(app, e.pmotion.x, e.pmotion.y);
            else if (app.linework.busy())
                lineworkDrag(app, e.pmotion.x, e.pmotion.y,
                             penSample(app, e.pmotion.which,
                                       e.pmotion.x, e.pmotion.y).pressure);
            else if (app.gradientDragging)
                previewGradient(app, e.pmotion.x, e.pmotion.y);
            else if (app.painting)
                paintWith(app, penSample(app, e.pmotion.which, e.pmotion.x, e.pmotion.y));
            break;

        case SDL_EVENT_PEN_UP:
            app.draggingGuide = kNoGuide;
            endSelectDrag(app, e.ptouch.x, e.ptouch.y);
            lineworkRelease(app);
            endGradient(app, e.ptouch.x, e.ptouch.y);
            endPaint(app);
            break;

        case SDL_EVENT_PEN_BUTTON_DOWN:
        case SDL_EVENT_PEN_BUTTON_UP:
            app.lastPenButton = e.pbutton.down ? e.pbutton.button : 0;
            break;

        // ----------------------------------------------------------- mouse
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (io.WantCaptureMouse || !overCanvas(app, e.button.x, e.button.y)) break;
            if (e.button.button == SDL_BUTTON_MIDDLE ||
                (e.button.button == SDL_BUTTON_LEFT && app.spaceHeld)) {
                app.panning    = true;
                // Screen space on both sides, so this stays right at any
                // rotation: pan translates the whole mapping, it does not
                // travel along the canvas axes.
                app.panAnchorX = e.button.x - app.view.panX;
                app.panAnchorY = e.button.y - app.view.panY;
            } else if (e.button.button == SDL_BUTTON_LEFT) {
                // A guide handle takes the press before any tool does, so the
                // artist never has to switch tools to move a ruler.
                app.draggingGuide = guideAt(app, e.button.x, e.button.y);
                if (app.draggingGuide != kNoGuide) break;
                if ((SDL_GetModState() & SDL_KMOD_ALT) != 0) {
                    pickColourAt(app, e.button.x, e.button.y);   // US-13.3
                } else if (app.tool == Tool::Fill) {
                    doFill(app, e.button.x, e.button.y);
                } else if (app.tool == Tool::Text) {
                    placeText(app, e.button.x, e.button.y);
                    syncTextures(app, app.text.takeChanged());
                } else if (app.tool == Tool::Linework) {
                    lineworkPress(app, e.button.x, e.button.y, 1.0f);
                } else if (app.tool == Tool::Wand) {
                    wandAt(app, e.button.x, e.button.y);
                } else if (app.tool == Tool::Gradient) {
                    beginGradient(app, e.button.x, e.button.y);
                } else if (selectsRegion(app.tool)) {
                    beginSelectDrag(app, e.button.x, e.button.y);
                } else {
                    beginPaint(app);
                    paintWith(app, mouseSample(app, e.button.x, e.button.y));
                }
            }
            break;

        case SDL_EVENT_MOUSE_MOTION:
            ++app.motionThisFrame;
            if (app.panning) {
                // Panning never alters pixels: no undo state, no dirty flag,
                // no tile upload (US-05.5).
                app.view.panX = e.motion.x - app.panAnchorX;
                app.view.panY = e.motion.y - app.panAnchorY;
            } else if (app.draggingGuide != kNoGuide) {
                moveGuide(app, e.motion.x, e.motion.y);
            } else if (app.selecting) {
                dragSelection(app, e.motion.x, e.motion.y);
            } else if (app.linework.busy()) {
                lineworkDrag(app, e.motion.x, e.motion.y, 1.0f);
            } else if (app.gradientDragging) {
                previewGradient(app, e.motion.x, e.motion.y);
            } else if (app.painting) {
                paintWith(app, mouseSample(app, e.motion.x, e.motion.y));
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            // Handled wherever the pointer is: releasing outside the window
            // must still end the stroke cleanly (US-02.5).
            app.panning = false;
            if (e.button.button == SDL_BUTTON_LEFT) {
                app.draggingGuide = kNoGuide;
                endSelectDrag(app, e.button.x, e.button.y);
                lineworkRelease(app);
                endGradient(app, e.button.x, e.button.y);
                endPaint(app);
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            if (!io.WantCaptureMouse) {
                float mx = 0.0f, my = 0.0f;
                SDL_GetMouseState(&mx, &my);
                zoomAbout(app.view, mx, my,
                          e.wheel.y > 0 ? kZoomStep : 1.0f / kZoomStep);
            }
            break;

        default:
            break;
    }
}

// ------------------------------------------------------------------------- ui

/// Panels are dockable from Milestone 4 (D-016). The status bar is not — it is
/// a readout pinned to the bottom edge, and letting it be dragged into a tab
/// would be a worse application, not a more flexible one.
constexpr ImGuiWindowFlags kStatusFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;

/// Builds the default arrangement the first time the application runs, or
/// whenever the artist asks for it back. Without this a fresh install opens
/// with every panel floating in the middle, which reads as broken.
void buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);

    ImGuiID centre = dockspace;
    const ImGuiID left  = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.18f,
                                                      nullptr, &centre);
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.24f,
                                                nullptr, &centre);
    const ImGuiID rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f,
                                                           nullptr, &right);

    ImGui::DockBuilderDockWindow("Tools",  left);
    ImGui::DockBuilderDockWindow("Colour", right);
    ImGui::DockBuilderDockWindow("Layers", rightLower);
    ImGui::DockBuilderFinish(dockspace);
}

void drawMenuBar(App& app, float& menuHeight) {
    if (!ImGui::BeginMainMenuBar()) return;
    menuHeight = ImGui::GetWindowSize().y;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", app.shortcuts.get(Action::NewCanvas).label().c_str())) requestPending(app, Pending::NewCanvas);
        if (ImGui::MenuItem("Open...", app.shortcuts.get(Action::OpenProject).label().c_str())) requestPending(app, Pending::Open);
        ImGui::Separator();
        if (ImGui::MenuItem("Save", app.shortcuts.get(Action::Save).label().c_str())) doSave(app);
        if (ImGui::MenuItem("Save as...", app.shortcuts.get(Action::SaveAs).label().c_str())) askForSavePath(app);
        ImGui::Separator();
        // Import and Export are whatever the registry knows about; the menu
        // greys out rather than opening a dialog with nothing in it.
        if (ImGui::MenuItem("Import...", nullptr, false,
                            !sbl::dialogFilters(sbl::FormatUse::Read, false).empty()))
            requestPending(app, Pending::Import);
        if (ImGui::MenuItem("Export...", app.shortcuts.get(Action::ExportPng).label().c_str(), false,
                            !sbl::dialogFilters(sbl::FormatUse::Write, false).empty()))
            askForExportPath(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", app.shortcuts.get(Action::Quit).label().c_str())) requestPending(app, Pending::Quit);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        // US-04.6: correct enabled/disabled state, with the action's label.
        const std::string undoLabel =
            app.doc.undo.canUndo() ? "Undo " + app.doc.undo.undoLabel() : "Undo";
        const std::string redoLabel =
            app.doc.undo.canRedo() ? "Redo " + app.doc.undo.redoLabel() : "Redo";
        if (ImGui::MenuItem(undoLabel.c_str(), app.shortcuts.get(Action::Undo).label().c_str(), false,
                            app.doc.undo.canUndo())) doUndo(app);
        if (ImGui::MenuItem(redoLabel.c_str(), app.shortcuts.get(Action::Redo).label().c_str(), false,
                            app.doc.undo.canRedo())) doRedo(app);
        ImGui::Separator();
        const sbl::Layer* layer = app.doc.layerById(app.doc.activeLayer);
        if (ImGui::MenuItem("Clear", nullptr, false,
                            layer != nullptr && !layer->tiles.empty())) doClear(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Fill selection", nullptr, false, layer != nullptr)) {
            sbl::UndoRecord rec =
                sbl::fillSelection(app.doc, app.doc.activeLayer, app.foreground);
            if (!rec.empty()) {
                for (const auto& snap : rec.tiles) app.canvas->markDirty(snap.key);
                app.doc.undo.push(std::move(rec));
                app.doc.dirty = true;
            }
        }
        if (ImGui::MenuItem("Deselect", app.shortcuts.get(Action::Deselect).label().c_str(), false,
                            app.doc.selection.has_value()))
            app.doc.selection.reset();
        ImGui::Separator();
        ImGui::TextDisabled("History: %zu steps, %.1f MB of %d MB",
                            app.doc.undo.size(),
                            static_cast<double>(app.doc.undo.memoryBytes()) /
                                (1024.0 * 1024.0),
                            app.undoBudgetMb);
        if (app.doc.undo.droppedRecords() > 0)
            ImGui::TextDisabled("%zu older step(s) dropped to stay in budget.",
                                app.doc.undo.droppedRecords());
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Fit to window", app.shortcuts.get(Action::FitToWindow).label().c_str()))
            fitToViewport(app.view, app.doc, app.viewport);
        if (ImGui::MenuItem("Actual size", app.shortcuts.get(Action::ActualSize).label().c_str()))
            zoomToActualSize(app.view, app.doc, app.viewport);
        if (ImGui::MenuItem("Rotate left", app.shortcuts.get(Action::RotateLeft).label().c_str()))
            rotateView(app, -kRotateStep);
        if (ImGui::MenuItem("Rotate right", app.shortcuts.get(Action::RotateRight).label().c_str()))
            rotateView(app, +kRotateStep);
        if (ImGui::MenuItem("Reset rotation", app.shortcuts.get(Action::ResetRotation).label().c_str(),
                            false, app.view.rotation != 0.0))
            rotateView(app, -app.view.rotation);
        ImGui::Separator();
        // D-021: one binary, one toggle, defaulting to the CPU. Disabled
        // rather than hidden when there is no device, so a bug report can say
        // "the menu item is greyed out" and the reason is one hover away.
        const bool haveGpu = sbl::gpuBackendCompiledIn();
        ImGui::BeginDisabled(!haveGpu || app.painting);
        if (ImGui::MenuItem("Paint and composite on the GPU", nullptr, &app.useGpu))
            applyGpuMode(app);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s\n\nThe CPU path is the reference: if the two disagree, the "
                "CPU is right. Switch back and export again to find out which "
                "one a colour problem is in.",
                haveGpu ? app.gpuWhy.c_str() : "This build has no GPU backend.");
        }
        ImGui::Separator();
        ImGui::MenuItem("Pressure calibration", nullptr, &app.showCalibration);
        ImGui::MenuItem("Tablet test pad", nullptr, &app.showTestPad);
        ImGui::MenuItem("Keyboard and interface", nullptr, &app.showShortcuts);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset panel layout")) app.resetLayout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void drawToolPanel(App& app) {
    if (ImGui::Begin("Tools")) {
        ImGui::TextDisabled("TOOL");
        // The binding is shown rather than hard-coded, so a reassigned key
        // stays truthful here (D-017's sibling problem — a stale label is a
        // small lie the artist has to discover by trying it).
        const auto toolRow = [&](Icon icon, const char* name, Tool tool, Action action) {
            char label[64];
            std::snprintf(label, sizeof label, "%s  (%s)", name,
                          app.shortcuts.get(action).label().c_str());
            if (!iconRadio(icon, label, app.tool == tool)) return;
            // Same rule as the keyboard: picking another tool commits the text.
            if (tool == Tool::Text) chooseTextTool(app);
            else { app.text.finish(app.window, app.doc); app.tool = tool; }
        };
        toolRow(Icon::Brush,     "Brush",     Tool::Brush,     Action::ToolBrush);
        toolRow(Icon::Eraser,    "Eraser",    Tool::Eraser,    Action::ToolEraser);
        toolRow(Icon::Fill,      "Fill",      Tool::Fill,      Action::ToolFill);
        toolRow(Icon::Gradient,  "Gradient",  Tool::Gradient,  Action::ToolGradient);
        toolRow(Icon::Select,    "Select",    Tool::Select,    Action::ToolSelect);
        toolRow(Icon::Lasso,     "Lasso",     Tool::Lasso,     Action::ToolLasso);
        toolRow(Icon::Wand,      "Magic wand", Tool::Wand,     Action::ToolWand);
        toolRow(Icon::Transform, "Transform", Tool::Transform, Action::ToolTransform);
        toolRow(Icon::Text,      "Text",      Tool::Text,      Action::ToolText);
        toolRow(Icon::Linework,  "Linework",  Tool::Linework,  Action::ToolLinework);

        if (app.tool == Tool::Text) {
            app.text.drawPanel(app.doc);
            syncTextures(app, app.text.takeChanged());
        }

        if (app.tool == Tool::Linework) {
            // The foreground colour is what a new stroke takes and what the
            // panel writes over a selected one, so it is read here rather than
            // only at pen-down: the artist picks the colour, then applies it.
            app.linework.colour = app.foreground;
            app.linework.drawPanel(app.doc);
            syncTextures(app, app.linework.takeChanged());
        }

        if (app.tool == Tool::Transform) {
            if (!app.doc.selection.has_value() || app.doc.selection->empty()) {
                ImGui::TextDisabled("Select a region first.");
            } else {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragScalarN("##move", ImGuiDataType_Double,
                                   &app.pendingTransform.dx, 2, 0.5f);
                ImGui::TextDisabled("offset x, y");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragScalarN("##scale", ImGuiDataType_Double,
                                   &app.pendingTransform.scaleX, 2, 0.01f);
                ImGui::TextDisabled("scale x, y");

                auto degrees = static_cast<float>(app.pendingTransform.angle * 180.0 / 3.14159265358979323846);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##rot", &degrees, -180.0f, 180.0f, "%.1f deg"))
                    app.pendingTransform.angle =
                        static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;

                if (ImGui::Button("Apply", ImVec2(-1.0f, 0.0f))) {
                    sbl::UndoRecord rec = sbl::transformRegion(
                        app.doc, app.doc.activeLayer, *app.doc.selection,
                        app.pendingTransform);
                    if (!rec.empty()) {
                        app.canvas->releaseAll();
                        app.doc.undo.push(std::move(rec));
                        app.doc.dirty = true;
                    }
                    app.pendingTransform = sbl::Transform{};
                }
                if (ImGui::Button("Reset", ImVec2(-1.0f, 0.0f)))
                    app.pendingTransform = sbl::Transform{};
            }
        }

        if (app.tool == Tool::Fill) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderInt("##tol", &app.fillTolerance, 0, 128, "tolerance %d");
        }
        if (app.tool == Tool::Gradient) {
            if (ImGui::RadioButton("Linear", app.gradientShape == sbl::GradientShape::Linear))
                app.gradientShape = sbl::GradientShape::Linear;
            if (ImGui::RadioButton("Radial", app.gradientShape == sbl::GradientShape::Radial))
                app.gradientShape = sbl::GradientShape::Radial;
            ImGui::Checkbox("To transparent", &app.gradientToTransparent);
            if (app.doc.depth == sbl::ColourDepth::Bits8) {
                ImGui::Checkbox("Dither", &app.gradientDither);
                ImGui::TextDisabled("breaks up 8-bit banding");
            } else {
                // Said rather than hidden: the checkbox vanishing without a
                // word reads as a missing feature, not as one that has nothing
                // left to do.
                ImGui::TextDisabled("16-bit: no dither needed");
            }
            ImGui::TextDisabled("drag to set the axis");
        }
        if (selectsRegion(app.tool)) {
            if (app.tool == Tool::Wand) {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderInt("##wandtol", &app.wandTolerance, 0, 128, "tolerance %d");
            }
            // Buttons as well as modifier keys. The keyboard is the only
            // accessibility affordance Sable has (PRD §6), so a mode that can
            // only be reached by holding Shift while dragging is a mode some
            // users do not have.
            ImGui::TextDisabled("MODE");
            const auto modeRow = [&](const char* label, sbl::SelectMode mode) {
                if (ImGui::RadioButton(label, app.selectMode == mode))
                    app.selectMode = mode;
            };
            modeRow("Replace",        sbl::SelectMode::Replace);
            modeRow("Add (Shift)",    sbl::SelectMode::Add);
            modeRow("Subtract (Alt)", sbl::SelectMode::Subtract);
            modeRow("Intersect",      sbl::SelectMode::Intersect);
            if (app.doc.selection.has_value() &&
                ImGui::SmallButton("Deselect")) app.doc.selection.reset();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("BRUSH PRESET");
        for (std::size_t i = 0; i < app.brushes.size(); ++i) {
            if (ImGui::RadioButton(app.brushes[i].name.c_str(), app.brushIndex == i)) {
                app.brushIndex = i;
                if (app.tool == Tool::Eraser) app.tool = Tool::Brush;
            }
        }

        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("##presetname", app.presetName, sizeof app.presetName);
        ImGui::SameLine();
        if (ImGui::SmallButton("Save")) {
            // A custom preset is a copy of the current brush under a new name,
            // with an id derived from it so it survives a restart.
            sbl::BrushPreset copy = activeBrush(app);
            copy.name = app.presetName;
            copy.id   = "custom-" + std::to_string(app.brushes.size()) + "-" + copy.name;
            app.brushes.push_back(std::move(copy));
            app.brushIndex = app.brushes.size() - 1;
            app.tool = Tool::Brush;
        }
        if (app.brushIndex >= 7 && ImGui::SmallButton("Delete preset")) {
            app.brushes.erase(app.brushes.begin() +
                              static_cast<std::ptrdiff_t>(app.brushIndex));
            app.brushIndex = 0;
        }

        ImGui::Spacing();
        ImGui::TextDisabled("SIZE");
        sbl::BrushPreset& brush = activeBrush(app);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##size", &brush.size, 1.0f, 500.0f, "%.1f px",
                           ImGuiSliderFlags_Logarithmic);      // US-12.1
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputFloat("##sizenum", &brush.size, 1.0f, 10.0f, "%.1f");
        brush.size = std::clamp(brush.size, 1.0f, 500.0f);

        ImGui::TextDisabled("presets  [ and ]");
        for (std::size_t i = 0; i < app.sizePresets.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            char label[16];
            std::snprintf(label, sizeof label, "%g", static_cast<double>(app.sizePresets[i]));
            if (ImGui::SmallButton(label)) brush.size = app.sizePresets[i];
            // Right-click stores the current size here, which is what makes
            // the presets editable (US-12.3).
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                app.sizePresets[i] = brush.size;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click to use, right-click to set to %.1f",
                                  static_cast<double>(brush.size));
            ImGui::PopID();
            if ((i + 1) % 4 != 0 && i + 1 < app.sizePresets.size()) ImGui::SameLine();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("STABILIZER");
        int level = brush.stabilizerLevel;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##stab", &level, "Off\0Low\0Medium\0High\0")) {
            // Saved per brush preset (US-11.5).
            brush.stabilizerLevel = static_cast<std::uint8_t>(level);
        }

        ImGui::Spacing();
        ImGui::TextDisabled("RULERS");
        // Bindings shown, not hard-coded, for the same reason as the tool rows.
        char rulerLabel[64];
        std::snprintf(rulerLabel, sizeof rulerLabel, "Symmetry  (%s)",
                      app.shortcuts.get(Action::ToggleSymmetry).label().c_str());
        ImGui::Checkbox(rulerLabel, &app.symmetry.enabled);
        if (app.symmetry.enabled) {
            ImGui::Indent();
            ImGui::Checkbox("Vertical axis", &app.symmetry.vertical);
            ImGui::Checkbox("Horizontal axis", &app.symmetry.horizontal);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderInt("##radial", &app.symmetry.radial, 1,
                             sbl::SymmetryRuler::kMaxRadial, "radial %d");
            if (ImGui::SmallButton("Recentre")) centreSymmetry(app);
            ImGui::TextDisabled("or drag the handle");
            ImGui::Unindent();
        }

        std::snprintf(rulerLabel, sizeof rulerLabel, "Perspective  (%s)",
                      app.shortcuts.get(Action::TogglePerspective).label().c_str());
        ImGui::Checkbox(rulerLabel, &app.perspective.enabled);
        if (app.perspective.enabled) {
            ImGui::Indent();
            for (int n = 1; n <= 3; ++n) {
                char button[8];
                std::snprintf(button, sizeof button, "%d pt", n);
                if (ImGui::SmallButton(button)) setVanishingPoints(app, n);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("None")) setVanishingPoints(app, 0);
            for (std::size_t i = 0; i < app.doc.vanishingPoints.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Checkbox("##on", &app.doc.vanishingPoints[i].enabled);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragScalarN("##xy", ImGuiDataType_Double,
                                       &app.doc.vanishingPoints[i].x, 2, 1.0f))
                    app.doc.dirty = true;
                ImGui::PopID();
            }
            ImGui::Unindent();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("BRUSH");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##hardness", &brush.hardness, 0.0f, 1.0f, "hardness %.2f");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##density", &brush.density, 0.05f, 1.0f, "density %.2f");

        ImGui::Spacing();
        ImGui::TextDisabled("PRESSURE DRIVES");
        ImGui::Checkbox("Size", &brush.pressure.toSize);        // US-08.4
        ImGui::Checkbox("Density", &brush.pressure.toDensity);
    }
    ImGui::End();
}

/// Records a layer operation, refreshing the textures it disturbed. Layer
/// changes move pixels around wholesale, so the simple thing is to drop the
/// texture cache and let the next frame rebuild what it needs.
void pushLayerAction(App& app, sbl::UndoRecord&& rec) {
    if (rec.empty()) return;
    app.canvas->releaseAll();
    app.doc.undo.push(std::move(rec));
    app.doc.dirty = true;
}

void drawLayerPanel(App& app) {
    if (ImGui::Begin("Layers")) {
        ImGui::TextDisabled("LAYERS");

        if (iconButton(Icon::Plus, "##addlayer", "New layer"))
            pushLayerAction(app, sbl::addLayerAbove(app.doc, app.doc.activeLayer,
                                                    "Layer " + std::to_string(app.doc.nextLayerId)));
        ImGui::SameLine();
        if (iconButton(Icon::Duplicate, "##duplayer", "Duplicate layer"))
            pushLayerAction(app, sbl::duplicateLayer(app.doc, app.doc.activeLayer));
        ImGui::SameLine();
        // Disabled rather than hidden when it would be a no-op, so the row
        // does not reflow under the pointer.
        ImGui::BeginDisabled(app.doc.layers.size() <= 1);
        if (iconButton(Icon::Delete, "##dellayer", "Delete layer"))
            pushLayerAction(app, sbl::deleteLayer(app.doc, app.doc.activeLayer));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (iconButton(Icon::Merge, "##mergelayer", "Merge down"))
            pushLayerAction(app, sbl::mergeLayerDown(app.doc, app.doc.activeLayer));

        if (iconButton(Icon::Group, "##grouplayer", "Group this layer")) {
            // A folder above the active layer, with that layer moved inside —
            // which is what "group this" means without a drag-and-drop tree.
            const sbl::LayerId child = app.doc.activeLayer;
            sbl::UndoRecord rec = sbl::addLayerAbove(app.doc, child, "Group");
            if (!rec.empty()) {
                const sbl::LayerId folder = app.doc.activeLayer;
                if (sbl::Layer* group = app.doc.layerById(folder); group != nullptr)
                    group->kind = sbl::LayerKind::Folder;
                if (sbl::Layer* inner = app.doc.layerById(child); inner != nullptr)
                    inner->parent = folder;
                pushLayerAction(app, std::move(rec));
                app.doc.activeLayer = folder;
            }
        }
        ImGui::SameLine();
        if (iconButton(Icon::Ungroup, "##ungrouplayer", "Move out of its group")) {
            if (sbl::Layer* layer = app.doc.layerById(app.doc.activeLayer);
                layer != nullptr && layer->parent.has_value()) {
                sbl::LayerProps props = sbl::propsOf(*layer);
                props.parent.reset();
                pushLayerAction(app, sbl::setLayerProps(app.doc, layer->id, props));
            }
        }

        ImGui::SameLine();
        if (iconButton(Icon::Raise, "##raiselayer", "Raise layer"))
            pushLayerAction(app, sbl::moveLayer(app.doc, app.doc.activeLayer, +1));
        ImGui::SameLine();
        if (iconButton(Icon::Lower, "##lowerlayer", "Lower layer"))
            pushLayerAction(app, sbl::moveLayer(app.doc, app.doc.activeLayer, -1));

        ImGui::Separator();

        // Top of the stack first, which is how the artist thinks about it —
        // Document::layers is bottom-first.
        sbl::LayerId propsFor = sbl::NO_LAYER;
        sbl::LayerProps edited;

        for (std::size_t i = app.doc.layers.size(); i-- > 0;) {
            sbl::Layer& layer = app.doc.layers[i];
            ImGui::PushID(static_cast<int>(layer.id));

            if (iconToggle(layer.visible ? Icon::Eye : Icon::EyeClosed, "##vis",
                           false, layer.visible ? "Hide layer" : "Show layer",
                           ImGui::GetFrameHeight() * 0.85f)) {
                edited = sbl::propsOf(layer);
                edited.visible = !layer.visible;
                propsFor = layer.id;
            }
            ImGui::SameLine();

            const bool active = layer.id == app.doc.activeLayer;
            if (layer.parent.has_value()) ImGui::Indent(16.0f);
            char label[160];
            std::snprintf(label, sizeof label, "%s%s%s",
                          layer.kind == sbl::LayerKind::Folder   ? "[group] "
                          : layer.kind == sbl::LayerKind::Text   ? "[text] "
                          : layer.kind == sbl::LayerKind::Linework ? "[line] " : "",
                          layer.name.c_str(), layer.locked ? "  [locked]" : "");
            if (ImGui::Selectable(label, active)) app.doc.activeLayer = layer.id;

            if (active) {
                ImGui::Indent(12.0f);
                float opacity = layer.opacity;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##op", &opacity, 0.0f, 1.0f, "opacity %.2f")) {
                    layer.opacity = opacity;    // live while dragging...
                    // Opacity is baked into the composited tiles now, so the
                    // drag is only visible if they are rebuilt.
                    app.canvas->markAllDirty();
                    app.doc.dirty = true;
                }
                // ...and one undo step when the drag ends, not one per frame.
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    const float settled = layer.opacity;
                    layer.opacity = 1.0f;
                    edited = sbl::propsOf(layer);
                    edited.opacity = settled;
                    propsFor = layer.id;
                }

                // Display names, in BlendMode order. The static_assert is the
                // whole point: add a mode to the enum without a label here and
                // the build stops, instead of the dropdown quietly setting the
                // wrong mode.
                static const char* const blendLabels[] = {
                    "Normal", "Multiply", "Screen", "Add", "Overlay",
                    "Darken", "Lighten", "Colour Dodge", "Colour Burn",
                    "Hard Light", "Soft Light", "Difference", "Exclusion",
                };
                static_assert(IM_ARRAYSIZE(blendLabels) == sbl::ALL_BLEND_MODES.size());

                int blend = static_cast<int>(layer.blend);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##blend", &blend, blendLabels,
                                 IM_ARRAYSIZE(blendLabels))) {
                    edited = sbl::propsOf(layer);
                    edited.blend = static_cast<sbl::BlendMode>(blend);
                    propsFor = layer.id;
                }

                bool locked = layer.locked;
                if (ImGui::Checkbox("Lock", &locked)) {
                    edited = sbl::propsOf(layer);
                    edited.locked = locked;
                    propsFor = layer.id;
                }
                ImGui::SameLine();
                bool preserve = layer.preserveOpacity;
                if (ImGui::Checkbox("Keep alpha", &preserve)) {
                    edited = sbl::propsOf(layer);
                    edited.preserveOpacity = preserve;
                    propsFor = layer.id;
                }
                bool clip = layer.clipToBelow;
                if (ImGui::Checkbox("Clip to layer below", &clip)) {
                    edited = sbl::propsOf(layer);
                    edited.clipToBelow = clip;
                    propsFor = layer.id;
                }
                // Text owns its layer's pixels, so painting on one and merging
                // one down are both refused. This is the way out: give up the
                // words and keep the picture. Undoable like any other property
                // change, which is what makes it safe to offer.
                if (layer.kind == sbl::LayerKind::Text &&
                    ImGui::SmallButton("Rasterise text")) {
                    app.text.finish(app.window, app.doc);
                    edited = sbl::propsOf(layer);
                    edited.text.reset();
                    propsFor = layer.id;
                }
                // The same way out of a linework layer, for the same reason:
                // give up the curves, keep the line, and get the paint tools
                // back. #17's last acceptance criterion.
                if (layer.kind == sbl::LayerKind::Linework &&
                    ImGui::SmallButton("Rasterise linework")) {
                    app.linework.release(app.doc);
                    edited = sbl::propsOf(layer);
                    edited.linework.reset();
                    propsFor = layer.id;
                }
                ImGui::Unindent(12.0f);
            }
            if (layer.parent.has_value()) ImGui::Unindent(16.0f);
            ImGui::PopID();
        }

        // Applied after the loop: setLayerProps can reorder nothing, but it
        // does write through a Layer& that the loop is iterating over.
        if (propsFor != sbl::NO_LAYER) {
            sbl::UndoRecord rec = sbl::setLayerProps(app.doc, propsFor, edited);
            if (!rec.empty()) {
                app.canvas->releaseAll();
                app.doc.undo.push(std::move(rec));
                app.doc.dirty = true;
            }
        }
    }
    ImGui::End();
}

void drawColourPanel(App& app) {
    if (ImGui::Begin("Colour")) {
        ImGui::TextDisabled("COLOUR");

        // The wheel is a custom widget — there is no built-in one (D-002).
        if (colourWheel("fg", app.foregroundHsv, rightPanel(app) - 30.0f * app.uiScale))
            syncColourFromHsv(app);

        ImGui::Spacing();
        bool hsvChanged = false;
        ImGui::SetNextItemWidth(-1.0f);
        hsvChanged |= ImGui::SliderFloat("##h", &app.foregroundHsv[0], 0.0f, 1.0f, "H %.3f");
        ImGui::SetNextItemWidth(-1.0f);
        hsvChanged |= ImGui::SliderFloat("##s", &app.foregroundHsv[1], 0.0f, 1.0f, "S %.3f");
        ImGui::SetNextItemWidth(-1.0f);
        hsvChanged |= ImGui::SliderFloat("##v", &app.foregroundHsv[2], 0.0f, 1.0f, "V %.3f");
        if (hsvChanged) syncColourFromHsv(app);

        // Hex, accepted and displayed (US-13.2).
        char hex[8];
        std::snprintf(hex, sizeof hex, "%02X%02X%02X",
                      app.foreground.r, app.foreground.g, app.foreground.b);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##hex", hex, sizeof hex,
                             ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            unsigned value = 0;
            if (std::sscanf(hex, "%x", &value) == 1) {
                app.foreground.r = static_cast<std::uint8_t>((value >> 16) & 0xFF);
                app.foreground.g = static_cast<std::uint8_t>((value >> 8) & 0xFF);
                app.foreground.b = static_cast<std::uint8_t>(value & 0xFF);
                syncHsvFromColour(app);
            }
        }

        ImGui::Spacing();
        const auto swatch = [](sbl::StraightRgba8 c) {
            return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
        };
        ImGui::ColorButton("##fgswatch", swatch(app.foreground),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(48, 32));
        ImGui::SameLine();
        ImGui::ColorButton("##bgswatch", swatch(app.background),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(48, 32));
        ImGui::SameLine();
        if (iconButton(Icon::Swap, "##swapcolours", "Swap foreground and background", 32.0f)) {
            std::swap(app.foreground, app.background);
            syncHsvFromColour(app);
        }
        ImGui::SameLine();
        if (iconButton(Icon::Reset, "##resetcolours", "Reset to black and white", 32.0f)) {
            app.foreground = sbl::StraightRgba8{0, 0, 0, 255};
            app.background = sbl::StraightRgba8{255, 255, 255, 255};
            syncHsvFromColour(app);
        }
        ImGui::TextDisabled("Alt+click samples the canvas");
    }
    ImGui::End();
}

void drawCalibration(App& app) {
    if (!app.showCalibration) return;
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Pressure calibration", &app.showCalibration,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        // US-09.1: Soft, Normal and Hard presets, plus an editable curve.
        if (ImGui::Button("Soft"))   app.profile.curve = sbl::curveSoft();
        ImGui::SameLine();
        if (ImGui::Button("Normal")) app.profile.curve = sbl::curveLinear();
        ImGui::SameLine();
        if (ImGui::Button("Hard"))   app.profile.curve = sbl::curveHard();

        pressureCurveEditor("curve", app.profile.curve, 260.0f,
                            app.lastFromMouse ? -1.0f : app.pressureFilter.lastRescaled());
        ImGui::TextDisabled("drag the points to shape the response");

        ImGui::Separator();
        ImGui::SliderFloat("Minimum", &app.profile.rawMin, 0.0f, 0.9f, "%.2f");
        ImGui::SliderFloat("Maximum", &app.profile.rawMax, 0.1f, 1.0f, "%.2f");
        ImGui::SliderFloat("Smoothing", &app.profile.smoothing, 0.0f, 1.0f, "%.2f");
        if (app.profile.rawMin >= app.profile.rawMax)
            app.profile.rawMin = std::max(0.0f, app.profile.rawMax - 0.05f);

        // US-09.5: any change affects the next stroke immediately. Nothing to
        // do — the profile is read on every sample.
        ImGui::TextDisabled("Changes apply to the next stroke.");
    }
    ImGui::End();
}

void drawTestPad(App& app) {
    if (!app.showTestPad) return;
    ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Tablet test pad", &app.showTestPad,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        // US-10.6: say so plainly rather than showing zeros.
        if (!app.penSeen) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No tablet detected.");
            ImGui::TextWrapped(
                "No pen events have arrived. Drawing with the mouse still works; "
                "everything below stays blank until a stylus is used.");
            ImGui::Separator();
        }

        ImGui::Text("Input:      %s", app.lastFromMouse ? "mouse" : "pen");
        ImGui::Text("Pen ID:     %u", static_cast<unsigned>(app.activePen));
        const char* kind = "unknown";
        if (app.activePen != 0) {
            switch (SDL_GetPenDeviceType(app.activePen)) {
                case SDL_PEN_DEVICE_TYPE_DIRECT:   kind = "direct (on screen)"; break;
                case SDL_PEN_DEVICE_TYPE_INDIRECT: kind = "indirect (tablet)";  break;
                default: break;
            }
        }
        // SDL exposes no pen NAME, only this. See D-015.
        ImGui::Text("Device:     %s", kind);
        ImGui::Text("Button:     %d", app.lastPenButton);

        ImGui::Separator();
        ImGui::Text("Raw pressure:        %.3f", static_cast<double>(app.lastRawPressure));
        ImGui::ProgressBar(app.lastRawPressure, ImVec2(-1.0f, 0.0f));
        ImGui::Text("Normalised pressure: %.3f", static_cast<double>(app.lastNormPressure));
        ImGui::ProgressBar(app.lastNormPressure, ImVec2(-1.0f, 0.0f));

        ImGui::Text("Position:   %.1f, %.1f", app.lastCanvasX, app.lastCanvasY);
        ImGui::Text("Tilt:       %.1f, %.1f deg",
                    static_cast<double>(app.lastTiltX), static_cast<double>(app.lastTiltY));

        ImGui::Separator();
        // US-10.5: the number that tells you whether samples are being lost.
        ImGui::Text("Motion events last frame: %d", app.motionLastFrame);
        if (app.motionLastFrame == 1 && !app.lastFromMouse)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                               "Only one per frame — samples may be being dropped.");

        ImGui::Separator();
        ImGui::TextDisabled("Active curve, with current input marked:");
        pressureCurveEditor("padcurve", app.profile.curve, 200.0f,
                            app.lastFromMouse ? -1.0f : app.pressureFilter.lastRescaled());
    }
    ImGui::End();
}

void drawShortcutEditor(App& app) {
    if (!app.showShortcuts) return;
    ImGui::SetNextWindowSize(ImVec2(420.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Keyboard and interface", &app.showShortcuts)) {
        ImGui::TextWrapped(
            "Sable has no screen-reader support, so the keyboard is the whole of "
            "its accessibility. Every action below is reachable and reassignable.");
        ImGui::Separator();

        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Interface scale", &app.uiScale, 0.75f, 2.5f, "%.2fx");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##scale")) app.uiScale = 1.0f;
        ImGui::Checkbox("Light theme", &app.lightTheme);

        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderInt("Undo memory", &app.undoBudgetMb, 16, 2048, "%d MB")) {
            applyUndoBudget(app);
        }
        ImGui::TextDisabled("Oldest steps are dropped first, and the status bar says so.");

        ImGui::Separator();
        if (ImGui::SmallButton("Restore defaults")) {
            app.shortcuts.resetToDefaults();
            app.rebinding = Action::Count;
        }
        if (app.rebinding != Action::Count)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Press a key for \"%s\"  (Escape cancels)",
                               Shortcuts::name(app.rebinding));
        else
            ImGui::TextDisabled("Click a binding to change it.");

        ImGui::Separator();
        if (ImGui::BeginTable("bindings", 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Action");
            ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
                const auto action = static_cast<Action>(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Shortcuts::name(action));
                ImGui::TableNextColumn();

                ImGui::PushID(i);
                const bool waiting = app.rebinding == action;
                const std::string label =
                    waiting ? "press a key..." : app.shortcuts.get(action).label();
                if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f)))
                    app.rebinding = waiting ? Action::Count : action;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void drawStatusBar(const App& app, float windowW, float windowH, float height) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, windowH - height));
    ImGui::SetNextWindowSize(ImVec2(windowW, height));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("##status", nullptr, kStatusFlags)) {
        ImGui::Text("%d%%   %ld deg   %d x %d%s",
                    static_cast<int>(std::lround(app.view.zoom * 100.0f)),
                    std::lround(rotationDegrees(app.view)),
                    app.doc.width, app.doc.height, app.doc.dirty ? "   *" : "");
        ImGui::SameLine();
        ImGui::SameLine();
        ImGui::TextDisabled("   %s %.1f px   %s   %s",
                            toolName(app),
                            static_cast<double>(activeBrush(const_cast<App&>(app)).size),
                            app.penSeen ? "pen" : "mouse",
                            app.doc.path.empty()
                                ? "(unsaved)"
                                : app.doc.path.filename().string().c_str());
        if (app.doc.selection.has_value() && !app.doc.selection->empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("   sel %d x %d",
                                app.doc.selection->w, app.doc.selection->h);
        }
        // #15: which backend is actually painting, always. The View menu says
        // what was ASKED for; only this says what happened, which is the
        // difference that matters when the answer is "not what you ticked".
        ImGui::SameLine();
        ImGui::TextDisabled("   %s", app.useGpu ? "GPU" : "CPU");
        // D-023: shown only when it is not the default, because a permanent
        // "8-bit" on the bar is noise on every document that never chose. When
        // it IS 16-bit it explains the halved history the undo numbers beside
        // it are about to report.
        if (app.doc.depth != sbl::ColourDepth::Bits8) {
            ImGui::SameLine();
            const std::string_view name = sbl::depthName(app.doc.depth);
            ImGui::TextDisabled("   %.*s", static_cast<int>(name.size()), name.data());
        }
        // Two memory numbers, because there are two memories. The undo budget
        // is host RAM and still counts exactly what it always counted (#12);
        // this is the other one, and leaving it off the bar would make the
        // first one look like the whole story.
        if (app.gpu != nullptr && app.useGpu) {
            ImGui::SameLine();
            ImGui::TextDisabled("   gpu %.0f MB",
                                static_cast<double>(sbl::gpuDeviceBytes(*app.gpu)) /
                                    (1024.0 * 1024.0));
        }
        // D-102 asks for a VISIBLE policy, not just a cap. Say it plainly, and
        // only once there is something to say.
        if (app.doc.undo.droppedRecords() > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                               "   %zu older undo step(s) dropped (%d MB limit)",
                               app.doc.undo.droppedRecords(), app.undoBudgetMb);
        }
        // #40 and #15. Non-modal on purpose: an import that dropped a filter
        // layer must not stand between the artist and their drawing, but it
        // must not be invisible either. One line here, the whole list on hover
        // — a .kra with six unsupported layers has six different things to say
        // and no room to say them.
        if (!app.notices.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "   %s",
                               app.notices.front().c_str());
            if (app.notices.size() > 1) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "(+%zu more)",
                                   app.notices.size() - 1);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                for (const std::string& notice : app.notices)
                    ImGui::TextUnformatted(notice.c_str());
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void drawModals(App& app) {
    if (app.openNew)       { ImGui::OpenPopup("New canvas");        app.openNew = false; }
    if (app.openDiscard)   { ImGui::OpenPopup("Unsaved changes");   app.openDiscard = false; }
    if (app.openOverwrite) { ImGui::OpenPopup("Overwrite file?");   app.openOverwrite = false; }
    if (app.openError)     { ImGui::OpenPopup("Could not export");  app.openError = false; }
    if (app.openRecovery)  { ImGui::OpenPopup("Recover unsaved work"); app.openRecovery = false; }

    if (ImGui::BeginPopupModal("Recover unsaved work", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Sable found %zu recovery file(s) from a session that did not exit "
            "cleanly. Opening one leaves your own files untouched.",
            app.offeredRecoveries.size());
        ImGui::Separator();

        for (const sbl::RecoveryEntry& entry : app.offeredRecoveries) {
            ImGui::PushID(entry.recoveryFile.string().c_str());
            const std::string label =
                entry.originalPath.empty()
                    ? std::string("(never saved)")
                    : entry.originalPath.filename().string();
            if (ImGui::Button("Open")) {
                doOpenDocument(app, entry.recoveryFile);
                // Opened from recovery, so it is NOT the artist's file — clear
                // the path so Ctrl+S cannot quietly overwrite the original.
                app.doc.path.clear();
                app.doc.dirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard")) {
                sbl::clearRecovery(entry.recoveryFile);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button("Not now", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("New canvas", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt("Width",  &app.form.width);
        ImGui::InputInt("Height", &app.form.height);
        app.form.width  = std::clamp(app.form.width,  1, 16384);
        app.form.height = std::clamp(app.form.height, 1, 16384);
        ImGui::Separator();
        int background = app.form.transparent ? 1 : 0;
        ImGui::RadioButton("White", &background, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Transparent", &background, 1);
        app.form.transparent = background == 1;

        // D-023. Here and only here: a document's depth is fixed when it is
        // made, because converting a painting's depth under the artist is a
        // destructive edit and the downward direction throws away exactly what
        // they turned it on for.
        ImGui::Separator();
        ImGui::Checkbox("16 bits per channel", &app.form.wideColour);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Smoother build-up from stacked low-opacity passes — soft "
                "shading and airbrush work.\n\nCosts double the memory per "
                "tile, so the same undo budget holds about half the history, "
                "and the GPU backend cannot paint it.\n\nCannot be changed "
                "afterwards.");
        }
        const auto depth = app.form.wideColour ? sbl::ColourDepth::Bits16
                                               : sbl::ColourDepth::Bits8;
        // The cost, in the units the status bar and the Edit menu already use,
        // rather than as a warning nobody reads.
        ImGui::TextDisabled(
            "%.0f MB per full layer at this size",
            static_cast<double>(
                static_cast<std::size_t>((app.form.width  + 255) / 256) *
                static_cast<std::size_t>((app.form.height + 255) / 256) *
                sbl::tileBytes(depth)) / (1024.0 * 1024.0));

        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(110, 0))) {
            resetDocument(app, app.form.width, app.form.height, app.form.transparent,
                          depth);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        // Cancelling leaves the current canvas untouched (US-01.3).
        if (ImGui::Button("Cancel", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Unsaved changes", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This canvas has unsaved strokes.");
        ImGui::Separator();
        if (ImGui::Button("Discard", ImVec2(110, 0))) {
            const Pending what = app.pending;
            app.pending = Pending::None;
            // The flag is NOT cleared here. Whatever replaces the document
            // brings its own (both makeDocument and loadProject start clean),
            // so clearing it early only mattered when the follow-up never
            // happened — a cancelled dialog or a path that would not load
            // would leave real unsaved strokes marked as saved.
            ImGui::CloseCurrentPopup();
            if (what == Pending::NewCanvas) app.openNew = true;
            else if (what == Pending::Open) openPending(app);
            else if (what == Pending::Import) askForImportPath(app);
            else if (what == Pending::Quit) app.running = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            app.pending = Pending::None;
            app.pendingOpenPath.clear();   // the drop was refused, not deferred
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Overwrite file?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(app.overwritePath.c_str());
        ImGui::TextUnformatted("already exists.");
        ImGui::Separator();
        if (ImGui::Button("Overwrite", ImVec2(110, 0))) {
            if (app.fileAction == App::FileAction::SaveProject)
                doSaveProject(app, app.overwritePath);
            else
                doExport(app, app.overwritePath);
            app.overwritePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            app.overwritePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Could not export", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", app.errorMessage.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/// The brush outline, showing the true painted diameter at the current zoom
/// (US-12.4).
/// Marching-ants-ish outline for the selection. A dashed rectangle would be
/// nicer; a two-tone one reads correctly over both dark and light art and is
/// four lines.
/// Rebuilds the cached boundary of a masked selection: every edge between a
/// pixel that is in and a neighbour that is not, in canvas coordinates.
///
/// Edges rather than a traced contour, because a selection may be several
/// disjoint islands with holes in them and this handles all of that without
/// knowing it did.
void rebuildAnts(App& app) {
    static const sbl::Selection kNothing;
    const bool live = app.doc.selection.has_value() && !app.doc.selection->empty();
    const sbl::Selection& s = live ? *app.doc.selection : kNothing;
    if (app.ants.from == s) return;

    app.ants.from = s;
    app.ants.segments.clear();
    if (s.mask.empty()) return;              // a rectangle draws from its corners

    for (std::int32_t y = s.y; y < s.y + s.h; ++y) {
        for (std::int32_t x = s.x; x < s.x + s.w; ++x) {
            if (!s.contains(x, y)) continue;
            const auto px = static_cast<double>(x);
            const auto py = static_cast<double>(y);
            if (!s.contains(x - 1, y)) app.ants.segments.push_back({px, py, px, py + 1});
            if (!s.contains(x + 1, y))
                app.ants.segments.push_back({px + 1, py, px + 1, py + 1});
            if (!s.contains(x, y - 1)) app.ants.segments.push_back({px, py, px + 1, py});
            if (!s.contains(x, y + 1))
                app.ants.segments.push_back({px, py + 1, px + 1, py + 1});
        }
    }
}

void drawSelectionOutline(App& app) {
    rebuildAnts(app);
    if (!app.doc.selection.has_value() || app.doc.selection->empty()) return;
    const sbl::Selection& s = *app.doc.selection;

    // Everything goes through the view transform, never screen-space
    // arithmetic: the selection is axis-aligned on the CANVAS, so it leans when
    // the canvas is turned and an upright box would sit over the wrong pixels.
    const auto at = [&](double cx, double cy) {
        return ImVec2(static_cast<float>(toScreenX(app.view, cx, cy)),
                      static_cast<float>(toScreenY(app.view, cx, cy)));
    };
    ImDrawList* draw = ImGui::GetForegroundDrawList();

    if (s.mask.empty()) {
        const ImVec2 a = at(s.x, s.y);
        const ImVec2 b = at(s.x + s.w, s.y);
        const ImVec2 c = at(s.x + s.w, s.y + s.h);
        const ImVec2 d = at(s.x, s.y + s.h);
        draw->AddQuad(a, b, c, d, IM_COL32(0, 0, 0, 200), 3.0f);
        draw->AddQuad(a, b, c, d, IM_COL32(255, 255, 255, 230), 1.0f);
        return;
    }
    for (const std::array<double, 4>& e : app.ants.segments) {
        const ImVec2 a = at(e[0], e[1]);
        const ImVec2 b = at(e[2], e[3]);
        draw->AddLine(a, b, IM_COL32(0, 0, 0, 200), 3.0f);
        draw->AddLine(a, b, IM_COL32(255, 255, 255, 230), 1.0f);
    }
}

/// The lasso loop while it is being drawn. Closed visibly, because that is
/// what releasing will do.
void drawLassoInProgress(const App& app) {
    if (!app.selecting || app.tool != Tool::Lasso || app.lassoPath.size() < 2) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    for (std::size_t i = 0; i < app.lassoPath.size(); ++i) {
        const sbl::Point& a = app.lassoPath[i];
        const sbl::Point& b = app.lassoPath[(i + 1) % app.lassoPath.size()];
        const ImVec2 pa(static_cast<float>(toScreenX(app.view, a.x, a.y)),
                        static_cast<float>(toScreenY(app.view, a.x, a.y)));
        const ImVec2 pb(static_cast<float>(toScreenX(app.view, b.x, b.y)),
                        static_cast<float>(toScreenY(app.view, b.x, b.y)));
        draw->AddLine(pa, pb, IM_COL32(0, 0, 0, 200), 3.0f);
        draw->AddLine(pa, pb, IM_COL32(255, 255, 255, 230), 1.0f);
    }
}

/// The control points of the active linework layer.
///
/// An overlay, like the selection outline and for the same reason: the handles
/// are how the curve is edited, not part of the picture, and nothing here may
/// ever reach the export. The line itself is in the layer's tiles, drawn by the
/// one compositor (#1).
void drawLineworkHandles(const App& app) {
    if (app.tool != Tool::Linework) return;
    const sbl::LineworkContent* content = LineworkTool::contentOf(app.doc);
    if (content == nullptr) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const std::optional<sbl::PointRef> held = app.linework.held();
    const std::vector<std::size_t>& selected = app.linework.selection();
    const float r = 4.0f * app.uiScale;

    for (std::size_t s = 0; s < content->strokes.size(); ++s) {
        // A selected stroke has to be tellable from an unselected one before
        // the artist commits to a transform they would otherwise have to undo.
        const bool inSelection =
            std::find(selected.begin(), selected.end(), s) != selected.end();
        for (std::size_t i = 0; i < content->strokes[s].points.size(); ++i) {
            const sbl::LinePoint& p = content->strokes[s].points[i];
            const ImVec2 at(static_cast<float>(toScreenX(app.view, p.x, p.y)),
                            static_cast<float>(toScreenY(app.view, p.x, p.y)));
            const bool grabbed = held.has_value() && held->stroke == s && held->point == i;
            // Two-tone like every other overlay, so a handle stays visible on
            // both the white of a fresh canvas and the black of the line.
            draw->AddCircleFilled(at, r + 1.0f, IM_COL32(0, 0, 0, 160));
            draw->AddCircleFilled(at, r,
                                  grabbed      ? IM_COL32(120, 220, 255, 255)
                                  : inSelection ? IM_COL32(255, 190, 80, 255)
                                                : IM_COL32(255, 255, 255, 230));
        }
    }
}

/// The ruler guides.
///
/// Everything here is in CANVAS space and goes through toScreenX/Y, the same
/// transform the art does. A guide drawn with its own screen-space arithmetic
/// looks right until the canvas is turned, and then quietly points elsewhere
/// than the ruler it is supposed to be showing.
void drawRulerGuides(const App& app) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const auto at = [&](double cx, double cy) {
        return ImVec2(static_cast<float>(toScreenX(app.view, cx, cy)),
                      static_cast<float>(toScreenY(app.view, cx, cy)));
    };
    // Two-tone like the selection outline, so a guide stays readable over both
    // dark and light art.
    const auto line = [&](ImVec2 a, ImVec2 b, ImU32 tint) {
        draw->AddLine(a, b, IM_COL32(0, 0, 0, 90), 3.0f);
        draw->AddLine(a, b, tint, 1.0f);
    };
    const auto handle = [&](ImVec2 p, ImU32 tint) {
        const float r = 6.0f * app.uiScale;
        draw->AddCircle(p, r, IM_COL32(0, 0, 0, 160), 0, 3.0f);
        draw->AddCircle(p, r, tint, 0, 1.5f);
    };

    const auto w = static_cast<double>(app.doc.width);
    const auto h = static_cast<double>(app.doc.height);

    if (app.symmetry.enabled) {
        constexpr ImU32 tint = IM_COL32(120, 220, 255, 200);
        const double cx = app.symmetry.centreX, cy = app.symmetry.centreY;
        if (app.symmetry.vertical)   line(at(cx, 0.0), at(cx, h), tint);
        if (app.symmetry.horizontal) line(at(0.0, cy), at(w, cy), tint);
        if (app.symmetry.radial > 1) {
            // Long enough to leave the canvas from any centre inside it.
            const double reach = std::hypot(w, h);
            const int n = std::min(app.symmetry.radial, sbl::SymmetryRuler::kMaxRadial);
            for (int k = 0; k < n; ++k) {
                const double a = 2.0 * 3.14159265358979323846 * k / n;
                line(at(cx, cy),
                     at(cx + reach * std::cos(a), cy + reach * std::sin(a)), tint);
            }
        }
        handle(at(cx, cy), tint);
    }

    if (!app.perspective.enabled) return;
    for (std::size_t i = 0; i < app.doc.vanishingPoints.size(); ++i) {
        const sbl::VanishingPoint& vp = app.doc.vanishingPoints[i];
        // The guide the live stroke locked onto is lit, so "which point am I
        // drawing to" is answered on the canvas rather than guessed at.
        const bool live = app.painting &&
                          app.perspective.chosen() == static_cast<int>(i);
        const ImU32 tint = !vp.enabled ? IM_COL32(150, 150, 150, 80)
                         : live        ? IM_COL32(255, 210, 90, 230)
                                       : IM_COL32(255, 150, 90, 140);

        // A fan out to points spaced evenly round the canvas edge. That reads
        // as perspective wherever the point sits, including well off-canvas,
        // and needs no special case for a point inside the picture.
        constexpr int kSpokes = 16;
        for (int k = 0; k < kSpokes; ++k) {
            const double t = 4.0 * k / kSpokes;           // 0..4 round the edge
            const double f = t - std::floor(t);
            switch (static_cast<int>(t)) {
                case 0:  line(at(vp.x, vp.y), at(w * f, 0.0), tint); break;
                case 1:  line(at(vp.x, vp.y), at(w, h * f), tint); break;
                case 2:  line(at(vp.x, vp.y), at(w * (1.0 - f), h), tint); break;
                default: line(at(vp.x, vp.y), at(0.0, h * (1.0 - f)), tint); break;
            }
        }
        handle(at(vp.x, vp.y), tint);
    }
}

void drawBrushCursor(const App& app) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    float mx = 0.0f, my = 0.0f;
    SDL_GetMouseState(&mx, &my);
    if (!overCanvas(app, mx, my)) return;

    if (!paintingTool(app)) return;
    // Zoom only: the cursor is a circle about the pointer, and a circle is the
    // one shape rotation leaves alone. It follows the view because the pointer
    // does.
    const float radius =
        activeBrush(const_cast<App&>(app)).size * 0.5f * app.view.zoom;
    if (radius < 1.0f) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddCircle(ImVec2(mx, my), radius, IM_COL32(0, 0, 0, 160), 0, 3.0f);
    draw->AddCircle(ImVec2(mx, my), radius, IM_COL32(255, 255, 255, 200), 0, 1.0f);
}

/// `present` exists for the self-test. Reading pixels back after
/// SDL_RenderPresent is undefined — the backbuffer has been handed to the
/// window system, and a Metal backend recycles drawables from a pool, so what
/// comes back is some earlier frame rather than the one just drawn. The
/// existing rotated-blit check already sidesteps this by rendering the canvas
/// itself and reading before presenting; a check that needs the FULL frame,
/// overlays included, needs the same thing from here.
void renderFrame(App& app, bool present = true) {
    // Rebuilding the style on every frame would compound the scaling, so it is
    // applied only when the artist actually changes it.
    if (app.uiScale != app.appliedScale || app.lightTheme != app.appliedLight) {
        ImGuiStyle fresh;
        if (app.lightTheme) ImGui::StyleColorsLight(&fresh);
        else                ImGui::StyleColorsDark(&fresh);
        fresh.ScaleAllSizes(app.uiScale);
        ImGui::GetStyle() = fresh;
        ImGui::GetIO().FontGlobalScale = app.uiScale;
        app.appliedScale = app.uiScale;
        app.appliedLight = app.lightTheme;
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    int w = 0, h = 0;
    SDL_GetRenderOutputSize(app.renderer, &w, &h);
    const auto windowW = static_cast<float>(w);
    const auto windowH = static_cast<float>(h);

    float menuHeight = 0.0f;
    drawMenuBar(app, menuHeight);
    const float statusHeight =
        ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

    // A host window holding the dockspace, sized to everything between the
    // menu bar and the status bar. PassthruCentralNode leaves the middle
    // transparent so the canvas we draw underneath shows through it.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x,
                                    std::max(0.0f, vp->WorkSize.y - statusHeight)));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspace = ImGui::GetID("sable.dockspace");
    if (app.resetLayout || ImGui::DockBuilderGetNode(dockspace) == nullptr) {
        buildDefaultLayout(dockspace);
        app.resetLayout = false;
    }
    ImGui::DockSpace(dockspace, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    drawToolPanel(app);
    drawColourPanel(app);
    drawLayerPanel(app);
    drawStatusBar(app, windowW, windowH, statusHeight);
    drawCalibration(app);
    drawTestPad(app);
    drawShortcutEditor(app);
    drawModals(app);

    // The canvas lives in whatever the central node has been left as, so it
    // follows the artist's docking rather than a hard-coded rectangle.
    app.viewport = SDL_FRect{0.0f, menuHeight, windowW,
                             std::max(0.0f, windowH - menuHeight - statusHeight)};
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace);
        central != nullptr && central->Size.x > 1.0f && central->Size.y > 1.0f) {
        app.viewport = SDL_FRect{central->Pos.x, central->Pos.y,
                                 central->Size.x, central->Size.y};
    }
    // Every overlay below draws canvas-space geometry into the shared
    // foreground list, and canvas space is unbounded: a ruler guide runs a
    // canvas diagonal past its centre, a vanishing point sits off-picture, a
    // selection survives a zoom that puts it off-screen. Unclipped, all of it
    // paints straight over the docked panels and out of the window. Clip once
    // here rather than guarding each drawer, so an overlay added later cannot
    // reintroduce it.
    ImDrawList* overlay = ImGui::GetForegroundDrawList();
    overlay->PushClipRect(ImVec2(app.viewport.x, app.viewport.y),
                          ImVec2(app.viewport.x + app.viewport.w,
                                 app.viewport.y + app.viewport.h), true);
    drawRulerGuides(app);
    drawSelectionOutline(app);
    drawLassoInProgress(app);
    drawLineworkHandles(app);
    drawBrushCursor(app);
    // Recolouring text is the colour panel, not a control of its own: while a
    // session is live the text simply IS the foreground colour.
    app.text.setColour(app.doc, app.foreground);
    syncTextures(app, app.text.takeChanged());
    app.text.frame(app.window, app.view, app.uiScale);   // draws the caret
    overlay->PopClipRect();

    ImGui::Render();

    if (app.lightTheme) SDL_SetRenderDrawColor(app.renderer, 205, 205, 210, 255);
    else                SDL_SetRenderDrawColor(app.renderer, 48, 48, 52, 255);
    SDL_RenderClear(app.renderer);
    app.canvas->render(app.doc, app.view, app.viewport);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app.renderer);
    if (present) SDL_RenderPresent(app.renderer);

    app.motionLastFrame = app.motionThisFrame;
    app.motionThisFrame = 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Renders a few frames and exits. Runs headless under
    // SDL_VIDEODRIVER=offscreen, which is the only automated check the SDL and
    // ImGui wiring gets — the engine has unit tests, this does not.
    const bool selfTest = argc > 1 && std::string_view(argv[1]) == "--selftest";

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // SDL can synthesise mouse events from a stylus. Left on, every pen stroke
    // is processed twice — once as pen, once as mouse — and the paint is
    // subtly doubled.
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");

    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Sable", 1280, 800, SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        SDL_Log("could not create a window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // The panel layout is the artist's, so it persists — but in the pref
    // directory, not as a stray imgui.ini beside the binary.
    static std::string iniPath;
    if (char* base = SDL_GetPrefPath("sable", "sable"); base != nullptr) {
        iniPath = std::string(base) + "layout.ini";
        SDL_free(base);
        ImGui::GetIO().IniFilename = iniPath.c_str();
    } else {
        ImGui::GetIO().IniFilename = nullptr;
    }
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // D-016
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    App app;
    app.window   = window;
    app.renderer = renderer;
    CanvasView canvas(renderer);
    app.canvas = &canvas;

    // The engine cannot ask the platform where user data lives (D-003), so
    // tell it. Without this, recovery lands in a temporary directory on
    // Windows and macOS — which is precisely what D-013 exists to prevent.
    if (char* base = SDL_GetPrefPath("sable", "sable"); base != nullptr) {
        sbl::setRecoveryDirectory(std::filesystem::path(base) / "recovery");
        SDL_free(base);
    }

    applySettings(app, loadSettings());
    // The self-test starts from the documented default whatever this machine's
    // preferences say, and switches to the GPU itself at the end.
    if (selfTest) app.useGpu = false;
    syncHsvFromColour(app);

    // D-013: restoring is an explicit user action. Offer, never auto-open.
    if (!selfTest) {
        app.offeredRecoveries = sbl::listRecoveries();
        app.openRecovery = !app.offeredRecoveries.empty();
    }

    int outW = 0, outH = 0;
    SDL_GetRenderOutputSize(renderer, &outW, &outH);
    app.viewport = SDL_FRect{leftPanel(app), 24.0f,
                             static_cast<float>(outW) - leftPanel(app) - rightPanel(app),
                             static_cast<float>(outH) - 48.0f};
    app.doc = sbl::makeDocument(kDefaultCanvas, kDefaultCanvas,
                                sbl::StraightRgba8{255, 255, 255, 255});
    applyUndoBudget(app);
    zoomToActualSize(app.view, app.doc, app.viewport);
    // After the document exists: switching backends reads the tiles back.
    applyGpuMode(app);

    // `Exec=sable %f` in the .desktop file promises a path argument opens that
    // document, which is what double-clicking a .sable does. A path that will
    // not load leaves the blank canvas above in place and shows the error
    // modal — refusing to start is never the right answer to a bad argument
    // (D-012).
    if (!selfTest && argc > 1) doOpenDocument(app, std::filesystem::path(argv[1]));

    if (selfTest) {
        // Paint a pressure-varying stroke through the real app path, then
        // export it, so a broken engine/app boundary fails here rather than in
        // someone's hands.
        app.brushes[0].stabilizerLevel = 2;
        // Both custom widgets on, so the colour wheel and the curve editor are
        // exercised too — they are the only ImGui code here that places its own
        // vertices, and the only code a headless run can still meaningfully hit.
        app.showTestPad     = true;
        app.showCalibration = true;
        app.showShortcuts   = true;
        beginPaint(app);
        for (double t = 0.0; t <= 1.0; t += 0.02) {
            const double cx = 100.0 + t * 400.0;
            const double cy = 100.0 + t * 250.0;
            sbl::InputSample sample =
                mouseSample(app, toScreenX(app.view, cx, cy),
                                 toScreenY(app.view, cx, cy));
            sample.fromMouse = false;
            sample.pressure = static_cast<float>(1.0 - std::abs(0.5 - t) * 2.0);
            paintWith(app, sample);
        }
        endPaint(app);

        // Exercise the Milestone 3 paths too: a second layer with a blend
        // mode, a bucket fill, a selection, and a project save/load round trip.
        pushLayerAction(app, sbl::addLayerAbove(app.doc, app.doc.activeLayer, "Shading"));
        app.doc.layerById(app.doc.activeLayer)->blend = sbl::BlendMode::Multiply;
        app.foreground = sbl::StraightRgba8{60, 120, 220, 255};
        app.doc.selection = sbl::Selection{200, 200, 300, 300};
        doFill(app, toScreenX(app.view, 300.0, 300.0),
                    toScreenY(app.view, 300.0, 300.0));
        app.doc.selection.reset();

        // Rulers, driven through the same path a real stroke takes. Two things
        // a unit test cannot see: the mirror appears while the stroke is being
        // drawn rather than at pen-up, and however many dabs symmetry
        // multiplied it into, it is still ONE undo step.
        {
            const std::size_t undoBefore = app.doc.undo.size();
            app.foreground = sbl::StraightRgba8{20, 20, 20, 255};
            app.symmetry.enabled  = true;
            app.symmetry.vertical = true;
            app.symmetry.radial   = 1;
            centreSymmetry(app);
            setVanishingPoints(app, 2);
            app.perspective.enabled = true;

            beginPaint(app);
            for (int i = 0; i <= 40; ++i) {
                // Below everything already drawn, so the only thing in this
                // band of the canvas is the stroke under test.
                const double cx = 120.0 + i * 9.0;
                const double cy = 700.0 + (i % 2 == 0 ? 6.0 : -6.0);
                paintWith(app, mouseSample(app, toScreenX(app.view, cx, cy),
                                                toScreenY(app.view, cx, cy)));
            }
            // Counted BEFORE endPaint: "live, not on stroke end" is the whole
            // acceptance criterion, and only this ordering tests it.
            // Dark, not merely opaque: the background is opaque white, so an
            // alpha test here would pass without a single dab being painted.
            const auto painted = [&](int x, int y) {
                return sbl::pickColour(app.doc, x, y).r < 128;
            };
            int mirroredDuringStroke = 0;
            for (int x = 520; x < 900; x += 7)
                for (int y = 505; y < 780; y += 7)
                    if (painted(x, y)) ++mirroredDuringStroke;
            endPaint(app);

            if (mirroredDuringStroke == 0) {
                SDL_Log("selftest FAILED: symmetry painted nothing on the far side "
                        "while the stroke was live");
                return 1;
            }
            if (app.doc.undo.size() != undoBefore + 1) {
                SDL_Log("selftest FAILED: a mirrored stroke pushed %zu undo steps",
                        app.doc.undo.size() - undoBefore);
                return 1;
            }

            // Mirrored about x = 512, so pixel x pairs with 1023 - x.
            int probes = 0;
            for (int x = 120; x < 500; x += 7) {
                for (int y = 505; y < 780; y += 7) {
                    const sbl::StraightRgba8 a = sbl::pickColour(app.doc, x, y);
                    const sbl::StraightRgba8 b = sbl::pickColour(app.doc, 1023 - x, y);
                    if (a.a != b.a || a.r != b.r) {
                        SDL_Log("selftest FAILED: %d,%d is not the mirror of %d,%d",
                                x, y, 1023 - x, y);
                        return 1;
                    }
                    if (painted(x, y)) ++probes;
                }
            }
            if (probes == 0) {
                SDL_Log("selftest FAILED: the mirrored stroke painted nothing");
                return 1;
            }
            SDL_Log("selftest: symmetry mirrors live over %d probes, one undo step",
                    probes);

            // Off again, positions kept — the toggle must not cost the artist
            // the guides they placed.
            app.symmetry.enabled    = false;
            app.perspective.enabled = false;
            if (app.doc.vanishingPoints.size() != 2 ||
                app.symmetry.centreX != 512.0) {
                SDL_Log("selftest FAILED: switching a ruler off moved its guides");
                return 1;
            }
            app.perspective.enabled = true;   // left on for the frames below
        }

        // Lasso and magic wand (#18), driven through the same combine path the
        // pointer uses. Left in place afterwards, so the frames below draw a
        // NON-rectangular outline through the rotated view transform and the
        // project round trip carries a real coverage mask.
        {
            const std::vector<sbl::Point> triangle{
                {700.0, 150.0}, {950.0, 150.0}, {825.0, 380.0}};
            app.selectBase.reset();
            app.dragMode = sbl::SelectMode::Replace;
            commitSelection(app, sbl::lassoSelection(triangle, app.doc.width,
                                                     app.doc.height));
            if (!app.doc.selection.has_value() || app.doc.selection->mask.empty()) {
                SDL_Log("selftest FAILED: the lasso produced no mask");
                return 1;
            }

            app.foreground = sbl::StraightRgba8{220, 40, 40, 255};
            sbl::UndoRecord rec =
                sbl::fillSelection(app.doc, app.doc.activeLayer, app.foreground);
            app.canvas->releaseAll();
            app.doc.undo.push(std::move(rec));

            const auto red = [&](int x, int y) {
                const sbl::StraightRgba8 c = sbl::pickColour(app.doc, x, y);
                return c.r > 150 && c.g < 110;
            };
            // Inside the loop, and a corner of its bounding box that the loop
            // does not enclose — a fill that ignored the mask would paint both.
            if (!red(825, 200) || red(705, 375)) {
                SDL_Log("selftest FAILED: the fill did not follow the lasso");
                return 1;
            }

            const sbl::Selection wand = sbl::magicWandSelection(app.doc, 825, 200, 8);
            const sbl::Selection& lasso = *app.doc.selection;
            if (wand.empty() || std::abs(wand.x - lasso.x) > 4 ||
                std::abs(wand.w - lasso.w) > 8) {
                SDL_Log("selftest FAILED: the wand found %d,%d %dx%d where the lasso "
                        "is %d,%d %dx%d", wand.x, wand.y, wand.w, wand.h,
                        lasso.x, lasso.y, lasso.w, lasso.h);
                return 1;
            }
            // Taking the wand's own region back out of the lasso must leave
            // little more than the anti-aliased rim.
            const sbl::Selection remainder =
                sbl::combineSelections(lasso, wand, sbl::SelectMode::Subtract);
            if (remainder.contains(825, 200)) {
                SDL_Log("selftest FAILED: subtract left the middle selected");
                return 1;
            }
            SDL_Log("selftest: lasso clips a fill, wand agrees with it, subtract works");

            app.doc.undo.undo(app.doc);       // put the canvas back
            app.canvas->releaseAll();
            app.foreground = sbl::StraightRgba8{0, 0, 0, 255};
        }

        // Rotation, checked where a unit test cannot reach: turning the view
        // must move no pixels (US-05.5's guarantee, extended to rotate), and
        // the app's own screen->canvas path must still land on the pixel the
        // artist is pointing at. Left turned on, so the frames below also
        // exercise the rotated blit and the turned outline.
        {
            const bool dirtyBefore = app.doc.dirty;
            const std::size_t undoBefore    = app.doc.undo.size();
            const std::size_t uploadsBefore = app.canvas->uploadCount();
            rotateView(app, kRotateStep * 2.0);
            if (app.doc.dirty != dirtyBefore ||
                app.doc.undo.size() != undoBefore ||
                app.canvas->uploadCount() != uploadsBefore) {
                SDL_Log("selftest FAILED: rotating the view altered the document");
                return 1;
            }
            const sbl::InputSample probe =
                mouseSample(app, toScreenX(app.view, 400.0, 250.0),
                                 toScreenY(app.view, 400.0, 250.0));
            if (std::abs(probe.x - 400.0) > 1e-6 || std::abs(probe.y - 250.0) > 1e-6) {
                SDL_Log("selftest FAILED: rotated screen->canvas lands at %.6f, %.6f",
                        probe.x, probe.y);
                return 1;
            }
            SDL_Log("selftest: rotation at %ld deg alters no pixels",
                    std::lround(rotationDegrees(app.view)));
        }

        for (int frame = 0; frame < 3; ++frame) renderFrame(app);

        // The blit has to agree with the transform. SDL turns each tile about
        // its own corner, and a sign error there would put the picture
        // somewhere the screen->canvas maths says it is not — with every unit
        // test still green. One pixel read back off the rotated frame catches
        // it: the probe is off-centre and inside the filled region, so the
        // mirrored point it would land on is a different colour.
        {
            SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
            SDL_RenderClear(app.renderer);
            app.canvas->render(app.doc, app.view, app.viewport);

            constexpr int probe = 350;
            const double centreX = probe + 0.5, centreY = probe + 0.5;
            const SDL_Rect one{
                static_cast<int>(std::lround(toScreenX(app.view, centreX, centreY))),
                static_cast<int>(std::lround(toScreenY(app.view, centreX, centreY))),
                1, 1};
            SDL_Surface* shot = SDL_RenderReadPixels(app.renderer, &one);
            Uint8 r = 0, g = 0, b = 0, a = 0;
            // A renderer that cannot read back says nothing either way, so it
            // is not a failure — but a mismatch is.
            if (shot != nullptr && SDL_ReadSurfacePixel(shot, 0, 0, &r, &g, &b, &a)) {
                const std::vector<sbl::PremulRgba8> want =
                    sbl::compositeRect(app.doc, probe, probe, 1, 1);
                const int dr = std::abs(int{r} - int{want[0].r});
                const int dg = std::abs(int{g} - int{want[0].g});
                const int db = std::abs(int{b} - int{want[0].b});
                if (dr > 8 || dg > 8 || db > 8) {
                    SDL_Log("selftest FAILED: rotated blit shows %d,%d,%d where the "
                            "transform says %d,%d,%d", r, g, b,
                            want[0].r, want[0].g, want[0].b);
                    SDL_DestroySurface(shot);
                    return 1;
                }
                SDL_Log("selftest: rotated blit lands on the pixel the transform names");
            }
            SDL_DestroySurface(shot);
        }

        // Overlays must stay inside the canvas viewport. Ruler guides are
        // canvas-space lines a full diagonal long, so unclipped they run
        // straight across the docked panels and out of the window — which is
        // exactly what shipped in v2.0.0.
        //
        // Moving the symmetry centre rather than switching the ruler off keeps
        // the panels pixel-identical between the two frames (the tool panel
        // shows the ruler's state, not its position), so anything that differs
        // outside the viewport is an overlay that escaped.
        //
        // The two spaces here are NOT the same one. `app.viewport` comes from
        // the dockspace's central node, so it is in ImGui's coordinates, which
        // are window points; SDL_RenderReadPixels hands back backing pixels.
        // On a 1x display they coincide, which is why this passed on Linux and
        // failed the macOS build with three quarters of the frame counted as
        // outside. The ratio is measured rather than assumed, so it stays
        // right on whatever the runner turns out to be.
        {
            const auto shoot = [&](double cx, double cy) -> SDL_Surface* {
                app.symmetry.centreX = cx;
                app.symmetry.centreY = cy;
                renderFrame(app, false);   // read the frame just drawn
                return SDL_RenderReadPixels(app.renderer, nullptr);
            };
            app.symmetry.enabled = true;
            app.symmetry.radial  = 12;   // a fan, so the guides cross everything
            // Switching the ruler on adds its widgets to the tool panel, and
            // ImGui settles a layout change on the following frame. Without
            // this the first capture is a frame behind the second and the
            // panel itself differs.
            for (int warm = 0; warm < 3; ++warm) renderFrame(app);
            SDL_Surface* shotA = shoot(80.0, 80.0);
            SDL_Surface* shotB  = shoot(static_cast<double>(app.doc.width) - 80.0,
                                      static_cast<double>(app.doc.height) - 80.0);
            if (shotA != nullptr && shotB != nullptr &&
                shotA->w == shotB->w && shotA->h == shotB->h) {
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                const float sx = display.x > 0.0f
                               ? static_cast<float>(shotA->w) / display.x : 1.0f;
                const float sy = display.y > 0.0f
                               ? static_cast<float>(shotA->h) / display.y : 1.0f;
                const float vx0 = app.viewport.x * sx;
                const float vy0 = app.viewport.y * sy;
                const float vx1 = (app.viewport.x + app.viewport.w) * sx;
                const float vy1 = (app.viewport.y + app.viewport.h) * sy;
                int escaped = 0, within = 0;
                for (int y = 0; y < shotA->h; ++y) {
                    for (int x = 0; x < shotA->w; ++x) {
                        const bool inside =
                            static_cast<float>(x) >= vx0 && static_cast<float>(y) >= vy0 &&
                            static_cast<float>(x) <  vx1 && static_cast<float>(y) <  vy1;
                        Uint8 ar = 0, ag = 0, ab = 0, aa = 0, br = 0, bg = 0, bb = 0, ba = 0;
                        if (!SDL_ReadSurfacePixel(shotA, x, y, &ar, &ag, &ab, &aa)) continue;
                        if (!SDL_ReadSurfacePixel(shotB,  x, y, &br, &bg, &bb, &ba)) continue;
                        if (ar == br && ag == bg && ab == bb) continue;
                        if (inside) ++within; else ++escaped;
                    }
                }
                if (escaped > 0) {
                    // The geometry goes with the failure: the first thing to
                    // rule out is the two coordinate spaces disagreeing, and
                    // that is unanswerable from a bare count.
                    SDL_Log("selftest FAILED: %d pixels outside the canvas viewport "
                            "changed when the symmetry centre moved — an overlay is "
                            "drawing over the panels "
                            "(%d changed INSIDE it, which is expected and is the "
                            "control: if that is also near zero the readback is not "
                            "the frame that was drawn. readback %dx%d, display "
                            "%.0fx%.0f, scale %.2fx%.2f, viewport %.1f,%.1f %.1fx%.1f)",
                            escaped, within, shotA->w, shotA->h, display.x, display.y, sx, sy,
                            app.viewport.x, app.viewport.y,
                            app.viewport.w, app.viewport.h);
                    SDL_DestroySurface(shotA);
                    SDL_DestroySurface(shotB);
                    return 1;
                }
                SDL_Log("selftest: ruler guides stay inside the canvas viewport "
                        "(%d pixels changed inside it, readback %dx%d at %.2fx%.2f)",
                        within, shotA->w, shotA->h, sx, sy);
            }
            SDL_DestroySurface(shotA);
            SDL_DestroySurface(shotB);
            app.symmetry.radial = 1;
            centreSymmetry(app);
        }

        const auto project = std::filesystem::temp_directory_path() / "sable_selftest.sable";
        std::error_code ec;
        std::filesystem::remove(project, ec);
        doSaveProject(app, project);
        // Through the registry, which is how the app opens files now — and the
        // native format is the one case that keeps a path to save back to.
        const auto reloaded = sbl::importDocument(project);
        if (!reloaded.has_value() || reloaded->layers.size() != 2 ||
            reloaded->layers[1].blend != sbl::BlendMode::Multiply ||
            reloaded->path != project) {
            SDL_Log("selftest FAILED: project round trip");
            return 1;
        }
        // The lasso selection left in place above, back off disk with its
        // coverage mask intact (#18).
        if (!reloaded->selection.has_value() || reloaded->selection->mask.empty() ||
            reloaded->selection->mask != app.doc.selection->mask) {
            SDL_Log("selftest FAILED: the selection mask did not survive the file");
            return 1;
        }
        SDL_Log("selftest: project round trip ok, %zu layers, %zu bytes",
                reloaded->layers.size(),
                static_cast<std::size_t>(std::filesystem::file_size(project)));

        // The argument and drag-and-drop entry points, which have no unit test
        // because they live above the engine boundary. A path that cannot be
        // loaded must leave the document exactly as it was, and a drop onto a
        // dirty canvas must prompt rather than discard the artist's strokes.
        doOpenDocument(app, std::filesystem::path("/sable-selftest/no-such.sable"));
        if (!app.openError || app.doc.layers.size() != 2) {
            SDL_Log("selftest FAILED: a bad path did not fall back to the error modal");
            return 1;
        }
        app.openError = false;
        app.errorMessage.clear();

        app.doc.dirty = true;
        requestPending(app, Pending::Open, "/sable-selftest/dropped.sable");
        if (!app.openDiscard || app.pendingOpenPath.empty()) {
            SDL_Log("selftest FAILED: a drop over unsaved work did not prompt");
            return 1;
        }
        app.openDiscard = false;
        app.pending     = Pending::None;
        app.pendingOpenPath.clear();
        app.doc.dirty   = false;
        SDL_Log("selftest: bad path recovers, drop over unsaved work prompts");

        // The text tool, driven through the events a keyboard and an input
        // method actually produce. Three things no engine test can see: that
        // SDL's text events reach the canvas at all, that a half-finished
        // composition is drawn and then REPLACED rather than left behind, and
        // that a whole editing session collapses to one undo step.
        //
        // This is not a test of an input method. It is a test that our side of
        // the contract is wired up; see the pull request for what was and was
        // not tried with a real IME.
        bool textTested = false;
        if (!sbl::systemFonts().empty()) {
            const std::size_t layersBefore = app.doc.layers.size();
            const std::size_t undoBefore   = app.doc.undo.size();

            app.foreground = sbl::StraightRgba8{20, 20, 20, 255};
            app.tool = Tool::Text;
            placeText(app, toScreenX(app.view, 60.0, 900.0),
                           toScreenY(app.view, 60.0, 900.0));

            // A font that actually carries the characters below, where there is
            // one. Without this the kanji come out as the empty box every
            // Latin-only font draws, and the check would pass on tofu.
            bool cjkFont = false;
            for (const sbl::FontEntry& entry : sbl::systemFonts()) {
                auto face = sbl::FontFace::load(entry.path);
                if (!face.has_value() || !face->hasGlyph(0x6F22)) continue;
                cjkFont = app.text.useFont(app.doc, entry.path);
                if (cjkFont) SDL_Log("selftest: typing 漢字 in %s", entry.name.c_str());
                break;
            }
            if (!cjkFont)
                SDL_Log("selftest: no CJK font here, the kanji below will be boxes");

            // Whether SDL accepted the request is the one half of the IME
            // question a self-test can answer: an input method can only reach
            // us through a window that has text input switched on. Whether a
            // real input method then composes into it is a question for a human
            // with one installed, and the offscreen driver has no answer at all.
            SDL_Log("selftest: SDL text input active under \"%s\": %s",
                    SDL_GetCurrentVideoDriver(),
                    SDL_TextInputActive(app.window) ? "yes" : "no");

            const auto darkPixels = [&] {
                int n = 0;
                for (int x = 40; x < 900; x += 3)
                    for (int y = 830; y < 940; y += 3)
                        if (sbl::pickColour(app.doc, x, y).r < 128) ++n;
                return n;
            };
            if (darkPixels() != 0) {
                SDL_Log("selftest FAILED: the text band was not empty to begin with");
                return 1;
            }

            // SDL_EVENT_TEXT_EDITING: the characters an input method is still
            // choosing. D-002 named this as the reason not to use an ImGui
            // field, so it is the first thing exercised.
            SDL_Event editing{};
            editing.type        = SDL_EVENT_TEXT_EDITING;
            editing.edit.text   = "\xE3\x81\x8B\xE3\x82\x93";   // かん, mid-composition
            editing.edit.start  = 2;
            editing.edit.length = 0;
            app.text.handleEvent(editing, app.doc, app.window);
            const int duringComposition = darkPixels();
            if (duringComposition == 0) {
                SDL_Log("selftest FAILED: a composition in progress drew nothing");
                return 1;
            }
            // Composition is NOT the document. It is on the canvas so the
            // artist can see it, and it must not be in the file.
            const sbl::Layer* textLayer = app.doc.active();
            if (textLayer == nullptr || !textLayer->text.has_value() ||
                !textLayer->text->utf8.empty()) {
                SDL_Log("selftest FAILED: an unfinished composition reached the document");
                return 1;
            }

            // The input method commits. The preedit must be replaced, not
            // added to — the classic doubled-character bug.
            SDL_Event input{};
            input.type      = SDL_EVENT_TEXT_INPUT;
            input.text.text = "\xE6\xBC\xA2\xE5\xAD\x97";       // 漢字
            app.text.handleEvent(input, app.doc, app.window);

            SDL_Event key{};
            key.type = SDL_EVENT_KEY_DOWN;
            key.key.key  = SDLK_B;      // a bare letter types, it does not
            key.key.down = true;        // switch to the brush
            app.text.handleEvent(key, app.doc, app.window);
            if (app.tool != Tool::Text) {
                SDL_Log("selftest FAILED: typing a letter changed the tool");
                return 1;
            }
            input.text.text = "b";
            app.text.handleEvent(input, app.doc, app.window);

            const sbl::Layer* typed = app.doc.active();
            if (typed == nullptr || typed->kind != sbl::LayerKind::Text ||
                !typed->text.has_value() ||
                typed->text->utf8 != "\xE6\xBC\xA2\xE5\xAD\x97" "b") {
                SDL_Log("selftest FAILED: the committed text is not what was typed");
                return 1;
            }
            const int afterCommit = darkPixels();
            if (afterCommit == 0) {
                SDL_Log("selftest FAILED: committed text drew nothing");
                return 1;
            }

            key.key.key = SDLK_ESCAPE;
            app.text.handleEvent(key, app.doc, app.window);
            if (app.text.active()) {
                SDL_Log("selftest FAILED: Escape did not finish the text");
                return 1;
            }

            // One layer, and one step for the words and the glyphs together —
            // plus the step that created the layer.
            if (app.doc.layers.size() != layersBefore + 1 ||
                app.doc.undo.size() != undoBefore + 2) {
                SDL_Log("selftest FAILED: a text session left %zu layers and %zu steps",
                        app.doc.layers.size() - layersBefore,
                        app.doc.undo.size() - undoBefore);
                return 1;
            }

            doUndo(app);
            // The words go back with the pixels: the layer is as blank as it
            // was the moment before the first character arrived.
            const sbl::Layer* reverted = app.doc.active();
            if (darkPixels() != 0 || reverted == nullptr ||
                (reverted->text.has_value() && !reverted->text->utf8.empty())) {
                SDL_Log("selftest FAILED: undoing the text left %d pixels and \"%s\"",
                        darkPixels(),
                        reverted != nullptr && reverted->text.has_value()
                            ? reverted->text->utf8.c_str() : "");
                return 1;
            }
            doRedo(app);
            if (darkPixels() != afterCommit) {
                SDL_Log("selftest FAILED: redo drew %d pixels where undo removed %d",
                        darkPixels(), afterCommit);
                return 1;
            }
            SDL_Log("selftest: text composes, commits and undoes as one step "
                    "(%d pixels)", afterCommit);
            textTested = true;
        } else {
            SDL_Log("selftest: no fonts on this machine, text tool not exercised");
        }

        // --- linework (#17). Driven through the tool rather than through
        // synthetic pen events: where the pointer is belongs to SDL, but what a
        // press MEANS is the tool's, and that is the half worth checking.
        {
            const std::size_t layersBefore = app.doc.layers.size();
            const std::size_t undoBefore   = app.doc.undo.size();
            // The linework layer's OWN pixels, not the composite: the canvas
            // already carries the strokes and the fill this self-test painted
            // earlier, and a probe that could be answered by those would prove
            // nothing about the line.
            const auto inkAt = [&](std::int32_t x, std::int32_t y) {
                const sbl::Layer* on = app.doc.active();
                if (on == nullptr) return false;
                const sbl::TileKey key{sbl::tileIndex(x), sbl::tileIndex(y)};
                const sbl::Tile* tile = on->find(key);
                return tile != nullptr &&
                       tile->pixel(x - key.first * sbl::TILE_SIZE,
                                   y - key.second * sbl::TILE_SIZE).a > 0;
            };

            app.tool = Tool::Linework;
            app.foreground = sbl::StraightRgba8{0, 0, 0, 255};
            app.linework.width = 8.0f;
            app.linework.colour = app.foreground;
            app.linework.press(app.doc, 100.0, 500.0, 1.0f, LineworkAction::Draw, 9.0);
            for (double x = 120.0; x <= 400.0; x += 20.0)
                app.linework.drag(app.doc, x, 500.0, 1.0f);
            app.linework.release(app.doc);
            syncTextures(app, app.linework.takeChanged());

            if (!inkAt(300, 500)) {
                SDL_Log("selftest FAILED: a linework stroke drew nothing");
                return 1;
            }
            // One layer, and two steps: the layer, then the whole gesture.
            if (app.doc.layers.size() != layersBefore + 1 ||
                app.doc.undo.size() != undoBefore + 2) {
                SDL_Log("selftest FAILED: linework left %zu layers and %zu steps",
                        app.doc.layers.size() - layersBefore,
                        app.doc.undo.size() - undoBefore);
                return 1;
            }

            // Re-shaping: grab the far end and drag it away. The line has to
            // follow, and leave where it was.
            app.linework.press(app.doc, 400.0, 500.0, 1.0f, LineworkAction::Draw, 12.0);
            if (!app.linework.busy()) {
                SDL_Log("selftest FAILED: no control point where one was drawn");
                return 1;
            }
            app.linework.drag(app.doc, 400.0, 640.0, 1.0f);
            app.linework.release(app.doc);
            syncTextures(app, app.linework.takeChanged());
            if (!inkAt(398, 640) || inkAt(400, 500)) {
                SDL_Log("selftest FAILED: the line did not follow its control point");
                return 1;
            }

            // And the whole re-shape undoes as one step, back to the line the
            // first gesture drew.
            doUndo(app);
            if (!inkAt(300, 500) || inkAt(398, 640)) {
                SDL_Log("selftest FAILED: undoing the drag left the wrong line");
                return 1;
            }
            doRedo(app);

            const sbl::Layer* line = app.doc.active();
            if (line == nullptr || !line->linework.has_value() ||
                line->linework->strokes.size() != 1) {
                SDL_Log("selftest FAILED: the curves did not survive the gesture");
                return 1;
            }
            SDL_Log("selftest: linework draws, re-shapes and undoes as one step "
                    "(%zu control points)", line->linework->strokes[0].points.size());

            // #51: a whole stroke is grabbed by the line rather than a handle,
            // and moves as one. The engine's geometry is tested headlessly; what
            // only a running app can show is that the tool's selection survives
            // press, drag and release, and leaves one undo step behind.
            app.linework.selectMode = true;
            const std::size_t beforeMove = app.doc.undo.size();
            app.linework.press(app.doc, 200.0, 500.0, 1.0f, LineworkAction::Select, 12.0);
            if (app.linework.selection().size() != 1) {
                SDL_Log("selftest FAILED: clicking a line selected %zu strokes",
                        app.linework.selection().size());
                return 1;
            }
            app.linework.drag(app.doc, 200.0, 300.0, 1.0f);
            app.linework.release(app.doc);
            syncTextures(app, app.linework.takeChanged());
            if (!inkAt(300, 300) || inkAt(300, 500)) {
                SDL_Log("selftest FAILED: the whole stroke did not move");
                return 1;
            }
            if (app.doc.undo.size() != beforeMove + 1) {
                SDL_Log("selftest FAILED: moving a stroke left %zu undo steps",
                        app.doc.undo.size() - beforeMove);
                return 1;
            }
            doUndo(app);
            if (!inkAt(300, 500)) {
                SDL_Log("selftest FAILED: undoing the move left the wrong line");
                return 1;
            }
            app.linework.selectMode = false;
            SDL_Log("selftest: a whole linework stroke selects, moves and undoes");
        }

        // --- gradient (#49). The engine's ramp has its own tests; what is only
        // testable here is the drag: several previews must collapse into ONE
        // undo step, and the last one must be the one left on the canvas.
        {
            sbl::Layer& onto = app.doc.addLayer("gradient");
            app.doc.activeLayer = onto.id;
            // The lasso check above left a selection, and clipping to it is the
            // engine's job and already tested there. This is about the drag.
            app.doc.selection.reset();
            app.tool = Tool::Gradient;
            app.gradientShape = sbl::GradientShape::Linear;
            app.gradientToTransparent = false;
            app.foreground = sbl::StraightRgba8{0, 0, 0, 255};
            app.background = sbl::StraightRgba8{255, 255, 255, 255};

            const std::size_t undoBefore = app.doc.undo.size();
            const auto screen = [&](double cx, double cy) {
                return std::pair{toScreenX(app.view, cx, cy), toScreenY(app.view, cx, cy)};
            };
            const auto [ax, ay] = screen(0.0, 0.0);
            beginGradient(app, ax, ay);
            // Three previews, the last of which is the drag the artist meant.
            for (const double end : {200.0, 600.0, 1000.0}) {
                const auto [ex, ey] = screen(end, 0.0);
                previewGradient(app, ex, ey);
            }
            const auto [fx, fy] = screen(1000.0, 0.0);
            endGradient(app, fx, fy);

            const auto redAt = [&](std::int32_t x, std::int32_t y) {
                const sbl::TileKey key{sbl::tileIndex(x), sbl::tileIndex(y)};
                const sbl::Tile* tile = onto.find(key);
                if (tile == nullptr) return -1;
                return static_cast<int>(sbl::narrowChannel(
                    tile->pixel(x - key.first * sbl::TILE_SIZE,
                                y - key.second * sbl::TILE_SIZE).r));
            };
            if (app.doc.undo.size() != undoBefore + 1) {
                SDL_Log("selftest FAILED: a gradient drag left %zu undo steps",
                        app.doc.undo.size() - undoBefore);
                return 1;
            }
            // Halfway along the LAST axis, not the first: a preview that was
            // not taken back would have left the 200 px ramp under this one.
            const int half = redAt(500, 20);
            if (half < 100 || half > 155) {
                SDL_Log("selftest FAILED: gradient midpoint reads %d, not mid-ramp", half);
                return 1;
            }
            doUndo(app);
            if (redAt(500, 20) != -1) {
                SDL_Log("selftest FAILED: undoing the gradient left pixels behind");
                return 1;
            }
            doRedo(app);
            SDL_Log("selftest: a gradient drag is one undo step (midpoint %d)", half);
        }

        // Drive one background recovery to completion. This is the only path
        // that hands document data to another thread, so it is the only thing
        // ThreadSanitizer has to look at — and it is worth nothing to CI unless
        // the self-test actually triggers it.
        // Recovery only runs for a document with unsaved changes, and the
        // project save just above cleared that flag.
        app.doc.dirty = true;
        app.lastAutosaveMs = 0;
        maybeAutosave(app, kAutosaveIntervalMs + 1);
        if (app.autosaveWorker.joinable()) app.autosaveWorker.join();
        {
            const std::lock_guard lock(app.recoveryMutex);
            if (app.lastRecoveryFile.empty()) {
                SDL_Log("selftest FAILED: no recovery file was written");
                return 1;
            }
            SDL_Log("selftest: recovery written to %s", app.lastRecoveryFile.c_str());
            // Clean up after ourselves — a self-test must not leave the next
            // real launch offering to recover work that never existed.
            sbl::clearRecovery(app.lastRecoveryFile);
            app.lastRecoveryFile.clear();
        }

        const auto out = std::filesystem::temp_directory_path() / "sable_selftest.png";
        const auto result = sbl::exportPng(app.doc, out);
        SDL_Log("selftest: %zu undo steps, %zu tiles, %zu uploads, export %s",
                app.doc.undo.size(), app.doc.active()->tiles.size(),
                app.canvas->uploadCount(),
                result.has_value() ? out.string().c_str()
                                   : result.error().detail.c_str());
        app.running = false;
        // Stroke, new layer, fill, and the mirrored stroke — each exactly one
        // undoable step, however many dabs it took. Plus the text layer and its
        // one text step, on a machine that has a font to draw them with.
        // Plus the linework layer and its three steps — the layer, the stroke,
        // and the re-shape — which need no font and so are not conditional.
        // Plus the gradient's layer and its ONE step: three previews and a
        // release, and the whole drag is a single entry (D-030).
        const std::size_t expectedUndo   = (textTested ? 6u : 4u) + 3u + 1u;
        const std::size_t expectedLayers = (textTested ? 3u : 2u) + 1u + 1u;
        if (!result.has_value() || app.doc.undo.size() != expectedUndo ||
            app.doc.layers.size() != expectedLayers ||
            app.doc.active()->tiles.empty()) {
            SDL_Log("selftest FAILED");
            return 1;
        }
        // The GPU backend through the real application path, if this machine
        // has one. In CI it almost never does, and "no GPU" is a pass — the
        // engine tests are where the two backends are compared pixel for
        // pixel. What this checks is the wiring: that flicking the switch
        // mid-session repaints the same canvas rather than a different one.
        const std::vector<sbl::StraightRgba8> onCpu = sbl::flatten(app.doc);
        const std::size_t undoBefore   = app.doc.undo.size();
        const std::size_t layersBefore = app.doc.layers.size();

        // #15 asks that a switch preserve the stroke in progress. It does so by
        // refusing to happen: the backend is holding a batch of dabs for the
        // layer being painted, and swapping it out would drop them. The menu
        // item is disabled mid-stroke, so this guard is the only thing that can
        // still be checked — and a regression here loses pixels silently.
        beginPaint(app);
        app.useGpu = true;
        applyGpuMode(app);
        if (&sbl::paintBackend() != static_cast<sbl::PaintBackend*>(&sbl::cpuBackend())) {
            SDL_Log("selftest FAILED: the paint backend changed mid-stroke");
            return 1;
        }
        endPaint(app);

        applyGpuMode(app);
        if (app.useGpu) {
            const std::vector<sbl::StraightRgba8> onGpu = sbl::flatten(app.doc);
            int worst = 0;
            for (std::size_t i = 0; i < onCpu.size() && i < onGpu.size(); ++i)
                worst = std::max({worst, std::abs(onCpu[i].r - onGpu[i].r),
                                  std::abs(onCpu[i].g - onGpu[i].g),
                                  std::abs(onCpu[i].b - onGpu[i].b),
                                  std::abs(onCpu[i].a - onGpu[i].a)});
            renderFrame(app);                     // one frame with tiles in VRAM
            app.useGpu = false;
            applyGpuMode(app);
            if (worst > 1 || onGpu.size() != onCpu.size()) {
                SDL_Log("selftest FAILED: the GPU backend differs by %d levels", worst);
                return 1;
            }
            // Back on the CPU, with the tiles read out of VRAM: the document
            // and its history have to be exactly what they were before the
            // round trip, or a switch costs the artist work.
            if (app.doc.undo.size() != undoBefore ||
                app.doc.layers.size() != layersBefore ||
                sbl::flatten(app.doc) != onCpu) {
                SDL_Log("selftest FAILED: switching backends changed the document");
                return 1;
            }
            SDL_Log("selftest: %s, agrees with the CPU within %d level(s)",
                    app.gpuWhy.c_str(), worst);
        } else {
            SDL_Log("selftest: no GPU backend here (%s); CPU only",
                    app.gpuWhy.c_str());
        }

        // --- #21: a 16-bit document, through the application rather than the
        // engine. The unit tests cover the maths; this covers the wiring the
        // artist actually touches — the New-canvas checkbox, the paint path,
        // Ctrl+S and reopening. Last, because it replaces the document every
        // phase above was counting.
        {
            // Asked for, so that the decline is what turns it off rather than
            // it having happened to be off already.
            app.useGpu = true;
            resetDocument(app, 512, 512, false, sbl::ColourDepth::Bits16);
            if (app.doc.depth != sbl::ColourDepth::Bits16) {
                SDL_Log("selftest FAILED: the new canvas is not 16-bit");
                return 1;
            }
            if (app.useGpu) {
                SDL_Log("selftest FAILED: the GPU stayed on for a 16-bit document");
                return 1;
            }

            activeBrush(app) = sbl::defaultAirbrush();
            app.foreground = sbl::StraightRgba8{30, 30, 30, 255};
            // Stacked low-opacity passes: the workflow D-004 named as the cost
            // it was accepting, driven through the real app paint path.
            for (int pass = 0; pass < 12; ++pass) {
                beginPaint(app);
                for (int i = 0; i < 24; ++i) {
                    const double cx = 120.0 + i * 8.0;
                    sbl::InputSample sample =
                        mouseSample(app, toScreenX(app.view, cx, 300.0),
                                         toScreenY(app.view, cx, 300.0));
                    sample.fromMouse = false;
                    sample.pressure  = 0.8f;
                    paintWith(app, sample);
                }
                endPaint(app);
            }

            const auto path = std::filesystem::temp_directory_path() /
                              "sable_selftest_16bit.sable";
            app.doc.path = path;
            doSaveProject(app, path);
            auto reopened = sbl::importDocument(path);
            if (!reopened.has_value() ||
                reopened->depth != sbl::ColourDepth::Bits16) {
                SDL_Log("selftest FAILED: a 16-bit project did not reopen as 16-bit");
                return 1;
            }
            // The pixels, not merely the manifest: a save that narrowed them
            // would reload as a perfectly valid 16-bit document full of 8-bit
            // values, which is the failure that looks like success.
            const sbl::Tile* saved = app.doc.layers.front().find(sbl::TileKey{0, 1});
            const sbl::Tile* back  = reopened->layers.front().find(sbl::TileKey{0, 1});
            if (saved == nullptr || back == nullptr) {
                SDL_Log("selftest FAILED: the 16-bit stroke wrote no tile");
                return 1;
            }
            int worst16 = 0;
            for (int y = 0; y < sbl::TILE_SIZE; y += 4)
                for (int x = 0; x < sbl::TILE_SIZE; x += 4)
                    worst16 = std::max(worst16,
                                       std::abs(static_cast<int>(saved->pixel(x, y).a) -
                                                static_cast<int>(back->pixel(x, y).a)));
            if (worst16 > 4) {
                SDL_Log("selftest FAILED: a 16-bit round trip moved alpha by %d",
                        worst16);
                return 1;
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
            SDL_Log("selftest: 16-bit canvas paints, saves and reopens "
                    "(alpha within %d of 65535), GPU declined it", worst16);
        }

        SDL_Log("selftest OK");

        // Exit before the settings write below. The self-test drives the app
        // with synthetic values, and persisting those would edit the real
        // user's preferences — which is how a test run silently changed
        // someone's stabilizer setting.
        sbl::setPaintBackend(nullptr);
        app.gpu.reset();
        canvas.releaseAll();
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    while (app.running) {
        SDL_Event e;
        if (app.framesToSettle > 0) {
            while (SDL_PollEvent(&e)) handleEvent(app, e);
            --app.framesToSettle;
        } else {
            // Block. Near-zero CPU while nobody is drawing (US-14.1).
            if (!SDL_WaitEvent(&e)) break;
            handleEvent(app, e);
            // Drain the whole queue: every queued motion event is a sample,
            // and keeping only the newest visibly degrades stroke quality.
            while (SDL_PollEvent(&e)) handleEvent(app, e);
        }
        pumpDialog(app);
        maybeAutosave(app, SDL_GetTicks());
        renderFrame(app);
    }

    // The worker owns a clone, so it is safe to let it finish — but not to
    // leave it running while `app` goes out of scope underneath it.
    if (app.autosaveWorker.joinable()) app.autosaveWorker.join();

    // A clean exit means the recovery copy has done its job. Leaving it would
    // offer the artist a stale "unsaved work" prompt on the next launch.
    if (!app.lastRecoveryFile.empty()) sbl::clearRecovery(app.lastRecoveryFile);

    Settings settings = loadSettings();
    collectSettings(app, settings);
    saveSettings(settings);

    // Before `app.gpu` is destroyed: the process default must never point at a
    // backend that has gone.
    sbl::setPaintBackend(nullptr);
    app.gpu.reset();

    canvas.releaseAll();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
