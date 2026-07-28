// The canvas viewport: pan, zoom, and one streaming SDL_Texture per visible
// tile. The GPU blits finished pixels; it never paints them (D-007).
//
// Each texture holds the whole layer stack composited for that tile, straight
// from the engine — the same code the export goes through, because a renderer
// with its own idea of blend modes is what shipped as #1.
#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <unordered_map>
#include <unordered_set>

#include "sbl/canvas.hpp"
#include "view_transform.hpp"   // View, and the screen <-> canvas maths

/// Fits the document inside `viewport` and centres it.
void fitToViewport(View& v, const sbl::Document& doc, const SDL_FRect& viewport);
/// Sets 100% and centres.
void zoomToActualSize(View& v, const sbl::Document& doc, const SDL_FRect& viewport);
/// Zooms about a screen point, so the canvas pixel under the cursor stays put.
void zoomAbout(View& v, double sx, double sy, float factor);
/// One rotation step, in radians. 15 degrees: fine enough to square up a
/// wobbly horizon, coarse enough that 24 presses go all the way round.
inline constexpr double kRotateStep = 3.14159265358979323846 / 12.0;

class CanvasView {
public:
    explicit CanvasView(SDL_Renderer* renderer) : renderer_(renderer) {}
    ~CanvasView();

    CanvasView(const CanvasView&)            = delete;
    CanvasView& operator=(const CanvasView&) = delete;

    /// Queue a tile for re-upload. Only tiles marked here are ever uploaded,
    /// which is what US-02.7 asks us to be able to prove.
    void markDirty(sbl::TileKey key);
    /// Marks every tile a dab covers, clipped to the canvas.
    void markDabArea(double x, double y, float radius, const sbl::Document& doc);
    /// Queues every live tile for re-upload, keeping the textures themselves.
    /// For changes that alter compositing rather than pixels — a layer's
    /// opacity, blend mode or visibility — where releaseAll() would recreate
    /// every texture on every frame of a slider drag.
    void markAllDirty();
    /// Drops a tile's texture — used when undo erases the tile (US-04.8).
    void release(sbl::TileKey key);
    void releaseAll();

    void render(const sbl::Document& doc, const View& view, const SDL_FRect& viewport);

    /// Cumulative texture uploads. Logged against the touched-tile set per
    /// stroke; the two must match.
    [[nodiscard]] std::size_t uploadCount() const noexcept { return uploads_; }

private:
    SDL_Texture* textureFor(sbl::TileKey key, const sbl::Document& doc, bool nearest);

    SDL_Renderer* renderer_ = nullptr;
    std::unordered_map<sbl::TileKey, SDL_Texture*, sbl::TileKeyHash> textures_;
    std::unordered_set<sbl::TileKey, sbl::TileKeyHash> dirty_;
    std::size_t uploads_ = 0;
};
