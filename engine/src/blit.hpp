// Straight-alpha rectangles into the sparse tile map, engine-internal.
//
// Every interop format stores a layer as a rectangle of pixels at an offset —
// ORA one PNG per layer, Krita one small tile at a time — while Sable stores
// sparse 256 px tiles. This is that conversion, in one place, because the
// coordinate arithmetic is where an importer goes subtly wrong.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "sbl/canvas.hpp"

namespace sbl {

/// Blits `imageW` x `imageH` straight-alpha RGBA8 pixels into `layer` with
/// their top-left at (offsetX, offsetY) in canvas coordinates.
///
/// Clipped to the canvas: Sable's canvas is fixed, the compositor draws
/// nothing outside it, and a file with a wild offset would otherwise allocate
/// tiles no one can ever see.
inline void blitStraightImage(Layer& layer, const unsigned char* straight,
                              std::int32_t imageW, std::int32_t imageH,
                              std::int32_t offsetX, std::int32_t offsetY,
                              std::int32_t canvasW, std::int32_t canvasH) {
    const std::int32_t x0 = std::max<std::int32_t>(0, offsetX);
    const std::int32_t y0 = std::max<std::int32_t>(0, offsetY);
    const std::int32_t x1 = std::min(canvasW, offsetX + imageW);
    const std::int32_t y1 = std::min(canvasH, offsetY + imageH);
    if (x1 <= x0 || y1 <= y0) return;

    for (std::int32_t ty = tileIndex(y0); ty <= tileIndex(y1 - 1); ++ty) {
        for (std::int32_t tx = tileIndex(x0); tx <= tileIndex(x1 - 1); ++tx) {
            const std::int32_t originX = tx * TILE_SIZE;
            const std::int32_t originY = ty * TILE_SIZE;
            const std::int32_t left   = std::max(x0, originX);
            const std::int32_t top    = std::max(y0, originY);
            const std::int32_t right  = std::min(x1, originX + TILE_SIZE);
            const std::int32_t bottom = std::min(y1, originY + TILE_SIZE);

            Tile& tile = layer.tileFor(TileKey{tx, ty});
            for (std::int32_t y = top; y < bottom; ++y) {
                for (std::int32_t x = left; x < right; ++x) {
                    const std::size_t at =
                        (static_cast<std::size_t>(y - offsetY) *
                             static_cast<std::size_t>(imageW) +
                         static_cast<std::size_t>(x - offsetX)) * 4;
                    tile.setPixel(x - originX, y - originY,
                                  StraightRgba8{straight[at + 0], straight[at + 1],
                                                straight[at + 2], straight[at + 3]}
                                      .premultiply());
                }
            }
        }
    }
}

/// What an imported rectangle leaves behind: tiles that were touched but are
/// entirely transparent. Dropping them is what keeps the map sparse (D-005)
/// rather than carrying blank 256 KiB buffers for the life of the document.
inline void dropBlankTiles(Layer& layer) {
    std::erase_if(layer.tiles, [](const auto& entry) {
        return entry.second.isFullyTransparent();
    });
}

}  // namespace sbl
