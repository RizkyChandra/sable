#include "sbl/project.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

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

std::string tilePath(LayerId layer, TileKey key) {
    return "layers/" + std::to_string(layer) + "/tiles/" +
           std::to_string(key.first) + "_" + std::to_string(key.second) + ".png";
}

/// Encodes one tile as straight-alpha RGBA8, so external tools can open it.
std::vector<unsigned char> encodeTile(const Tile& tile) {
    std::vector<unsigned char> straight(
        static_cast<std::size_t>(TILE_PIXELS) * 4);
    const PremulRgba8* src = tile.pixels();
    for (int i = 0; i < TILE_PIXELS; ++i) {
        const StraightRgba8 c = src[i].unpremultiply();
        straight[i * 4 + 0] = c.r;
        straight[i * 4 + 1] = c.g;
        straight[i * 4 + 2] = c.b;
        straight[i * 4 + 3] = c.a;
    }

    std::vector<unsigned char> png;
    lodepng::encode(png, straight, TILE_SIZE, TILE_SIZE);
    return png;
}

bool decodeTile(const unsigned char* data, std::size_t size, Tile& out) {
    std::vector<unsigned char> straight;
    unsigned w = 0, h = 0;
    if (lodepng::decode(straight, w, h, data, size) != 0) return false;
    if (w != TILE_SIZE || h != TILE_SIZE) return false;

    PremulRgba8* dst = out.pixels();
    for (int i = 0; i < TILE_PIXELS; ++i)
        dst[i] = StraightRgba8{straight[i * 4 + 0], straight[i * 4 + 1],
                               straight[i * 4 + 2], straight[i * 4 + 3]}.premultiply();
    return true;
}

/// A small thumbnail for file browsers. Nearest-neighbour is fine — this is a
/// 256 px preview, not an export.
std::vector<unsigned char> encodeThumbnail(const Document& doc) {
    constexpr unsigned kMax = 256;
    const std::vector<StraightRgba8> full = flatten(doc);
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
    manifest["format_version"] = SABLE_FORMAT_VERSION;
    manifest["app"]            = "Sable 0.1.0";
    manifest["width"]          = doc.width;
    manifest["height"]         = doc.height;
    manifest["dpi"]            = doc.dpi;
    manifest["background"]     = hexColour(doc.background);
    manifest["tile_size"]      = TILE_SIZE;
    manifest["colour"] = {{"depth", 8}, {"space", "sRGB"}, {"premultiplied", true}};
    manifest["active_layer"]   = doc.activeLayer;
    manifest["layers"]         = json::array();

    for (const Layer& layer : doc.layers) {
        json entry;
        entry["id"]      = layer.id;
        entry["kind"]    = layer.kind == LayerKind::Folder ? "folder" : "raster";
        entry["name"]    = layer.name;
        entry["opacity"] = layer.opacity;
        entry["blend"]   = std::string(blendModeName(layer.blend));
        entry["visible"]          = layer.visible;
        entry["locked"]           = layer.locked;
        entry["preserve_opacity"] = layer.preserveOpacity;
        entry["clip_to_below"]    = layer.clipToBelow;
        if (layer.parent.has_value()) entry["parent"] = *layer.parent;
        else                          entry["parent"] = nullptr;

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

    if (!manifest.contains("layers") || !manifest["layers"].is_array())
        return fail(ErrorKind::Malformed, "the manifest lists no layers");

    for (const auto& entry : manifest["layers"]) {
        Layer layer;
        layer.id   = entry.value("id", 0u);
        if (layer.id == NO_LAYER) continue;         // 0 is the reserved "no layer"
        layer.kind = entry.value("kind", std::string("raster")) == "folder"
                         ? LayerKind::Folder : LayerKind::Raster;
        layer.name    = entry.value("name", std::string("Layer"));
        layer.opacity = std::clamp(entry.value("opacity", 1.0f), 0.0f, 1.0f);
        layer.blend   = blendModeFromName(entry.value("blend", std::string("normal")));
        layer.visible         = entry.value("visible", true);
        layer.locked          = entry.value("locked", false);
        layer.preserveOpacity = entry.value("preserve_opacity", false);
        layer.clipToBelow     = entry.value("clip_to_below", false);
        if (entry.contains("parent") && entry["parent"].is_number_unsigned())
            layer.parent = entry["parent"].get<LayerId>();

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

        // layers/<id>/tiles/<tx>_<ty>.png
        LayerId layerId = 0;
        int tx = 0, ty = 0;
        if (std::sscanf(name.c_str(), "layers/%u/tiles/%d_%d.png", &layerId, &tx, &ty) != 3)
            continue;

        Layer* layer = doc.layerById(layerId);
        if (layer == nullptr) { repaired = true; continue; }

        std::size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (data == nullptr) { repaired = true; continue; }

        Tile tile;
        const bool ok = decodeTile(static_cast<const unsigned char*>(data), size, tile);
        mz_free(data);
        if (!ok) { repaired = true; continue; }       // one bad tile, not a bad file

        layer->tiles.insert_or_assign(TileKey{tx, ty}, std::move(tile));
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
