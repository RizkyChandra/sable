#include "sbl/openraster.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "blit.hpp"
#include "lodepng.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "xml.hpp"

namespace sbl {
namespace {

std::unexpected<Error> fail(ErrorKind kind, std::string detail) {
    return std::unexpected(Error{kind, std::move(detail)});
}

// ------------------------------------------------------------- composite-op
// ORA names its blend modes after SVG's compositing operators, which is the
// same list as BlendMode with three spelling differences. Deriving the name
// from blendModeName() rather than tabulating all thirteen means a mode added
// to the enum cannot be silently forgotten here.

std::string oraComposite(BlendMode mode) {
    if (mode == BlendMode::Normal) return "svg:src-over";
    if (mode == BlendMode::Add)    return "svg:plus";

    std::string name(blendModeName(mode));
    if (const std::size_t at = name.find("colour"); at != std::string::npos)
        name.replace(at, 6, "color");     // SVG spells it the American way
    return "svg:" + name;
}

BlendMode blendFromOra(std::string_view op) {
    if (op.starts_with("svg:")) op.remove_prefix(4);
    if (op == "src-over") return BlendMode::Normal;
    if (op == "plus")     return BlendMode::Add;

    std::string name(op);
    if (const std::size_t at = name.find("color"); at != std::string::npos)
        name.replace(at, 5, "colour");
    // Everything else — src-atop, dst-in, Krita's own extensions — degrades to
    // Normal rather than failing the import.
    return blendModeFromName(name);
}

/// Nine significant digits is what a float round-trips through decimal in, and
/// the round trip is the point: an opacity that changes in the last bit
/// changes composited pixels, which is what the .ora round-trip test hashes.
std::string formatFloat(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.9g", static_cast<double>(value));
    return buffer;
}

std::string hexColour(StraightRgba8 c) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buffer;
}

StraightRgba8 parseHexColour(const std::string& text) {
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (std::sscanf(text.c_str(), "#%2x%2x%2x%2x", &r, &g, &b, &a) < 3) return {};
    return StraightRgba8{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                         static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
}

std::int32_t parseInt(const std::string& text) {
    return static_cast<std::int32_t>(std::strtol(text.c_str(), nullptr, 10));
}

/// The layers at one nesting level, bottom first — the same rule the
/// compositor uses (io.cpp), kept local rather than exported for one caller.
std::vector<const Layer*> levelOf(const Document& doc, std::optional<LayerId> parent) {
    std::vector<const Layer*> out;
    for (const Layer& layer : doc.layers)
        if (layer.parent == parent) out.push_back(&layer);
    return out;
}

struct Rect {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool empty() const noexcept { return w <= 0 || h <= 0; }
};

// ------------------------------------------------------------------- import

struct Importer {
    mz_zip_archive& zip;
    Document& doc;

    [[nodiscard]] std::string entry(const std::string& name) const {
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, name.c_str(), &size, 0);
        if (data == nullptr) return {};
        std::string out(static_cast<const char*>(data), size);
        mz_free(data);
        return out;
    }

    LayerId add(const XmlNode& element, std::optional<LayerId> parent, LayerKind kind,
                std::int32_t offsetX, std::int32_t offsetY) {
        Layer layer;
        layer.id   = doc.nextLayerId++;
        layer.kind = kind;
        layer.name = element.attributeOr(
            "name", kind == LayerKind::Folder ? "Group" : "Layer");
        layer.opacity = std::clamp(
            std::strtof(element.attributeOr("opacity", "1").c_str(), nullptr),
            0.0f, 1.0f);
        layer.blend   = blendFromOra(element.attributeOr("composite-op", "svg:src-over"));
        layer.visible = element.attributeOr("visibility", "visible") != "hidden";
        layer.parent  = parent;

        if (kind == LayerKind::Raster) {
            const std::string src = element.attributeOr("src", "");
            const std::string png = src.empty() ? std::string{} : entry(src);
            std::vector<unsigned char> straight;
            unsigned w = 0, h = 0;
            // A layer whose PNG is missing or corrupt arrives empty rather than
            // sinking the whole import — the same call as .sable's bad tile.
            if (!png.empty() &&
                lodepng::decode(straight, w, h,
                                reinterpret_cast<const unsigned char*>(png.data()),
                                png.size()) == 0) {
                blitStraightImage(layer, straight.data(), static_cast<std::int32_t>(w),
                                  static_cast<std::int32_t>(h),
                                  offsetX + parseInt(element.attributeOr("x", "0")),
                                  offsetY + parseInt(element.attributeOr("y", "0")),
                                  doc.width, doc.height);
                // An ORA layer is a rectangle, so much of what was written may
                // be transparent.
                dropBlankTiles(layer);
            }
        }

        const LayerId id = layer.id;
        doc.layers.push_back(std::move(layer));
        return id;
    }

    /// ORA lists the TOPMOST layer first; Document::layers is bottom first.
    ///
    /// A <stack> may carry an offset of its own, which moves everything inside
    /// it. A Sable folder has no offset, so it is pushed down into the children
    /// — the pixels land where the file says they should either way.
    void addStack(const XmlNode& stack, std::optional<LayerId> parent,
                  std::int32_t offsetX, std::int32_t offsetY) {
        for (auto child = stack.children.rbegin(); child != stack.children.rend(); ++child) {
            if (child->name == "layer") {
                add(*child, parent, LayerKind::Raster, offsetX, offsetY);
            } else if (child->name == "stack") {
                addStack(*child, add(*child, parent, LayerKind::Folder, 0, 0),
                         offsetX + parseInt(child->attributeOr("x", "0")),
                         offsetY + parseInt(child->attributeOr("y", "0")));
            }
            // Anything else (a <text> or <filter> element from a future
            // revision) is skipped: an unknown element must not lose the
            // layers around it.
        }
    }
};

// ------------------------------------------------------------------- export

/// The layer's non-transparent bounding box in canvas coordinates, clipped to
/// the canvas. Empty when the layer has no visible pixels at all.
Rect boundsOf(const Layer& layer, std::int32_t canvasW, std::int32_t canvasH) {
    std::int32_t minX = canvasW, minY = canvasH, maxX = -1, maxY = -1;
    for (const auto& [key, tile] : layer.tiles) {
        const std::int32_t originX = key.first  * TILE_SIZE;
        const std::int32_t originY = key.second * TILE_SIZE;
        const std::int32_t y0 = std::max<std::int32_t>(0, -originY);
        const std::int32_t y1 = std::min<std::int32_t>(TILE_SIZE, canvasH - originY);
        const std::int32_t x0 = std::max<std::int32_t>(0, -originX);
        const std::int32_t x1 = std::min<std::int32_t>(TILE_SIZE, canvasW - originX);

        for (std::int32_t y = y0; y < y1; ++y) {
            for (std::int32_t x = x0; x < x1; ++x) {
                // Depth-agnostic: this only asks whether a pixel is empty, and
                // an export scan is nowhere near the hot path.
                if (tile.pixel(x, y).a == 0) continue;
                minX = std::min(minX, originX + x);
                maxX = std::max(maxX, originX + x);
                minY = std::min(minY, originY + y);
                maxY = std::max(maxY, originY + y);
            }
        }
    }
    if (maxX < minX || maxY < minY) return {};
    return Rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

/// The layer's pixels over `r` as straight-alpha RGBA8, which is what a PNG
/// holds and what every other application expects (D-004).
///
/// A layer mask is MULTIPLIED INTO the alpha here (#48), because OpenRaster has
/// no mask element to write one into — its layer is a PNG and nothing else. So
/// the choice is between an .ora that looks like the painting and an .ora that
/// shows what the artist masked away, and D-027 already answered that one: the
/// drawing is what matters, and the `.sable` file keeps the editable mask.
std::vector<unsigned char> encodeRegion(const Layer& layer, Rect r) {
    std::vector<unsigned char> straight(
        static_cast<std::size_t>(r.w) * static_cast<std::size_t>(r.h) * 4, 0);

    for (const auto& [key, tile] : layer.tiles) {
        const std::int32_t originX = key.first  * TILE_SIZE;
        const std::int32_t originY = key.second * TILE_SIZE;
        const std::int32_t left   = std::max(r.x, originX);
        const std::int32_t top    = std::max(r.y, originY);
        const std::int32_t right  = std::min(r.x + r.w, originX + TILE_SIZE);
        const std::int32_t bottom = std::min(r.y + r.h, originY + TILE_SIZE);

        for (std::int32_t y = top; y < bottom; ++y) {
            for (std::int32_t x = left; x < right; ++x) {
                // ORA layers are 8-bit PNGs, so this is a real export boundary
                // and the narrowing belongs here (D-023). Unpremultiplied wide
                // and narrowed after, which is one rounding rather than two.
                const StraightRgba8 c =
                    narrow(tile.pixel(x - originX, y - originY).unpremultiply());
                const std::size_t at =
                    (static_cast<std::size_t>(y - r.y) * static_cast<std::size_t>(r.w) +
                     static_cast<std::size_t>(x - r.x)) * 4;
                straight[at + 0] = c.r;
                straight[at + 1] = c.g;
                straight[at + 2] = c.b;
                straight[at + 3] = c.a;
                // Straight alpha, so the colour channels stay as they are and
                // only the coverage moves — the same multiply the compositor
                // does, at the one boundary that cannot carry the mask itself.
                if (layer.mask.has_value() && layer.mask->enabled)
                    straight[at + 3] = static_cast<unsigned char>(
                        (static_cast<unsigned>(c.a) * maskCoverage(*layer.mask, x, y) +
                         127u) / 255u);
            }
        }
    }
    return straight;
}

/// Always 8-bit RGBA, never lodepng's automatic choice of a smaller colour
/// type. Krita takes the DOCUMENT's colour model from the layer PNGs, so a
/// black-and-white layer that lodepng helpfully stored as greyscale opens the
/// whole image as greyscale — verified against Krita 6.0.3, and it is exactly
/// the sort of thing that only shows up in the other application.
std::vector<unsigned char> encodePng(const std::vector<unsigned char>& straight,
                                     unsigned w, unsigned h) {
    lodepng::State state;
    state.info_raw.colortype       = LCT_RGBA;
    state.info_raw.bitdepth        = 8;
    state.info_png.color.colortype = LCT_RGBA;
    state.info_png.color.bitdepth  = 8;
    state.encoder.auto_convert     = 0;

    std::vector<unsigned char> png;
    if (lodepng::encode(png, straight, w, h, state) != 0) png.clear();
    return png;
}

struct Exporter {
    mz_zip_archive& zip;
    const Document& doc;
    std::string xml;
    int nextImage = 0;
    bool ok = true;

    void addFile(const std::string& name, const void* data, std::size_t size,
                 mz_uint flags) {
        if (ok && !mz_zip_writer_add_mem(&zip, name.c_str(), data, size, flags))
            ok = false;
    }

    void line(int depth, const std::string& text) {
        xml.append(static_cast<std::size_t>(depth), ' ');
        xml += text;
        xml += '\n';
    }

    /// name, opacity, visibility and composite-op are identical on <layer> and
    /// <stack>, so they are written once.
    [[nodiscard]] std::string commonAttributes(const Layer& layer) const {
        return " name=\"" + xmlEscape(layer.name) + "\""
               " opacity=\"" + formatFloat(std::clamp(layer.opacity, 0.0f, 1.0f)) + "\""
               " visibility=\"" + (layer.visible ? "visible" : "hidden") + "\""
               " composite-op=\"" + oraComposite(layer.blend) + "\"";
    }

    void writeLevel(std::optional<LayerId> parent, int depth) {
        const std::vector<const Layer*> level = levelOf(doc, parent);
        // Topmost first, which is ORA's document order and the reverse of ours.
        for (auto it = level.rbegin(); it != level.rend(); ++it) {
            const Layer& layer = **it;
            if (layer.kind == LayerKind::Folder) {
                // isolation="isolate": a Sable folder composites its children
                // into their own buffer before blending (io.cpp), and saying so
                // is what stops another application flattening the group.
                line(depth, "<stack" + commonAttributes(layer) +
                            " isolation=\"isolate\" x=\"0\" y=\"0\">");
                writeLevel(layer.id, depth + 1);
                line(depth, "</stack>");
                continue;
            }

            Rect bounds = boundsOf(layer, doc.width, doc.height);
            // A 1x1 transparent pixel: src is required, and an empty layer is
            // a normal thing to have in a stack.
            if (bounds.empty()) bounds = Rect{0, 0, 1, 1};

            const std::string src = "data/layer" + std::to_string(nextImage++) + ".png";
            const std::vector<unsigned char> png =
                encodePng(encodeRegion(layer, bounds),
                          static_cast<unsigned>(bounds.w), static_cast<unsigned>(bounds.h));
            if (png.empty()) { ok = false; return; }
            // Already deflated; deflating it again costs time for nothing (D-100).
            addFile(src, png.data(), png.size(), MZ_NO_COMPRESSION);

            line(depth, "<layer" + commonAttributes(layer) +
                        " src=\"" + src + "\""
                        " x=\"" + std::to_string(bounds.x) + "\""
                        " y=\"" + std::to_string(bounds.y) + "\"/>");
        }
    }
};

std::vector<unsigned char> straightBytes(const std::vector<StraightRgba8>& pixels) {
    std::vector<unsigned char> out(pixels.size() * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        out[i * 4 + 0] = pixels[i].r;
        out[i * 4 + 1] = pixels[i].g;
        out[i * 4 + 2] = pixels[i].b;
        out[i * 4 + 3] = pixels[i].a;
    }
    return out;
}

/// Nearest-neighbour, because this is a file-browser preview and not an export.
std::vector<unsigned char> thumbnailOf(const std::vector<unsigned char>& merged,
                                       unsigned w, unsigned h,
                                       unsigned& outW, unsigned& outH) {
    constexpr unsigned kMax = 256;
    const unsigned scale = std::max(1u, (std::max(w, h) + kMax - 1) / kMax);
    outW = std::max(1u, w / scale);
    outH = std::max(1u, h / scale);
    if (scale == 1) return merged;

    std::vector<unsigned char> small(static_cast<std::size_t>(outW) * outH * 4);
    for (unsigned y = 0; y < outH; ++y) {
        for (unsigned x = 0; x < outW; ++x) {
            const std::size_t from =
                (static_cast<std::size_t>(std::min(y * scale, h - 1)) * w +
                 std::min(x * scale, w - 1)) * 4;
            const std::size_t to = (static_cast<std::size_t>(y) * outW + x) * 4;
            std::copy_n(merged.begin() + static_cast<std::ptrdiff_t>(from), 4,
                        small.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
    return small;
}

}  // namespace

// ---------------------------------------------------------------------- read

std::expected<Document, Error> readOpenRaster(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return fail(ErrorKind::NotFound, path.string() + " does not exist");

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.string().c_str(), 0))
        return fail(ErrorKind::Malformed,
                    path.string() + " is not a readable ZIP archive, so it cannot "
                    "be an OpenRaster image");
    struct ZipGuard {
        mz_zip_archive* z;
        ~ZipGuard() { mz_zip_reader_end(z); }
    } guard{&zip};

    Document doc;
    Importer importer{zip, doc};

    const std::string manifest = importer.entry("stack.xml");
    if (manifest.empty())
        return fail(ErrorKind::Malformed, path.string() + " has no stack.xml");

    const std::optional<XmlNode> image = parseXml(manifest);
    if (!image.has_value() || image->name != "image")
        return fail(ErrorKind::Malformed,
                    "the stack.xml in " + path.string() + " is not a valid "
                    "OpenRaster manifest");

    doc.width  = parseInt(image->attributeOr("w", "0"));
    doc.height = parseInt(image->attributeOr("h", "0"));
    if (doc.width <= 0 || doc.height <= 0 || doc.width > 65536 || doc.height > 65536)
        return fail(ErrorKind::Malformed,
                    path.string() + " gives an implausible canvas size");
    doc.dpi = static_cast<std::uint32_t>(
        std::max<std::int32_t>(1, parseInt(image->attributeOr("xres", "72"))));

    // ORA has no notion of a canvas background, so an imported image keeps its
    // transparency. The attribute below is Sable's own, declared in its own
    // namespace so that other applications ignore it, and is what makes
    // Sable -> ORA -> Sable give back the document that went in.
    doc.background = StraightRgba8{0, 0, 0, 0};
    if (const std::string* background = image->attribute("sable:background"))
        doc.background = parseHexColour(*background);

    const XmlNode* root = image->child("stack");
    if (root == nullptr)
        return fail(ErrorKind::Malformed,
                    "the stack.xml in " + path.string() + " has no root stack");
    importer.addStack(*root, std::nullopt,
                      parseInt(root->attributeOr("x", "0")),
                      parseInt(root->attributeOr("y", "0")));

    if (doc.layers.empty())
        return fail(ErrorKind::Malformed, path.string() + " has no layers");

    doc.activeLayer = doc.layers.back().id;
    doc.dirty = false;
    return doc;
}

// --------------------------------------------------------------------- write

std::expected<void, Error> writeOpenRaster(const Document& doc,
                                           const std::filesystem::path& path) {
    if (doc.width <= 0 || doc.height <= 0)
        return fail(ErrorKind::Malformed, "canvas has no size");

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, path.string().c_str(), 0))
        return fail(ErrorKind::Permission, "could not create " + path.string());

    const auto abort = [&](ErrorKind kind, const std::string& detail) {
        mz_zip_writer_end(&zip);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return fail(kind, detail);
    };

    // First, and stored rather than deflated: that is what lets `file` and
    // every desktop's MIME sniffer recognise an .ora by its content.
    static constexpr char kMime[] = "image/openraster";
    if (!mz_zip_writer_add_mem(&zip, "mimetype", kMime, sizeof kMime - 1,
                               MZ_NO_COMPRESSION))
        return abort(ErrorKind::Io, "could not write the mimetype to " + path.string());

    Exporter exporter{zip, doc, {}, 0, true};
    exporter.line(0, "<?xml version='1.0' encoding='UTF-8'?>");
    exporter.line(0, "<image version=\"0.0.3\""
                     " w=\"" + std::to_string(doc.width) + "\""
                     " h=\"" + std::to_string(doc.height) + "\""
                     " xres=\"" + std::to_string(doc.dpi) + "\""
                     " yres=\"" + std::to_string(doc.dpi) + "\""
                     " xmlns:sable=\"https://sable.paint/ns/ora\""
                     " sable:background=\"" + hexColour(doc.background) + "\">");
    exporter.line(1, "<stack>");
    exporter.writeLevel(std::nullopt, 2);
    exporter.line(1, "</stack>");
    exporter.line(0, "</image>");
    if (!exporter.ok)
        return abort(ErrorKind::Io, "could not write the layers to " + path.string());

    // mergedimage.png IS flatten(): the composite other applications show
    // before they parse the stack must be the one Sable draws (#1).
    const std::vector<unsigned char> merged = straightBytes(flatten(doc));
    const auto width  = static_cast<unsigned>(doc.width);
    const auto height = static_cast<unsigned>(doc.height);
    const std::vector<unsigned char> mergedPng = encodePng(merged, width, height);
    if (mergedPng.empty())
        return abort(ErrorKind::Io, "could not encode the merged image");
    exporter.addFile("mergedimage.png", mergedPng.data(), mergedPng.size(),
                     MZ_NO_COMPRESSION);

    unsigned thumbW = 0, thumbH = 0;
    const std::vector<unsigned char> thumb =
        thumbnailOf(merged, width, height, thumbW, thumbH);
    const std::vector<unsigned char> thumbPng = encodePng(thumb, thumbW, thumbH);
    if (!thumbPng.empty())
        exporter.addFile("Thumbnails/thumbnail.png", thumbPng.data(), thumbPng.size(),
                         MZ_NO_COMPRESSION);

    exporter.addFile("stack.xml", exporter.xml.data(), exporter.xml.size(),
                     MZ_DEFAULT_COMPRESSION);
    if (!exporter.ok)
        return abort(ErrorKind::Io, "could not finish writing " + path.string());

    if (!mz_zip_writer_finalize_archive(&zip))
        return abort(ErrorKind::Io, "could not finalise " + path.string());
    mz_zip_writer_end(&zip);
    return {};
}

}  // namespace sbl
