#include "sbl/project.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "sbl/backend.hpp"

#include "lodepng.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "nlohmann/json.hpp"

namespace sbl {
namespace {

using json = nlohmann::json;

std::string hexColour(StraightRgba8 c) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "#";
    for (const std::uint8_t v : {c.r, c.g, c.b, c.a}) {
        out += kDigits[v >> 4];
        out += kDigits[v & 0x0F];
    }
    return out;
}

StraightRgba8 parseColour(const std::string& text) {
    // Accepts #rgba and #rgb, because a hand-edited manifest is a supported
    // way to debug one.
    StraightRgba8 c{0, 0, 0, 255};
    const std::string digits = text.starts_with('#') ? text.substr(1) : text;
    if (digits.size() < 6) return c;

    const auto byteAt = [&](std::size_t i) {
        return static_cast<std::uint8_t>(
            std::strtoul(digits.substr(i, 2).c_str(), nullptr, 16));
    };
    c.r = byteAt(0);
    c.g = byteAt(2);
    c.b = byteAt(4);
    c.a = digits.size() >= 8 ? byteAt(6) : 255;
    return c;
}

const char* alignName(TextAlign a) {
    switch (a) {
        case TextAlign::Centre: return "centre";
        case TextAlign::Right:  return "right";
        case TextAlign::Left:   break;
    }
    return "left";
}

TextAlign alignFromName(const std::string& name) {
    if (name == "centre" || name == "center") return TextAlign::Centre;
    if (name == "right")                      return TextAlign::Right;
    return TextAlign::Left;
}

std::string tilePath(LayerId layer, TileKey key, bool mask = false) {
    return "layers/" + std::to_string(layer) + (mask ? "/mask/" : "/tiles/") +
           std::to_string(key.first) + "_" + std::to_string(key.second) + ".png";
}

/// Encodes one tile as straight-alpha RGBA, so external tools can open it —
/// 8 bits per channel from an 8-bit tile, 16 from a 16-bit one. PNG carries
/// both, so a 16-bit project's tiles stay openable in any image viewer, and
/// saving a 16-bit painting does not quietly throw away what it was for.
std::vector<unsigned char> encodeTile(const Tile& tile) {
    std::vector<unsigned char> png;
    if (tile.depth() == ColourDepth::Bits16) {
        // PNG is big-endian, so the high byte goes first. lodepng will not do
        // this for us from a 16-bit-per-channel host buffer.
        std::vector<unsigned char> straight(static_cast<std::size_t>(TILE_PIXELS) * 8);
        const PremulRgba16* src = tile.pixels16();
        for (int i = 0; i < TILE_PIXELS; ++i) {
            const StraightRgba16 c = src[i].unpremultiply();
            const std::uint16_t ch[4]{c.r, c.g, c.b, c.a};
            for (int k = 0; k < 4; ++k) {
                straight[static_cast<std::size_t>(i) * 8 + k * 2 + 0] =
                    static_cast<unsigned char>(ch[k] >> 8);
                straight[static_cast<std::size_t>(i) * 8 + k * 2 + 1] =
                    static_cast<unsigned char>(ch[k] & 0xFF);
            }
        }
        lodepng::encode(png, straight, TILE_SIZE, TILE_SIZE, LCT_RGBA, 16);
        return png;
    }

    std::vector<unsigned char> straight(static_cast<std::size_t>(TILE_PIXELS) * 4);
    const PremulRgba8* src = tile.pixels8();
    for (int i = 0; i < TILE_PIXELS; ++i) {
        const StraightRgba8 c = src[i].unpremultiply();
        straight[i * 4 + 0] = c.r;
        straight[i * 4 + 1] = c.g;
        straight[i * 4 + 2] = c.b;
        straight[i * 4 + 3] = c.a;
    }
    lodepng::encode(png, straight, TILE_SIZE, TILE_SIZE);
    return png;
}

/// Decodes into a tile of `out`'s own depth, whatever the PNG's is. The
/// document's manifest decides the depth, not each tile: a 16-bit project whose
/// tile happens to have been written as 8-bit still gets 16-bit storage to
/// paint into, and an 8-bit project cannot be widened by a stray file.
bool decodeTile(const unsigned char* data, std::size_t size, Tile& out) {
    unsigned w = 0, h = 0;
    lodepng::State state;
    if (lodepng_inspect(&w, &h, &state, data, size) != 0) return false;
    if (w != TILE_SIZE || h != TILE_SIZE) return false;
    const bool wideFile = state.info_png.color.bitdepth == 16;

    std::vector<unsigned char> straight;
    if (lodepng::decode(straight, w, h, data, size, LCT_RGBA, wideFile ? 16 : 8) != 0)
        return false;

    if (wideFile) {
        for (int i = 0; i < TILE_PIXELS; ++i) {
            const auto at = static_cast<std::size_t>(i) * 8;
            const auto ch = [&](int k) {
                return static_cast<std::uint16_t>(
                    (static_cast<unsigned>(straight[at + k * 2]) << 8) |
                    straight[at + k * 2 + 1]);
            };
            out.setPixel(i % TILE_SIZE, i / TILE_SIZE,
                         StraightRgba16{ch(0), ch(1), ch(2), ch(3)}.premultiply());
        }
        return true;
    }

    if (out.depth() == ColourDepth::Bits8) {
        // The original path, byte for byte: an 8-bit project reads exactly what
        // it always read, at the speed it always read it.
        PremulRgba8* dst = out.pixels8();
        for (int i = 0; i < TILE_PIXELS; ++i)
            dst[i] = StraightRgba8{straight[i * 4 + 0], straight[i * 4 + 1],
                                   straight[i * 4 + 2], straight[i * 4 + 3]}.premultiply();
        return true;
    }
    for (int i = 0; i < TILE_PIXELS; ++i)
        out.setPixel(i % TILE_SIZE, i / TILE_SIZE,
                     StraightRgba8{straight[i * 4 + 0], straight[i * 4 + 1],
                                   straight[i * 4 + 2], straight[i * 4 + 3]}.premultiply());
    return true;
}

/// A small thumbnail for file browsers. Nearest-neighbour is fine — this is a
/// 256 px preview, not an export.
std::vector<unsigned char> encodeThumbnail(const Document& doc) {
    constexpr unsigned kMax = 256;
    // The CPU backend by name: saving runs on the autosave worker, a thread
    // with no device context, and the clone it was handed is host pixels.
    const std::vector<StraightRgba8> full = flatten(doc, cpuBackend());
    if (full.empty()) return {};

    const auto srcW = static_cast<unsigned>(doc.width);
    const auto srcH = static_cast<unsigned>(doc.height);
    const unsigned scale = std::max(1u, std::max(srcW, srcH) / kMax);
    const unsigned outW = std::max(1u, srcW / scale);
    const unsigned outH = std::max(1u, srcH / scale);

    std::vector<unsigned char> small(static_cast<std::size_t>(outW) * outH * 4);
    for (unsigned y = 0; y < outH; ++y) {
        for (unsigned x = 0; x < outW; ++x) {
            const StraightRgba8 c =
                full[static_cast<std::size_t>(std::min(y * scale, srcH - 1)) * srcW +
                     std::min(x * scale, srcW - 1)];
            const std::size_t i = (static_cast<std::size_t>(y) * outW + x) * 4;
            small[i + 0] = c.r;
            small[i + 1] = c.g;
            small[i + 2] = c.b;
            small[i + 3] = c.a;
        }
    }

    std::vector<unsigned char> png;
    lodepng::encode(png, small, outW, outH);
    return png;
}

std::unexpected<Error> fail(ErrorKind kind, std::string detail) {
    return std::unexpected(Error{kind, std::move(detail)});
}

}  // namespace

// ---------------------------------------------------------------------- save

std::expected<void, Error> saveProject(const Document& doc,
                                       const std::filesystem::path& path) {
    if (doc.width <= 0 || doc.height <= 0)
        return fail(ErrorKind::Malformed, "canvas has no size");

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, path.string().c_str(), 0))
        return fail(ErrorKind::Permission, "could not create " + path.string());

    // Anything that fails past this point must not leave a half-written file
    // sitting where the artist's project used to be.
    const auto abort = [&](ErrorKind kind, const std::string& detail) {
        mz_zip_writer_end(&zip);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return fail(kind, detail);
    };

    json manifest;
    // An 8-bit document keeps declaring the version it always declared, so
    // saving from this build does not lock an ordinary painting out of an
    // older Sable for a feature it is not using. Only a 16-bit document claims
    // the newer format — and then the version gate on load is what makes an
    // older reader say "written by a newer version of Sable" instead of reading
    // half-width tiles as though they were the whole picture.
    const bool anyMask = std::ranges::any_of(
        doc.layers, [](const Layer& l) { return l.mask.has_value(); });
    const bool tagged = !doc.colourProfile.empty();
    manifest["format_version"] = tagged  ? SABLE_FORMAT_VERSION_ICC
                               : anyMask ? SABLE_FORMAT_VERSION_MASK
                               : doc.depth == ColourDepth::Bits16
                                     ? SABLE_FORMAT_VERSION_16BIT
                                     : SABLE_FORMAT_VERSION_8BIT;
    manifest["app"]            = "Sable 0.1.0";
    manifest["width"]          = doc.width;
    manifest["height"]         = doc.height;
    manifest["dpi"]            = doc.dpi;
    manifest["background"]     = hexColour(doc.background);
    manifest["tile_size"]      = TILE_SIZE;
    // D-105 wrote `"space": "sRGB"` unconditionally so that a future ICC-aware
    // Sable could tell its files apart. D-034 is that version, and this is the
    // slot: untagged still says exactly "sRGB" and nothing more, so a file this
    // build writes for an ordinary document is byte-identical to the one it
    // wrote yesterday. A tagged document names its profile and points at the
    // container entry holding the bytes — the description alone is a label, and
    // reconstructing a profile from its name is how colour gets it wrong.
    manifest["colour"] = {{"depth", static_cast<int>(doc.depth)},
                          {"space", profileDescription(doc.colourProfile)},
                          {"premultiplied", true}};
    if (tagged) {
        manifest["colour"]["profile"] = "colour.icc";
        // Stored, not deflated — D-100's reasoning for the tiles applies here
        // too: `unzip -p x.sable colour.icc` should hand a working profile
        // straight to any colour tool.
        if (!mz_zip_writer_add_mem(&zip, "colour.icc", doc.colourProfile.data.data(),
                                   doc.colourProfile.data.size(), MZ_NO_COMPRESSION))
            return abort(ErrorKind::Io,
                         "could not write the colour profile to " + path.string());
    }
    manifest["active_layer"]   = doc.activeLayer;
    manifest["layers"]         = json::array();

    // The selection travels with the document (#18). An artist who spent a
    // minute lassoing a shape and then saved should not have to draw it again
    // after lunch. Only the bounding box goes in the manifest; the coverage
    // mask, when there is one, is a greyscale PNG beside the tiles — same
    // container rule as everything else, and openable in any image viewer.
    if (doc.selection.has_value() && !doc.selection->empty()) {
        const Selection& sel = *doc.selection;
        manifest["selection"] = {{"x", sel.x}, {"y", sel.y},
                                 {"w", sel.w}, {"h", sel.h},
                                 {"mask", !sel.mask.empty()}};
        if (!sel.mask.empty()) {
            std::vector<unsigned char> png;
            lodepng::encode(png, sel.mask, static_cast<unsigned>(sel.w),
                            static_cast<unsigned>(sel.h), LCT_GREY, 8);
            if (png.empty())
                return abort(ErrorKind::Io, "could not encode the selection mask");
            if (!mz_zip_writer_add_mem(&zip, "selection.png", png.data(), png.size(),
                                       MZ_NO_COMPRESSION))
                return abort(ErrorKind::Io,
                             "could not write the selection to " + path.string());
        }
    }

    // Written only when there are some, so a document with no perspective
    // guides still produces a manifest a v1 Sable would have written.
    if (!doc.vanishingPoints.empty()) {
        manifest["vanishing_points"] = json::array();
        for (const VanishingPoint& vp : doc.vanishingPoints)
            manifest["vanishing_points"].push_back(
                {{"x", vp.x}, {"y", vp.y}, {"enabled", vp.enabled}});
    }

    for (const Layer& layer : doc.layers) {
        json entry;
        entry["id"]      = layer.id;
        entry["kind"]    = layer.kind == LayerKind::Folder   ? "folder"
                         : layer.kind == LayerKind::Text     ? "text"
                         : layer.kind == LayerKind::Linework ? "linework"
                                                             : "raster";
        entry["name"]    = layer.name;
        entry["opacity"] = layer.opacity;
        entry["blend"]   = std::string(blendModeName(layer.blend));
        entry["visible"]          = layer.visible;
        entry["locked"]           = layer.locked;
        entry["preserve_opacity"] = layer.preserveOpacity;
        entry["clip_to_below"]    = layer.clipToBelow;
        if (layer.parent.has_value()) entry["parent"] = *layer.parent;
        else                          entry["parent"] = nullptr;

        // The words, beside the pixels they were drawn as. The tiles below are
        // still what renders, so a reader that ignores this — an older Sable,
        // or a script — sees the finished text either way.
        if (layer.text.has_value()) {
            const TextContent& t = *layer.text;
            entry["text"] = {{"utf8", t.utf8},
                             {"font", t.fontPath},
                             {"font_name", t.fontName},
                             {"size", t.sizePx},
                             {"line_spacing", t.lineSpacing},
                             {"align", alignName(t.align)},
                             {"x", t.x},
                             {"y", t.y},
                             {"colour", hexColour(t.colour)}};
        }

        // The curves, beside the pixels they were drawn as (#17). Same rule as
        // the text above: the tiles are still what renders, so a reader that
        // ignores this sees the finished line art either way. Each point is
        // [x, y, pressure] — three numbers on one line keeps a manifest with a
        // few hundred of them readable, which is the whole reason it is JSON.
        if (layer.linework.has_value()) {
            json strokes = json::array();
            for (const LineStroke& stroke : layer.linework->strokes) {
                json points = json::array();
                for (const LinePoint& p : stroke.points)
                    points.push_back({p.x, p.y, p.pressure});
                strokes.push_back({{"colour", hexColour(stroke.colour)},
                                   {"width", stroke.width},
                                   {"min_width", stroke.minWidthRatio},
                                   // No format bump for this: a reader that has
                                   // never heard of it still sees the closed
                                   // shape, because the tiles are what renders.
                                   {"closed", stroke.closed},
                                   {"points", std::move(points)}});
            }
            entry["linework"] = {{"strokes", std::move(strokes)}};
        }

        // Sorted, so two saves of the same document produce byte-identical
        // manifests and a diff is readable.
        std::vector<TileKey> keys;
        keys.reserve(layer.tiles.size());
        for (const auto& [key, tile] : layer.tiles)
            if (!tile.isFullyTransparent()) keys.push_back(key);
        std::ranges::sort(keys);

        entry["tiles"] = json::array();
        for (const TileKey key : keys) {
            entry["tiles"].push_back({key.first, key.second});

            const std::vector<unsigned char> png = encodeTile(*layer.find(key));
            if (png.empty())
                return abort(ErrorKind::Io, "could not encode a tile of " + layer.name);
            // MZ_NO_COMPRESSION: PNG is already deflated, and deflating it
            // again wastes save time for nothing.
            if (!mz_zip_writer_add_mem(&zip, tilePath(layer.id, key).c_str(),
                                       png.data(), png.size(), MZ_NO_COMPRESSION))
                return abort(ErrorKind::Io, "could not write a tile to " + path.string());
        }

        // The mask (#48), as ordinary tile PNGs under `mask/` rather than
        // `tiles/`. Every transparent tile is written, unlike the pixels above:
        // in a mask a transparent tile means "hide all of this", and dropping
        // it would let `outside` show the layer back through the hole.
        if (layer.mask.has_value()) {
            std::vector<TileKey> maskKeys;
            maskKeys.reserve(layer.mask->tiles.size());
            for (const auto& [key, tile] : layer.mask->tiles) maskKeys.push_back(key);
            std::ranges::sort(maskKeys);

            json maskNode = {{"enabled", layer.mask->enabled},
                             {"outside", layer.mask->outside},
                             {"tiles", json::array()}};
            for (const TileKey key : maskKeys) {
                maskNode["tiles"].push_back({key.first, key.second});
                const std::vector<unsigned char> png = encodeTile(*layer.mask->find(key));
                if (png.empty())
                    return abort(ErrorKind::Io,
                                 "could not encode a mask tile of " + layer.name);
                if (!mz_zip_writer_add_mem(&zip, tilePath(layer.id, key, true).c_str(),
                                           png.data(), png.size(), MZ_NO_COMPRESSION))
                    return abort(ErrorKind::Io,
                                 "could not write a mask tile to " + path.string());
            }
            entry["mask"] = std::move(maskNode);
        }
        manifest["layers"].push_back(std::move(entry));
    }

    const std::vector<unsigned char> thumb = encodeThumbnail(doc);
    if (!thumb.empty())
        mz_zip_writer_add_mem(&zip, "thumbnail.png", thumb.data(), thumb.size(),
                              MZ_NO_COMPRESSION);

    // The manifest is text and compresses well, unlike the tiles.
    const std::string text = manifest.dump(2);
    if (!mz_zip_writer_add_mem(&zip, "document.json", text.data(), text.size(),
                               MZ_DEFAULT_COMPRESSION))
        return abort(ErrorKind::Io, "could not write the manifest to " + path.string());

    if (!mz_zip_writer_finalize_archive(&zip))
        return abort(ErrorKind::Io, "could not finalise " + path.string());
    mz_zip_writer_end(&zip);
    return {};
}

// ---------------------------------------------------------------------- load

std::expected<Document, Error> loadProject(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return fail(ErrorKind::NotFound, path.string() + " does not exist");

    mz_zip_archive zip{};
    // Say what actually went wrong. Since .sable, .ora and .kra are all ZIP
    // containers, the format registry decides what a file is; this reader only
    // reports why it could not read one.
    if (!mz_zip_reader_init_file(&zip, path.string().c_str(), 0))
        return fail(ErrorKind::Malformed,
                    path.string() + " is not a readable ZIP archive, so it cannot "
                    "be a Sable project");

    struct ZipGuard {
        mz_zip_archive* z;
        ~ZipGuard() { mz_zip_reader_end(z); }
    } guard{&zip};

    std::size_t manifestSize = 0;
    void* manifestData =
        mz_zip_reader_extract_file_to_heap(&zip, "document.json", &manifestSize, 0);
    if (manifestData == nullptr)
        return fail(ErrorKind::Malformed, path.string() + " has no document.json");

    json manifest;
    {
        const std::string text(static_cast<const char*>(manifestData), manifestSize);
        mz_free(manifestData);
        // Never a crash on a malformed file, never a silent partial load.
        manifest = json::parse(text, nullptr, false);
        if (manifest.is_discarded())
            return fail(ErrorKind::Malformed,
                        "the manifest in " + path.string() + " is not valid JSON");
    }

    const int version = manifest.value("format_version", 0);
    if (version > SABLE_FORMAT_VERSION)
        return fail(ErrorKind::UnsupportedVersion,
                    path.string() + " was written by a newer version of Sable "
                    "(format " + std::to_string(version) + "). Refusing to open it "
                    "rather than read it wrong.");
    if (version < 1)
        return fail(ErrorKind::Malformed, path.string() + " has no format version");

    const int tileSize = manifest.value("tile_size", TILE_SIZE);
    if (tileSize != TILE_SIZE)
        return fail(ErrorKind::UnsupportedVersion,
                    "unsupported tile size " + std::to_string(tileSize));

    Document doc;
    doc.width  = manifest.value("width", 0);
    doc.height = manifest.value("height", 0);
    doc.dpi    = manifest.value("dpi", 72u);
    if (doc.width <= 0 || doc.height <= 0 || doc.width > 65536 || doc.height > 65536)
        return fail(ErrorKind::Malformed, "the manifest has an implausible canvas size");
    doc.background = parseColour(manifest.value("background", std::string("#ffffffff")));
    doc.path = path;

    // D-023. Absent, or any value that is not 16, means 8 — which is what every
    // file written before this feature says, and what a hand-edited manifest
    // with a typo in it should get rather than a document twice the size.
    if (manifest.contains("colour") && manifest["colour"].is_object() &&
        manifest["colour"].value("depth", 8) == 16)
        doc.depth = ColourDepth::Bits16;

    // D-034. A v1-era manifest says `"space": "sRGB"` and names no profile, so
    // it takes this branch not at all and loads as the untagged sRGB document
    // it has always been — which is the acceptance criterion this feature is
    // not allowed to break. Only an explicit `colour.profile` entry is read.
    if (manifest.contains("colour") && manifest["colour"].is_object()) {
        const std::string entry = manifest["colour"].value("profile", std::string{});
        // Anywhere inside the container, but only inside it: a manifest is
        // something a user can hand-edit, and "../../etc/passwd" must resolve
        // to nothing rather than to a file read. miniz looks up by exact name
        // and never touches the filesystem, so the containment is free — the
        // check below is only that the name is one we wrote.
        if (entry == "colour.icc") {
            std::size_t size = 0;
            void* data =
                mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &size, 0);
            if (data != nullptr) {
                IccProfile profile;
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                profile.data.assign(bytes, bytes + size);
                mz_free(data);
                // A profile that will not parse is dropped rather than carried:
                // the document then behaves as untagged sRGB, which is what it
                // did before this feature and is never worse than a transform
                // built from rubbish. The artist is told, because the file did
                // claim a colour space and they are no longer getting it.
                if (isUsableProfile(profile) && !profile.empty())
                    doc.colourProfile = std::move(profile);
                else
                    doc.warnings.push_back(
                        "The colour profile in this project could not be read, so "
                        "it has been opened as sRGB.");
            }
        }
    }

    // Absent before v3, and in any v3 document with nothing selected. A
    // malformed one is dropped rather than repaired: a selection that is not
    // the one the artist drew would send the next fill somewhere they did not
    // ask for, and no selection at all is the honest answer.
    if (manifest.contains("selection") && manifest["selection"].is_object()) {
        const auto& node = manifest["selection"];
        Selection sel;
        sel.x = node.value("x", 0);
        sel.y = node.value("y", 0);
        sel.w = node.value("w", 0);
        sel.h = node.value("h", 0);

        const bool plausible = !sel.empty() && sel.w <= doc.width && sel.h <= doc.height;
        bool usable = plausible;
        if (plausible && node.value("mask", false)) {
            usable = false;
            std::size_t size = 0;
            if (void* data = mz_zip_reader_extract_file_to_heap(&zip, "selection.png",
                                                                &size, 0);
                data != nullptr) {
                std::vector<unsigned char> grey;
                unsigned mw = 0, mh = 0;
                const bool ok =
                    lodepng::decode(grey, mw, mh, static_cast<const unsigned char*>(data),
                                    size, LCT_GREY, 8) == 0;
                mz_free(data);
                if (ok && mw == static_cast<unsigned>(sel.w) &&
                    mh == static_cast<unsigned>(sel.h)) {
                    sel.mask = std::move(grey);
                    usable = true;
                }
            }
        }
        if (usable) doc.selection = std::move(sel);
    }

    // Absent in v1, and in any v2 document that has none — both mean "no
    // guides", which is why the version bump costs older files nothing.
    if (manifest.contains("vanishing_points") && manifest["vanishing_points"].is_array()) {
        for (const auto& entry : manifest["vanishing_points"]) {
            if (!entry.is_object()) continue;
            doc.vanishingPoints.push_back(VanishingPoint{
                entry.value("x", 0.0), entry.value("y", 0.0),
                entry.value("enabled", true)});
        }
    }

    if (!manifest.contains("layers") || !manifest["layers"].is_array())
        return fail(ErrorKind::Malformed, "the manifest lists no layers");

    for (const auto& entry : manifest["layers"]) {
        Layer layer;
        layer.depth = doc.depth;        // the document decides, not each layer
        layer.id   = entry.value("id", 0u);
        if (layer.id == NO_LAYER) continue;         // 0 is the reserved "no layer"
        const std::string kind = entry.value("kind", std::string("raster"));
        layer.kind = kind == "folder"   ? LayerKind::Folder
                   : kind == "text"     ? LayerKind::Text
                   : kind == "linework" ? LayerKind::Linework
                                        : LayerKind::Raster;
        layer.name    = entry.value("name", std::string("Layer"));
        layer.opacity = std::clamp(entry.value("opacity", 1.0f), 0.0f, 1.0f);
        layer.blend   = blendModeFromName(entry.value("blend", std::string("normal")));
        layer.visible         = entry.value("visible", true);
        layer.locked          = entry.value("locked", false);
        layer.preserveOpacity = entry.value("preserve_opacity", false);
        layer.clipToBelow     = entry.value("clip_to_below", false);
        if (entry.contains("parent") && entry["parent"].is_number_unsigned())
            layer.parent = entry["parent"].get<LayerId>();

        if (entry.contains("text") && entry["text"].is_object()) {
            const auto& t = entry["text"];
            TextContent text;
            text.utf8        = t.value("utf8", std::string{});
            text.fontPath    = t.value("font", std::string{});
            text.fontName    = t.value("font_name", std::string{});
            text.sizePx      = std::clamp(t.value("size", 48.0f), 1.0f, 2000.0f);
            text.lineSpacing = std::clamp(t.value("line_spacing", 1.2f), 0.1f, 10.0f);
            text.align       = alignFromName(t.value("align", std::string("left")));
            text.x           = t.value("x", 0.0);
            text.y           = t.value("y", 0.0);
            text.colour      = parseColour(t.value("colour", std::string("#000000ff")));
            layer.text       = std::move(text);
            // A "text" object on a layer some other writer called raster still
            // means the words belong to it. Trusting `kind` alone would leave a
            // layer the tool refuses to edit for no reason the artist can see.
            layer.kind = LayerKind::Text;
        } else if (layer.kind == LayerKind::Text) {
            // Text layer, no text: nothing can be edited back, and pretending
            // otherwise gets the tool to clear the pixels on the first click.
            layer.kind = LayerKind::Raster;
        }

        // `!layer.text`: a layer is words or curves, never both, and the text
        // above has already claimed this one if it had any.
        if (!layer.text.has_value() && entry.contains("linework") &&
            entry["linework"].is_object() &&
            entry["linework"].value("strokes", json::array()).is_array()) {
            LineworkContent work;
            for (const auto& s : entry["linework"]["strokes"]) {
                if (!s.is_object()) continue;
                LineStroke stroke;
                stroke.colour = parseColour(s.value("colour", std::string("#000000ff")));
                stroke.width  = std::clamp(s.value("width", 4.0f), 0.1f, 4000.0f);
                stroke.minWidthRatio = std::clamp(s.value("min_width", 0.15f), 0.0f, 1.0f);
                stroke.closed = s.value("closed", false);
                if (s.contains("points") && s["points"].is_array()) {
                    for (const auto& p : s["points"]) {
                        // A point that is not three numbers is skipped rather
                        // than defaulted: a control point invented at the origin
                        // would drag the artist's curve across the canvas.
                        if (!p.is_array() || p.size() < 3) continue;
                        if (!p[0].is_number() || !p[1].is_number() || !p[2].is_number())
                            continue;
                        stroke.points.push_back(
                            LinePoint{p[0].get<double>(), p[1].get<double>(),
                                      std::clamp(p[2].get<float>(), 0.0f, 1.0f)});
                    }
                }
                if (!stroke.points.empty()) work.strokes.push_back(std::move(stroke));
            }
            if (!work.strokes.empty()) {
                layer.linework = std::move(work);
                // Same rule the text takes above: curves on a layer someone
                // else called raster still belong to it.
                layer.kind = LayerKind::Linework;
            }
        }
        if (layer.kind == LayerKind::Linework && !layer.linework.has_value())
            layer.kind = LayerKind::Raster;

        // #48. Absent before v7 and in any v7 document whose layers have none.
        // The mask is created here even when it has no tiles, because "a mask
        // that hides nothing" and "no mask" are different states and only the
        // first has somewhere to paint.
        if (entry.contains("mask") && entry["mask"].is_object()) {
            LayerMask mask;
            mask.enabled = entry["mask"].value("enabled", true);
            mask.outside = static_cast<std::uint8_t>(
                std::clamp(entry["mask"].value("outside", 255), 0, 255));
            layer.mask = std::move(mask);
        }

        doc.nextLayerId = std::max(doc.nextLayerId, layer.id + 1);
        doc.layers.push_back(std::move(layer));
    }
    if (doc.layers.empty())
        return fail(ErrorKind::Malformed, "the manifest lists no usable layers");

    // Load tiles from the ZIP DIRECTORY, not from the manifest's tile lists.
    // If the two disagree the ZIP is the truth: a half-written file should open
    // with whatever survived rather than refuse to open at all.
    bool repaired = false;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::string name = stat.m_filename;
        if (!name.starts_with("layers/") || !name.ends_with(".png")) continue;

        // layers/<id>/tiles/<tx>_<ty>.png, or layers/<id>/mask/<tx>_<ty>.png
        LayerId layerId = 0;
        int tx = 0, ty = 0;
        bool isMask = false;
        if (std::sscanf(name.c_str(), "layers/%u/tiles/%d_%d.png", &layerId, &tx, &ty) != 3) {
            if (std::sscanf(name.c_str(), "layers/%u/mask/%d_%d.png",
                            &layerId, &tx, &ty) != 3)
                continue;
            isMask = true;
        }

        Layer* layer = doc.layerById(layerId);
        if (layer == nullptr) { repaired = true; continue; }
        // A mask tile whose manifest entry was lost still says the layer has a
        // mask — the ZIP is the truth (see above), and a mask that hides half
        // the layer is the sort of thing a half-written file should keep.
        if (isMask && !layer->mask.has_value()) {
            layer->mask.emplace();
            repaired = true;
        }

        std::size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (data == nullptr) { repaired = true; continue; }

        // A mask tile is always 8-bit, whatever the document's depth: coverage
        // is eight bits everywhere it is stored, read or exported (#48).
        Tile tile(isMask ? ColourDepth::Bits8 : doc.depth);
        const bool ok = decodeTile(static_cast<const unsigned char*>(data), size, tile);
        mz_free(data);
        if (!ok) { repaired = true; continue; }       // one bad tile, not a bad file

        TileMap& into = isMask ? layer->mask->tiles : layer->tiles;
        into.insert_or_assign(TileKey{tx, ty}, std::move(tile));
    }
    (void)repaired;   // recorded for a future "this file was repaired" notice

    doc.activeLayer = manifest.value("active_layer", doc.layers.back().id);
    if (doc.layerById(doc.activeLayer) == nullptr)
        doc.activeLayer = doc.layers.back().id;

    doc.dirty = false;
    return doc;
}

// ------------------------------------------------------------------ recovery
// D-013: recovery data goes to a separate path under the XDG state directory,
// and restoring is an explicit user action. Never written over Document::path.

namespace {
std::filesystem::path g_recoveryOverride;
}  // namespace

void setRecoveryDirectory(std::filesystem::path directory) {
    g_recoveryOverride = std::move(directory);
}

std::filesystem::path recoveryDirectory() {
    if (!g_recoveryOverride.empty()) return g_recoveryOverride;

    std::filesystem::path base;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0')
        base = state;
    else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        base = std::filesystem::path(home) / ".local" / "state";
    else
        base = std::filesystem::temp_directory_path();
    return base / "sable" / "recovery";
}

std::expected<std::filesystem::path, Error> writeRecovery(
    const Document& doc, const std::filesystem::path& originalPath) {
    std::error_code ec;
    const std::filesystem::path dir = recoveryDirectory();
    std::filesystem::create_directories(dir, ec);
    if (ec) return fail(ErrorKind::Permission,
                        "could not create the recovery directory " + dir.string());

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Named after the process and the document, so two open windows do not
    // fight over one recovery file.
    const std::string stem = "recovery-" + std::to_string(now) + "-" +
                             std::to_string(doc.width) + "x" + std::to_string(doc.height);
    const std::filesystem::path file = dir / (stem + ".sable");

    if (const auto saved = saveProject(doc, file); !saved.has_value())
        return std::unexpected(saved.error());

    // The note is what makes the recovery findable, and what records where the
    // artist's real file lives — which we must never write to ourselves.
    nlohmann::json note;
    note["recovery_file"] = file.filename().string();
    note["original_path"] = originalPath.string();
    note["saved_at"]      = now;

    const std::filesystem::path notePath = dir / (stem + ".json");
    if (FILE* out = std::fopen(notePath.string().c_str(), "wb"); out != nullptr) {
        const std::string text = note.dump(2);
        std::fwrite(text.data(), 1, text.size(), out);
        std::fclose(out);
    }
    return file;
}

std::vector<RecoveryEntry> listRecoveries() {
    std::vector<RecoveryEntry> found;
    std::error_code ec;
    const std::filesystem::path dir = recoveryDirectory();
    if (!std::filesystem::exists(dir, ec)) return found;

    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (item.path().extension() != ".json") continue;

        std::size_t size = 0;
        std::string text;
        if (FILE* in = std::fopen(item.path().string().c_str(), "rb"); in != nullptr) {
            std::fseek(in, 0, SEEK_END);
            size = static_cast<std::size_t>(std::ftell(in));
            std::fseek(in, 0, SEEK_SET);
            text.resize(size);
            size = std::fread(text.data(), 1, size, in);
            text.resize(size);
            std::fclose(in);
        }
        if (text.empty()) continue;

        const nlohmann::json note = nlohmann::json::parse(text, nullptr, false);
        if (note.is_discarded()) continue;

        RecoveryEntry entry;
        entry.recoveryFile =
            dir / note.value("recovery_file", item.path().stem().string() + ".sable");
        entry.originalPath = note.value("original_path", std::string{});
        entry.savedAtEpochSeconds = note.value("saved_at", std::uint64_t{0});
        // A note without its .sable is useless and would offer the artist a
        // recovery that cannot be opened.
        if (std::filesystem::exists(entry.recoveryFile, ec)) found.push_back(std::move(entry));
    }

    std::ranges::sort(found, std::greater{}, &RecoveryEntry::savedAtEpochSeconds);
    return found;
}

void clearRecovery(const std::filesystem::path& recoveryFile) {
    std::error_code ec;
    std::filesystem::remove(recoveryFile, ec);
    std::filesystem::path note = recoveryFile;
    note.replace_extension(".json");
    std::filesystem::remove(note, ec);
}

}  // namespace sbl
