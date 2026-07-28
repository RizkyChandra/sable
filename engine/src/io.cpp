#include "sbl/io.hpp"

#include <algorithm>
#include <optional>
#include <system_error>

#include "lodepng.h"

namespace sbl {
namespace {

[[nodiscard]] std::uint8_t scale8(std::uint8_t c, float f) noexcept {
    return static_cast<std::uint8_t>(static_cast<float>(c) * f + 0.5f);
}

}  // namespace

std::string_view describe(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::NotFound:           return "not found";
        case ErrorKind::Permission:         return "permission denied";
        case ErrorKind::Malformed:          return "malformed file";
        case ErrorKind::UnsupportedVersion: return "unsupported version";
        case ErrorKind::Io:                 break;
    }
    return "I/O error";
}

namespace {

/// The layers at one nesting level: top level is everything with no parent,
/// a folder's level is its children in document order.
std::vector<const Layer*> levelOf(const Document& doc, std::optional<LayerId> parent) {
    std::vector<const Layer*> out;
    for (const Layer& layer : doc.layers)
        if (layer.parent == parent) out.push_back(&layer);
    return out;
}

/// Composites one level into `buf`, which is already premultiplied.
///
/// A folder composites its children into a transparent scratch buffer first,
/// then blends that as a single unit — which is the whole point of a group:
/// its opacity and blend mode apply to the result, not to each child.
void compositeLevel(std::vector<PremulRgba8>& buf, const Document& doc,
                    std::optional<LayerId> parent, std::size_t w, std::size_t h) {
    std::vector<std::uint8_t> clipMask;

    for (const Layer* layer : levelOf(doc, parent)) {
        const bool clipped = layer->clipToBelow && !clipMask.empty();
        if (!layer->visible || layer->opacity <= 0.0f) {
            if (!layer->clipToBelow) clipMask.clear();
            continue;
        }
        const float opacity = std::clamp(layer->opacity, 0.0f, 1.0f);
        std::vector<std::uint8_t> ownAlpha;
        if (!layer->clipToBelow) ownAlpha.assign(w * h, 0);

        if (layer->kind == LayerKind::Folder) {
            std::vector<PremulRgba8> group(w * h, PremulRgba8{});
            compositeLevel(group, doc, layer->id, w, h);
            for (std::size_t i = 0; i < buf.size(); ++i) {
                PremulRgba8 src = group[i];
                if (!ownAlpha.empty()) ownAlpha[i] = src.a;
                if (src.a == 0) continue;
                float scale = opacity;
                if (clipped) scale *= static_cast<float>(clipMask[i]) / 255.0f;
                if (scale <= 0.0f) continue;
                if (scale < 1.0f)
                    src = PremulRgba8{scale8(src.r, scale), scale8(src.g, scale),
                                      scale8(src.b, scale), scale8(src.a, scale)};
                buf[i] = blendOver(layer->blend, src, buf[i]);
            }
        } else {
            for (const auto& [key, tile] : layer->tiles) {
                const std::int32_t ox = key.first  * TILE_SIZE;
                const std::int32_t oy = key.second * TILE_SIZE;
                const std::int32_t y0 = std::max<std::int32_t>(0, -oy);
                const std::int32_t y1 = std::min<std::int32_t>(TILE_SIZE, doc.height - oy);
                const std::int32_t x0 = std::max<std::int32_t>(0, -ox);
                const std::int32_t x1 = std::min<std::int32_t>(TILE_SIZE, doc.width - ox);

                for (std::int32_t ty = y0; ty < y1; ++ty) {
                    const PremulRgba8* row =
                        tile.pixels() + static_cast<std::size_t>(ty) * TILE_SIZE;
                    const std::size_t rowStart =
                        static_cast<std::size_t>(oy + ty) * w + static_cast<std::size_t>(ox);
                    for (std::int32_t tx = x0; tx < x1; ++tx) {
                        const std::size_t at = rowStart + static_cast<std::size_t>(tx);
                        PremulRgba8 src = row[tx];
                        if (!ownAlpha.empty()) ownAlpha[at] = src.a;
                        if (src.a == 0) continue;
                        float scale = opacity;
                        if (clipped) scale *= static_cast<float>(clipMask[at]) / 255.0f;
                        if (scale <= 0.0f) continue;
                        if (scale < 1.0f)
                            src = PremulRgba8{scale8(src.r, scale), scale8(src.g, scale),
                                              scale8(src.b, scale), scale8(src.a, scale)};
                        buf[at] = blendOver(layer->blend, src, buf[at]);
                    }
                }
            }
        }
        if (!layer->clipToBelow) clipMask = std::move(ownAlpha);
    }
}

/// The single-pixel mirror of compositeLevel. The two MUST agree: one draws
/// the canvas, the other answers Alt+click, and a divergence means the artist
/// samples a colour that is not on screen. A test asserts it pixel for pixel.
PremulRgba8 pickLevel(const Document& doc, std::optional<LayerId> parent,
                      std::int32_t x, std::int32_t y, PremulRgba8 acc) {
    const TileKey key{tileIndex(x), tileIndex(y)};
    const int tx = x - key.first  * TILE_SIZE;
    const int ty = y - key.second * TILE_SIZE;
    int clipAlpha = -1;

    for (const Layer* layer : levelOf(doc, parent)) {
        const bool clipped = layer->clipToBelow && clipAlpha >= 0;

        PremulRgba8 own{};
        if (layer->kind == LayerKind::Folder)
            own = pickLevel(doc, layer->id, x, y, PremulRgba8{});
        else if (const Tile* tile = layer->find(key); tile != nullptr)
            own = tile->pixel(tx, ty);

        if (!layer->clipToBelow) clipAlpha = own.a;
        if (!layer->visible || layer->opacity <= 0.0f || own.a == 0) continue;

        float scale = std::clamp(layer->opacity, 0.0f, 1.0f);
        if (clipped) scale *= static_cast<float>(clipAlpha) / 255.0f;
        if (scale <= 0.0f) continue;

        PremulRgba8 src = own;
        if (scale < 1.0f)
            src = PremulRgba8{scale8(src.r, scale), scale8(src.g, scale),
                              scale8(src.b, scale), scale8(src.a, scale)};
        acc = blendOver(layer->blend, src, acc);
    }
    return acc;
}

}  // namespace

std::vector<StraightRgba8> flatten(const Document& doc) {
    const auto w = static_cast<std::size_t>(std::max(doc.width, 0));
    const auto h = static_cast<std::size_t>(std::max(doc.height, 0));
    if (w == 0 || h == 0) return {};

    // Composite in premultiplied space, convert once at the end. Doing it the
    // other way round is what produces the dark halo (D-004, US-07.3).
    std::vector<PremulRgba8> buf(w * h, doc.background.premultiply());
    compositeLevel(buf, doc, std::nullopt, w, h);

    std::vector<StraightRgba8> out(w * h);
    for (std::size_t i = 0; i < buf.size(); ++i) out[i] = buf[i].unpremultiply();
    return out;
}

StraightRgba8 pickColour(const Document& doc, std::int32_t x, std::int32_t y) noexcept {
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return StraightRgba8{};
    return pickLevel(doc, std::nullopt, x, y, doc.background.premultiply()).unpremultiply();
}

std::expected<void, Error> exportPng(const Document& doc,
                                     const std::filesystem::path& path) {
    if (doc.width <= 0 || doc.height <= 0)
        return std::unexpected(Error{ErrorKind::Malformed, "canvas has no size"});

    // Exactly the canvas dimensions — not the viewport, and unaffected by zoom.
    const std::vector<StraightRgba8> pixels = flatten(doc);

    const unsigned rc = lodepng::encode(
        path.string(),
        reinterpret_cast<const unsigned char*>(pixels.data()),
        static_cast<unsigned>(doc.width), static_cast<unsigned>(doc.height));
    if (rc == 0) return {};

    // 79 is lodepng's "failed to open file for writing". Everything else that
    // can go wrong at this point is a write failure too.
    ErrorKind kind = ErrorKind::Io;
    if (rc == 79) {
        std::error_code ec;
        const auto parent = path.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent, ec))
            kind = ErrorKind::NotFound;
        else
            kind = ErrorKind::Permission;
    }
    return std::unexpected(Error{
        kind, "could not write " + path.string() + ": " + lodepng_error_text(rc)});
}

}  // namespace sbl
