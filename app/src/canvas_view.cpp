#include "canvas_view.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "sbl/io.hpp"

namespace {

constexpr float kMinZoom = 0.1f;    // US-05.3: at least 10% - 800%
constexpr float kMaxZoom = 8.0f;

/// The screen-space bounding box of a canvas rectangle. Four corners, because
/// once the view is turned the box is no longer the rectangle.
SDL_FRect screenBounds(const View& v, double x, double y, double w, double h) {
    const double xs[4]{x, x + w, x + w, x};
    const double ys[4]{y, y, y + h, y + h};
    double minX = toScreenX(v, xs[0], ys[0]), maxX = minX;
    double minY = toScreenY(v, xs[0], ys[0]), maxY = minY;
    for (int i = 1; i < 4; ++i) {
        const double sx = toScreenX(v, xs[i], ys[i]);
        const double sy = toScreenY(v, xs[i], ys[i]);
        minX = std::min(minX, sx); maxX = std::max(maxX, sx);
        minY = std::min(minY, sy); maxY = std::max(maxY, sy);
    }
    return SDL_FRect{static_cast<float>(minX), static_cast<float>(minY),
                     static_cast<float>(maxX - minX), static_cast<float>(maxY - minY)};
}

/// Puts the document centre in the middle of the viewport. Works at any angle,
/// which the old "pan = corner offset" arithmetic did not.
void centreOn(View& v, const sbl::Document& doc, const SDL_FRect& viewport) {
    anchorAt(v, doc.width * 0.5, doc.height * 0.5,
             viewport.x + viewport.w * 0.5, viewport.y + viewport.h * 0.5);
}

}  // namespace

void fitToViewport(View& v, const sbl::Document& doc, const SDL_FRect& viewport) {
    if (doc.width <= 0 || doc.height <= 0) return;
    const float pad = 16.0f;
    // Fit the turned canvas, not the upright one: at 45 degrees a square needs
    // room for its diagonal or the corners hang off the edge.
    const double c = std::abs(std::cos(v.rotation));
    const double s = std::abs(std::sin(v.rotation));
    const auto span = [&](double along, double across) {
        return static_cast<float>(doc.width * along + doc.height * across);
    };
    const float zx = (viewport.w - pad * 2.0f) / span(c, s);
    const float zy = (viewport.h - pad * 2.0f) / span(s, c);
    v.zoom = std::clamp(std::min(zx, zy), kMinZoom, kMaxZoom);
    centreOn(v, doc, viewport);
}

void zoomToActualSize(View& v, const sbl::Document& doc, const SDL_FRect& viewport) {
    v.zoom = 1.0f;
    centreOn(v, doc, viewport);
}

void zoomAbout(View& v, double sx, double sy, float factor) {
    // The canvas pixel under the cursor must not move — US-05.2 is explicit
    // that zoom is about the cursor, not the window centre.
    const double cx = toCanvasX(v, sx, sy);
    const double cy = toCanvasY(v, sx, sy);
    const float next = std::clamp(v.zoom * factor, kMinZoom, kMaxZoom);
    if (next == v.zoom) return;
    v.zoom = next;
    anchorAt(v, cx, cy, sx, sy);
}

CanvasView::~CanvasView() { releaseAll(); }

void CanvasView::markDirty(sbl::TileKey key) { dirty_.insert(key); }

void CanvasView::markDabArea(double x, double y, float radius,
                             const sbl::Document& doc) {
    const auto lo = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    const auto hi = [](double v) { return static_cast<std::int32_t>(std::ceil(v)); };

    const std::int32_t minX = std::max<std::int32_t>(0, lo(x - radius));
    const std::int32_t maxX = std::min<std::int32_t>(doc.width  - 1, hi(x + radius));
    const std::int32_t minY = std::max<std::int32_t>(0, lo(y - radius));
    const std::int32_t maxY = std::min<std::int32_t>(doc.height - 1, hi(y + radius));
    if (minX > maxX || minY > maxY) return;

    for (std::int32_t ty = sbl::tileIndex(minY); ty <= sbl::tileIndex(maxY); ++ty)
        for (std::int32_t tx = sbl::tileIndex(minX); tx <= sbl::tileIndex(maxX); ++tx)
            dirty_.insert(sbl::TileKey{tx, ty});
}

void CanvasView::markAllDirty() {
    for (const auto& [key, tex] : textures_) dirty_.insert(key);
}

void CanvasView::release(sbl::TileKey key) {
    if (const auto it = textures_.find(key); it != textures_.end()) {
        SDL_DestroyTexture(it->second);
        textures_.erase(it);
    }
    dirty_.erase(key);
}

void CanvasView::releaseAll() {
    for (auto& [key, tex] : textures_) SDL_DestroyTexture(tex);
    textures_.clear();
    dirty_.clear();
}

SDL_Texture* CanvasView::textureFor(sbl::TileKey key, const sbl::Document& doc,
                                    bool nearest) {
    auto it = textures_.find(key);
    bool fresh = false;
    if (it == textures_.end()) {
        SDL_Texture* tex = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
            sbl::TILE_SIZE, sbl::TILE_SIZE);
        if (tex == nullptr) return nullptr;
        // D-004/D-002: we store premultiplied alpha. The default blend mode
        // assumes straight alpha and renders "nearly right" — grey fringes on
        // soft edges — for as long as it takes someone to notice.
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        it = textures_.emplace(key, tex).first;
        fresh = true;
    }

    if (fresh || dirty_.contains(key)) {
        // The engine composites, the GPU blits (D-007). Going through
        // compositeRect is what makes the screen agree with the export: blend
        // modes, clipping groups and folders all live in there, and a second
        // implementation in this file would drift from it (#1).
        const std::vector<sbl::PremulRgba8> px = sbl::compositeRect(
            doc, key.first * sbl::TILE_SIZE, key.second * sbl::TILE_SIZE,
            sbl::TILE_SIZE, sbl::TILE_SIZE);
        SDL_UpdateTexture(it->second, nullptr, px.data(), sbl::TILE_SIZE * 4);
        dirty_.erase(key);
        ++uploads_;
    }
    // US-05.7: 1:1 pixels at 100% and above, no resampling blur.
    SDL_SetTextureScaleMode(it->second,
                            nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    return it->second;
}

void CanvasView::render(const sbl::Document& doc, const View& view,
                        const SDL_FRect& viewport) {
    const SDL_Rect clip{static_cast<int>(viewport.x), static_cast<int>(viewport.y),
                        static_cast<int>(viewport.w), static_cast<int>(viewport.h)};
    SDL_SetRenderClipRect(renderer_, &clip);

    const auto angle = rotationDegrees(view);

    // No separate background fill: the background is the bottom of the
    // composite, and drawing it twice would double-blend a transparent canvas.
    const bool nearest = view.zoom >= 1.0f;
    std::vector<sbl::TileKey> visible;

    // Every tile of the canvas, not every tile of every layer — one composited
    // tile now stands for the whole stack.
    for (std::int32_t ty = 0; ty <= sbl::tileIndex(doc.height - 1); ++ty) {
        for (std::int32_t tx = 0; tx <= sbl::tileIndex(doc.width - 1); ++tx) {
            const sbl::TileKey key{tx, ty};
            // Clipped to the canvas, so an edge tile does not blit its
            // transparent padding over the outline.
            const SDL_FRect src{
                0.0f, 0.0f,
                static_cast<float>(std::min(sbl::TILE_SIZE, doc.width  - tx * sbl::TILE_SIZE)),
                static_cast<float>(std::min(sbl::TILE_SIZE, doc.height - ty * sbl::TILE_SIZE)),
            };
            const double ox = static_cast<double>(tx) * sbl::TILE_SIZE;
            const double oy = static_cast<double>(ty) * sbl::TILE_SIZE;
            // dst is the tile placed at its rotated corner but still upright;
            // SDL turns it about that corner below, which composes to the same
            // transform toScreenX/Y describe.
            const SDL_FRect dst{
                static_cast<float>(toScreenX(view, ox, oy)),
                static_cast<float>(toScreenY(view, ox, oy)),
                src.w * view.zoom,
                src.h * view.zoom,
            };
            // Cull against the turned footprint: dst itself is in the wrong
            // place to test once the view is rotated, which would evict tiles
            // that are plainly on screen.
            const SDL_FRect bounds = screenBounds(view, ox, oy, src.w, src.h);
            if (!SDL_HasRectIntersectionFloat(&bounds, &viewport)) continue;

            visible.push_back(key);
            if (SDL_Texture* tex = textureFor(key, doc, nearest); tex != nullptr) {
                if (view.rotation == 0.0) {
                    SDL_RenderTexture(renderer_, tex, &src, &dst);
                } else {
                    const SDL_FPoint pivot{0.0f, 0.0f};   // the tile's own corner
                    SDL_RenderTextureRotated(renderer_, tex, &src, &dst, angle,
                                             &pivot, SDL_FLIP_NONE);
                }
            }
        }
    }

    // Evict textures for tiles that left the viewport. Their pixels live in
    // the Tile; the texture is a cache, not storage.
    if (textures_.size() > visible.size()) {
        const std::unordered_set<sbl::TileKey, sbl::TileKeyHash>
            keep(visible.begin(), visible.end());
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (keep.contains(it->first)) { ++it; continue; }
            SDL_DestroyTexture(it->second);
            it = textures_.erase(it);
        }
    }

    // A one-pixel outline, so a transparent canvas still has visible bounds.
    // Drawn as a closed polyline rather than a rect: it has to turn with the
    // canvas, and an axis-aligned box would be a lie at any angle.
    const double cw = doc.width, ch = doc.height;
    const double corners[5][2]{{0, 0}, {cw, 0}, {cw, ch}, {0, ch}, {0, 0}};
    SDL_FPoint outline[5];
    for (int i = 0; i < 5; ++i)
        outline[i] = SDL_FPoint{
            static_cast<float>(toScreenX(view, corners[i][0], corners[i][1])),
            static_cast<float>(toScreenY(view, corners[i][0], corners[i][1]))};

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 90, 90, 96, 255);
    SDL_RenderLines(renderer_, outline, 5);
    SDL_SetRenderClipRect(renderer_, nullptr);
}
