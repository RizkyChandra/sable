#include "sbl/io.hpp"

#include "sbl/backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
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

/// The rectangle of canvas a buffer covers: the whole document for flatten(),
/// a single tile for the canvas view. Buffer pixel 0 is canvas pixel (x, y).
struct Region {
    std::int32_t x = 0, y = 0;
    std::size_t  w = 0, h = 0;
};

/// Composites one level into `buf`, which is already premultiplied.
///
/// **This is an 8-bit compositor at both document depths, deliberately.** The
/// screen is 8-bit and so is PNG export, so a 16-bit tile has to be narrowed
/// somewhere; doing it here — per pixel, before the blend — means a 16-bit
/// document's smooth ramp arrives as the CORRECT 8-bit ramp rather than as the
/// plateaued one an 8-bit document would have stored in the first place, which
/// is the whole of the visible benefit. Compositing wide would additionally
/// help a STACK of semi-transparent 16-bit layers, where the intermediate
/// results are rounded twice. That is a second change and is not this one.
///
/// A folder composites its children into a transparent scratch buffer first,
/// then blends that as a single unit — which is the whole point of a group:
/// its opacity and blend mode apply to the result, not to each child.
void compositeLevel(std::vector<PremulRgba8>& buf, const Document& doc,
                    std::optional<LayerId> parent, const Region& r) {
    const std::size_t w = r.w;
    const std::size_t h = r.h;
    std::vector<std::uint8_t> clipMask;

    for (const Layer* layer : levelOf(doc, parent)) {
        const bool clipped = layer->clipToBelow && !clipMask.empty();
        if (!layer->visible || layer->opacity <= 0.0f) {
            if (!layer->clipToBelow) clipMask.clear();
            continue;
        }
        const float opacity = std::clamp(layer->opacity, 0.0f, 1.0f);
        // #48. Coverage folds into the same `scale` the opacity and the clip
        // mask already go through, so the three are one rounding rather than
        // three — and the layer's own alpha is published to the clip mask
        // AFTER the mask, because a clipped layer must not show through a pixel
        // its base has hidden.
        const LayerMask* mask =
            layer->mask.has_value() && layer->mask->enabled ? &*layer->mask : nullptr;
        std::vector<std::uint8_t> ownAlpha;
        if (!layer->clipToBelow) ownAlpha.assign(w * h, 0);

        if (layer->kind == LayerKind::Folder) {
            std::vector<PremulRgba8> group(w * h, PremulRgba8{});
            compositeLevel(group, doc, layer->id, r);
            for (std::size_t i = 0; i < buf.size(); ++i) {
                PremulRgba8 src = group[i];
                // A folder's mask applies to what its children composited to,
                // which is the whole reason a group can have one. Per pixel
                // here, not per tile: this buffer is a region, not a tile, and
                // a folder mask is rare enough not to earn a second path.
                float cover = 1.0f;
                if (mask != nullptr)
                    cover = static_cast<float>(maskCoverage(
                                *mask, r.x + static_cast<std::int32_t>(i % w),
                                r.y + static_cast<std::int32_t>(i / w))) / 255.0f;
                if (!ownAlpha.empty())
                    ownAlpha[i] = cover < 1.0f ? scale8(src.a, cover) : src.a;
                if (src.a == 0) continue;
                float scale = opacity * cover;
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
                // Tile-local bounds, clipped to the canvas and to the region.
                const std::int32_t y0 = std::max({0, -oy, r.y - oy});
                const std::int32_t y1 = std::min({TILE_SIZE, doc.height - oy,
                                                  r.y + static_cast<std::int32_t>(h) - oy});
                const std::int32_t x0 = std::max({0, -ox, r.x - ox});
                const std::int32_t x1 = std::min({TILE_SIZE, doc.width - ox,
                                                  r.x + static_cast<std::int32_t>(w) - ox});

                const PremulRgba8*  px8  = tile.pixels8();
                // The mask is keyed exactly like the pixels, so the tile that
                // covers these pixels is the tile at THIS key — one lookup for
                // 65'536 pixels instead of one each, which is the difference
                // between a mask costing a multiply and costing a hash.
                const Tile* maskTile = mask != nullptr ? mask->find(key) : nullptr;
                const PremulRgba8* maskPx =
                    maskTile != nullptr ? maskTile->pixels8() : nullptr;
                for (std::int32_t ty = y0; ty < y1; ++ty) {
                    const std::size_t rowAt = static_cast<std::size_t>(ty) * TILE_SIZE;
                    // Signed: ox - r.x is negative for a tile that starts left
                    // of the region, and only the sum with tx is in bounds.
                    const std::int64_t rowStart =
                        static_cast<std::int64_t>(oy + ty - r.y) *
                            static_cast<std::int64_t>(w) + (ox - r.x);
                    for (std::int32_t tx = x0; tx < x1; ++tx) {
                        const auto at = static_cast<std::size_t>(rowStart + tx);
                        // A 16-bit tile is narrowed HERE, on the way to an
                        // 8-bit screen and an 8-bit PNG, and nowhere earlier.
                        // See compositeLevel16 for the path a 16-bit document
                        // actually takes; this one only meets a wide tile in a
                        // document some other reader made mixed.
                        PremulRgba8 src = px8 != nullptr
                            ? px8[rowAt + static_cast<std::size_t>(tx)]
                            : narrow(tile.pixel(tx, ty));
                        float cover = 1.0f;
                        if (mask != nullptr) {
                            const std::uint8_t cov =
                                maskTile == nullptr ? mask->outside
                                : maskPx != nullptr
                                    ? maskPx[rowAt + static_cast<std::size_t>(tx)].r
                                    : narrowChannel(maskTile->pixel(tx, ty).r);
                            cover = static_cast<float>(cov) / 255.0f;
                        }
                        if (!ownAlpha.empty())
                            ownAlpha[at] = cover < 1.0f ? scale8(src.a, cover) : src.a;
                        if (src.a == 0) continue;
                        float scale = opacity * cover;
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
            own = narrow(tile->pixel(tx, ty));   // same narrowing as compositeLevel

        // The mask, in the same order and the same arithmetic compositeLevel
        // uses: the alpha published to the clip mask is the masked one, and
        // coverage multiplies into `scale` beside the opacity.
        float cover = 1.0f;
        if (layer->mask.has_value() && layer->mask->enabled)
            cover = static_cast<float>(maskCoverage(*layer->mask, x, y)) / 255.0f;
        const std::uint8_t ownAlpha = cover < 1.0f ? scale8(own.a, cover) : own.a;

        if (!layer->clipToBelow) clipAlpha = ownAlpha;
        if (!layer->visible || layer->opacity <= 0.0f || own.a == 0) continue;

        float scale = std::clamp(layer->opacity, 0.0f, 1.0f) * cover;
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

std::vector<PremulRgba8> CpuBackend::compositeRect(const Document& doc, std::int32_t x,
                                                   std::int32_t y, std::int32_t w,
                                                   std::int32_t h) {
    if (w <= 0 || h <= 0) return {};
    const Region r{x, y, static_cast<std::size_t>(w), static_cast<std::size_t>(h)};

    // Outside the document stays transparent, so an edge tile does not paint
    // background past the canvas bounds.
    std::vector<PremulRgba8> buf(r.w * r.h, PremulRgba8{});
    const PremulRgba8 bg = doc.background.premultiply();
    const std::int32_t x0 = std::max(x, 0), x1 = std::min(x + w, doc.width);
    const std::int32_t y0 = std::max(y, 0), y1 = std::min(y + h, doc.height);
    for (std::int32_t py = y0; py < y1; ++py) {
        const auto row = static_cast<std::ptrdiff_t>(
            static_cast<std::size_t>(py - y) * r.w);
        std::fill(buf.begin() + row + (x0 - x), buf.begin() + row + (x1 - x), bg);
    }

    compositeLevel(buf, doc, std::nullopt, r);
    return buf;
}

std::vector<StraightRgba8> flatten(const Document& doc) {
    return flatten(doc, paintBackend());
}

std::vector<StraightRgba8> flatten(const Document& doc, PaintBackend& backend) {
    const auto w = static_cast<std::size_t>(std::max(doc.width, 0));
    const auto h = static_cast<std::size_t>(std::max(doc.height, 0));
    if (w == 0 || h == 0) return {};

    // Composite in premultiplied space, convert once at the end. Doing it the
    // other way round is what produces the dark halo (D-004, US-07.3).
    const std::vector<PremulRgba8> buf =
        backend.compositeRect(doc, 0, 0, doc.width, doc.height);

    std::vector<StraightRgba8> out(w * h);
    for (std::size_t i = 0; i < buf.size(); ++i) out[i] = buf[i].unpremultiply();
    return out;
}

StraightRgba8 CpuBackend::pickColour(const Document& doc, std::int32_t x,
                                     std::int32_t y) {
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return StraightRgba8{};
    return pickLevel(doc, std::nullopt, x, y, doc.background.premultiply()).unpremultiply();
}

IccProfile iccProfileFromPng(const void* bytes, std::size_t size) {
    // 8 signature bytes, then chunks. Anything shorter is not a PNG, and
    // lodepng's chunk walkers assume they are given at least a chunk header.
    if (bytes == nullptr || size < 8 + 12) return {};
    const auto* png = static_cast<const unsigned char*>(bytes);
    const unsigned char* end = png + size;
    const unsigned char* chunk =
        lodepng_chunk_find_const(png + 8, end, "iCCP");
    if (chunk == nullptr) return {};

    const unsigned length = lodepng_chunk_length(chunk);
    const unsigned char* data = lodepng_chunk_data_const(chunk);
    // Compared as a size, not as `data + length > end`: the length is four
    // bytes some other program wrote, and forming the past-the-end pointer to
    // test it is undefined before it is false.
    if (length > static_cast<std::size_t>(end - data)) return {};

    // iCCP: a NUL-terminated name, one compression-method byte that must be 0,
    // then the zlib stream. A name running to the end of the chunk leaves no
    // profile, which is malformed rather than empty — either way, nothing.
    const auto* nul = static_cast<const unsigned char*>(
        std::memchr(data, 0, length));
    if (nul == nullptr) return {};
    const std::size_t headerLen = static_cast<std::size_t>(nul - data) + 2;
    if (headerLen >= length || nul[1] != 0) return {};

    unsigned char* out = nullptr;
    std::size_t outSize = 0;
    const unsigned rc = lodepng_zlib_decompress(
        &out, &outSize, data + headerLen, length - headerLen,
        &lodepng_default_decompress_settings);
    IccProfile profile;
    if (rc == 0 && out != nullptr) profile.data.assign(out, out + outSize);
    std::free(out);          // lodepng allocates with malloc, not new
    return profile;
}

std::expected<void, Error> exportPng(const Document& doc,
                                     const std::filesystem::path& path) {
    if (doc.width <= 0 || doc.height <= 0)
        return std::unexpected(Error{ErrorKind::Malformed, "canvas has no size"});

    // Exactly the canvas dimensions — not the viewport, and unaffected by zoom.
    const std::vector<StraightRgba8> pixels = flatten(doc);

    // A State rather than the four-argument encode, so the profile has
    // somewhere to go. The three lines below are what `lodepng_encode_memory`
    // sets for itself, so an untagged document still produces the byte-identical
    // PNG it produced before this existed (D-034) — including lodepng's
    // auto_convert, which is why a flat black-and-white export is still a small
    // greyscale file.
    lodepng::State state;
    state.info_raw.colortype       = LCT_RGBA;
    state.info_raw.bitdepth        = 8;
    state.info_png.color.colortype = LCT_RGBA;
    state.info_png.color.bitdepth  = 8;

    // No profile is written for an untagged document, deliberately. A bare PNG
    // already means sRGB by convention, so tagging one would change every
    // existing export's bytes to say what readers already assume — and #53's
    // first requirement is that nothing about existing work changes. A document
    // that actually carries a profile is the case where downstream was guessing.
    std::vector<unsigned char> profile;
    if (!doc.colourProfile.empty()) {
        profile = std::vector<unsigned char>(doc.colourProfile.data.begin(),
                                             doc.colourProfile.data.end());
        // lodepng requires a name of 1..79 printable bytes; "ICC profile" is
        // what libpng and Photoshop write and needs no escaping.
        lodepng_set_icc(&state.info_png, "ICC profile", profile.data(),
                        static_cast<unsigned>(profile.size()));
    }

    std::vector<unsigned char> png;
    unsigned rc = lodepng::encode(
        png, reinterpret_cast<const unsigned char*>(pixels.data()),
        static_cast<unsigned>(doc.width), static_cast<unsigned>(doc.height), state);
    if (rc == 0) rc = lodepng::save_file(png, path.string());
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
