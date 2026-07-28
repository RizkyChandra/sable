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
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <atomic>
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
#include "sbl/canvas.hpp"
#include "sbl/format.hpp"
#include "sbl/io.hpp"
#include "sbl/paint.hpp"
#include "sbl/project.hpp"
#include "settings.hpp"
#include "shortcuts.hpp"
#include "widgets.hpp"

namespace {

constexpr int   kDefaultCanvas = 1024;
constexpr float kZoomStep      = 1.15f;
constexpr float kLeftPanelBase  = 200.0f;
constexpr float kRightPanelBase = 250.0f;
constexpr float kDegToRad      = 3.14159265358979323846f / 180.0f;

/// What to do once the artist answers the "discard unsaved work?" prompt.
enum class Pending { None, NewCanvas, Quit, Open, Import };

enum class Tool { Brush, Eraser, Fill, Select, Transform };

/// How often a recovery copy is written while the document is dirty (D-013).
constexpr std::uint64_t kAutosaveIntervalMs = 120'000;

struct NewCanvasForm {
    int  width  = kDefaultCanvas;
    int  height = kDefaultCanvas;
    bool transparent = false;
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

    sbl::StraightRgba8 foreground{0, 0, 0, 255};
    sbl::StraightRgba8 background{255, 255, 255, 255};
    float foregroundHsv[3]{0.0f, 0.0f, 0.0f};

    std::vector<float> sizePresets{2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f};

    sbl::Stroke           stroke;
    std::vector<sbl::Dab> scratch;
    bool painting = false;

    // --- tablet
    std::unordered_map<SDL_PenID, PenAxisState> penAxes;
    sbl::TabletProfile  profile;              // see D-015: one profile, all pens
    sbl::PressureFilter pressureFilter;
    sbl::Stabilizer     stabilizer;
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
        case Tool::Transform: return "transform";
    }
    return "brush";
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
    app.shortcuts.load(settings);

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
    app.shortcuts.store(settings);

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

void resetDocument(App& app, std::int32_t w, std::int32_t h, bool transparent) {
    app.canvas->releaseAll();
    app.doc = sbl::makeDocument(
        w, h, transparent ? sbl::StraightRgba8{0, 0, 0, 0}
                          : sbl::StraightRgba8{255, 255, 255, 255});
    applyUndoBudget(app);
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
    auto loaded = sbl::importDocument(path);
    if (!loaded.has_value()) {
        showError(app, loaded.error());
        return;
    }
    refreshAllTextures(app);
    app.doc = std::move(*loaded);
    applyUndoBudget(app);
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
    app.pressureFilter.reset();
    app.painting = true;
    SDL_CaptureMouse(true);      // so a release outside the window still arrives
}

void paintWith(App& app, sbl::InputSample sample) {
    sbl::Layer* layer = app.doc.active();
    if (layer == nullptr) return;

    app.lastCanvasX = sample.x;
    app.lastCanvasY = sample.y;

    // Positions only — the stabilizer never touches pressure (US-11.6).
    sample = app.stabilizer.apply(sample);

    const sbl::Selection* selection =
        app.doc.selection.has_value() && !app.doc.selection->empty()
            ? &*app.doc.selection : nullptr;
    sbl::PaintTarget target{*layer, app.stroke.pending, app.stroke.touched,
                            app.doc.width, app.doc.height, selection};
    sbl::paintSample(app.stroke, target, sample, app.scratch);

    for (const sbl::Dab& d : app.scratch)
        app.canvas->markDabArea(d.x, d.y, d.radius, app.doc);
}

sbl::InputSample mouseSample(App& app, double sx, double sy) {
    sbl::InputSample sample;
    sample.x = toCanvasX(app.view, sx);
    sample.y = toCanvasY(app.view, sy);
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
    sample.x = toCanvasX(app.view, sx);
    sample.y = toCanvasY(app.view, sy);
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
            const sbl::InputSample last =
                app.stabilizer.finish(app.stroke.samples.back());
            sbl::PaintTarget target{*layer, app.stroke.pending, app.stroke.touched,
                                    app.doc.width, app.doc.height};
            sbl::paintSample(app.stroke, target, last, app.scratch);
            for (const sbl::Dab& d : app.scratch)
                app.canvas->markDabArea(d.x, d.y, d.radius, app.doc);
        }
    }

    if (!app.stroke.pending.empty()) {
        app.doc.undo.push(std::move(app.stroke.pending));
        app.doc.dirty = true;
    }
    app.stroke.samples.clear();
}

void doFill(App& app, double sx, double sy) {
    const auto x = static_cast<std::int32_t>(std::floor(toCanvasX(app.view, sx)));
    const auto y = static_cast<std::int32_t>(std::floor(toCanvasY(app.view, sy)));

    sbl::UndoRecord rec =
        sbl::bucketFill(app.doc, app.doc.activeLayer, x, y, app.foreground,
                        app.fillTolerance);
    if (rec.empty()) return;

    for (const auto& snap : rec.tiles) app.canvas->markDirty(snap.key);
    app.doc.undo.push(std::move(rec));
    app.doc.dirty = true;
}

void updateSelection(App& app, double sx, double sy) {
    const double cx = toCanvasX(app.view, sx);
    const double cy = toCanvasY(app.view, sy);

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
    app.doc.selection = selection;
}

void pickColourAt(App& app, double sx, double sy) {
    const auto x = static_cast<std::int32_t>(std::floor(toCanvasX(app.view, sx)));
    const auto y = static_cast<std::int32_t>(std::floor(toCanvasY(app.view, sy)));
    if (x < 0 || y < 0 || x >= app.doc.width || y >= app.doc.height) return;

    const sbl::StraightRgba8 picked = sbl::pickColour(app.doc, x, y);
    app.foreground = sbl::StraightRgba8{picked.r, picked.g, picked.b, 255};
    syncHsvFromColour(app);
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
void requestPending(App& app, Pending what) {
    app.pending = what;
    if (app.doc.dirty) { app.openDiscard = true; return; }
    if (what == Pending::NewCanvas)   app.openNew = true;
    else if (what == Pending::Open)   askForOpenPath(app);
    else if (what == Pending::Import) askForImportPath(app);
    else                              app.running = false;
    app.pending = Pending::None;
}

// ----------------------------------------------------------------------- input

bool overCanvas(const App& app, float x, float y) {
    return x >= app.viewport.x && y >= app.viewport.y &&
           x < app.viewport.x + app.viewport.w && y < app.viewport.y + app.viewport.h;
}

void handleKey(App& app, const SDL_KeyboardEvent& key) {
    if (key.key == SDLK_SPACE) { app.spaceHeld = key.down; return; }
    if (!key.down) return;

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

        case Action::ToolBrush:  app.tool = Tool::Brush;  break;
        case Action::ToolEraser: app.tool = Tool::Eraser; break;
        case Action::ToolFill:   app.tool = Tool::Fill;   break;
        case Action::ToolSelect: app.tool = Tool::Select; break;
        case Action::ToolTransform: app.tool = Tool::Transform; break;

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

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (!io.WantCaptureKeyboard) handleKey(app, e.key);
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
            beginPaint(app);
            paintWith(app, penSample(app, e.ptouch.which, e.ptouch.x, e.ptouch.y));
            break;

        case SDL_EVENT_PEN_MOTION:
            app.activePen = e.pmotion.which;
            ++app.motionThisFrame;
            // Hovering without contact does not paint (US-08.8) — the pen only
            // paints between PEN_DOWN and PEN_UP.
            if (app.painting)
                paintWith(app, penSample(app, e.pmotion.which, e.pmotion.x, e.pmotion.y));
            break;

        case SDL_EVENT_PEN_UP:
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
                app.panAnchorX = e.button.x - app.view.panX;
                app.panAnchorY = e.button.y - app.view.panY;
            } else if (e.button.button == SDL_BUTTON_LEFT) {
                if ((SDL_GetModState() & SDL_KMOD_ALT) != 0) {
                    pickColourAt(app, e.button.x, e.button.y);   // US-13.3
                } else if (app.tool == Tool::Fill) {
                    doFill(app, e.button.x, e.button.y);
                } else if (app.tool == Tool::Select) {
                    app.selecting = true;
                    app.selectAnchorX = toCanvasX(app.view, e.button.x);
                    app.selectAnchorY = toCanvasY(app.view, e.button.y);
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
            } else if (app.selecting) {
                updateSelection(app, e.motion.x, e.motion.y);
            } else if (app.painting) {
                paintWith(app, mouseSample(app, e.motion.x, e.motion.y));
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            // Handled wherever the pointer is: releasing outside the window
            // must still end the stroke cleanly (US-02.5).
            app.panning = false;
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (app.selecting) {
                    app.selecting = false;
                    // A click with no drag clears the selection rather than
                    // leaving a zero-sized one that silently blocks painting.
                    if (app.doc.selection.has_value() && app.doc.selection->empty())
                        app.doc.selection.reset();
                }
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
            if (iconRadio(icon, label, app.tool == tool)) app.tool = tool;
        };
        toolRow(Icon::Brush,     "Brush",     Tool::Brush,     Action::ToolBrush);
        toolRow(Icon::Eraser,    "Eraser",    Tool::Eraser,    Action::ToolEraser);
        toolRow(Icon::Fill,      "Fill",      Tool::Fill,      Action::ToolFill);
        toolRow(Icon::Select,    "Select",    Tool::Select,    Action::ToolSelect);
        toolRow(Icon::Transform, "Transform", Tool::Transform, Action::ToolTransform);

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
        if (app.tool == Tool::Select && app.doc.selection.has_value()) {
            if (ImGui::SmallButton("Deselect")) app.doc.selection.reset();
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
                          layer.kind == sbl::LayerKind::Folder ? "[group] " : "",
                          layer.name.c_str(), layer.locked ? "  [locked]" : "");
            if (ImGui::Selectable(label, active)) app.doc.activeLayer = layer.id;

            if (active) {
                ImGui::Indent(12.0f);
                float opacity = layer.opacity;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##op", &opacity, 0.0f, 1.0f, "opacity %.2f")) {
                    layer.opacity = opacity;    // live while dragging...
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

                int blend = static_cast<int>(layer.blend);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##blend", &blend,
                                 "Normal\0Multiply\0Screen\0Add\0Overlay\0")) {
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
        ImGui::Text("%d%%   %d x %d%s",
                    static_cast<int>(std::lround(app.view.zoom * 100.0f)),
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
        // D-102 asks for a VISIBLE policy, not just a cap. Say it plainly, and
        // only once there is something to say.
        if (app.doc.undo.droppedRecords() > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                               "   %zu older undo step(s) dropped (%d MB limit)",
                               app.doc.undo.droppedRecords(), app.undoBudgetMb);
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

        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(110, 0))) {
            resetDocument(app, app.form.width, app.form.height, app.form.transparent);
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
            app.doc.dirty = false;
            ImGui::CloseCurrentPopup();
            if (what == Pending::NewCanvas) app.openNew = true;
            else if (what == Pending::Open) askForOpenPath(app);
            else if (what == Pending::Import) askForImportPath(app);
            else if (what == Pending::Quit) app.running = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            app.pending = Pending::None;
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
void drawSelectionOutline(const App& app) {
    if (!app.doc.selection.has_value() || app.doc.selection->empty()) return;
    const sbl::Selection& s = *app.doc.selection;

    const ImVec2 lo(static_cast<float>(app.view.panX + s.x * app.view.zoom),
                    static_cast<float>(app.view.panY + s.y * app.view.zoom));
    const ImVec2 hi(static_cast<float>(app.view.panX + (s.x + s.w) * app.view.zoom),
                    static_cast<float>(app.view.panY + (s.y + s.h) * app.view.zoom));

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRect(lo, hi, IM_COL32(0, 0, 0, 200), 0.0f, 0, 3.0f);
    draw->AddRect(lo, hi, IM_COL32(255, 255, 255, 230), 0.0f, 0, 1.0f);
}

void drawBrushCursor(const App& app) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    float mx = 0.0f, my = 0.0f;
    SDL_GetMouseState(&mx, &my);
    if (!overCanvas(app, mx, my)) return;

    if (!paintingTool(app)) return;
    const float radius =
        activeBrush(const_cast<App&>(app)).size * 0.5f * app.view.zoom;
    if (radius < 1.0f) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddCircle(ImVec2(mx, my), radius, IM_COL32(0, 0, 0, 160), 0, 3.0f);
    draw->AddCircle(ImVec2(mx, my), radius, IM_COL32(255, 255, 255, 200), 0, 1.0f);
}

void renderFrame(App& app) {
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
    drawSelectionOutline(app);
    drawBrushCursor(app);

    ImGui::Render();

    if (app.lightTheme) SDL_SetRenderDrawColor(app.renderer, 205, 205, 210, 255);
    else                SDL_SetRenderDrawColor(app.renderer, 48, 48, 52, 255);
    SDL_RenderClear(app.renderer);
    app.canvas->render(app.doc, app.view, app.viewport);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app.renderer);
    SDL_RenderPresent(app.renderer);

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
            sbl::InputSample sample =
                mouseSample(app, app.view.panX + 100.0 + t * 400.0,
                                 app.view.panY + 100.0 + t * 250.0);
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
        doFill(app, app.view.panX + 300.0, app.view.panY + 300.0);
        app.doc.selection.reset();

        for (int frame = 0; frame < 3; ++frame) renderFrame(app);

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
        SDL_Log("selftest: project round trip ok, %zu layers, %zu bytes",
                reloaded->layers.size(),
                static_cast<std::size_t>(std::filesystem::file_size(project)));

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
        // Stroke, new layer, and fill — each exactly one undoable step.
        if (!result.has_value() || app.doc.undo.size() != 3 ||
            app.doc.layers.size() != 2 || app.doc.active()->tiles.empty()) {
            SDL_Log("selftest FAILED");
            return 1;
        }
        SDL_Log("selftest OK");

        // Exit before the settings write below. The self-test drives the app
        // with synthetic values, and persisting those would edit the real
        // user's preferences — which is how a test run silently changed
        // someone's stabilizer setting.
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

    canvas.releaseAll();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
