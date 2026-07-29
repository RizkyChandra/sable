#include "sbl/kra.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sbl/io.hpp"   // iccProfileFromPng

#include "blit.hpp"
#include "miniz.h"
#include "miniz_zip.h"
#include "xml.hpp"

namespace sbl {
namespace {

std::unexpected<Error> fail(ErrorKind kind, std::string detail) {
    return std::unexpected(Error{kind, std::move(detail)});
}

std::int32_t parseInt(const std::string& text) {
    return static_cast<std::int32_t>(std::strtol(text.c_str(), nullptr, 10));
}

/// Krita's compositeop ids.
///
/// Established black-box: Krita 6.0.3 was asked to write one layer per mode,
/// and the ids it put in maindoc.xml were read back with `unzip` — then
/// cross-checked against the same document exported to ORA, where Krita names
/// each mode with its SVG operator ("dodge" comes out as svg:color-dodge, and
/// "add" as svg:plus, which is why neither can be guessed from the name).
///
/// Krita has many more modes than Sable's thirteen. Anything not listed here —
/// its own difference variants, divide, subtract, the HSL family — degrades to
/// Normal, which is wrong but recognisably wrong, rather than refusing to open
/// the file.
BlendMode blendFromKrita(std::string_view op) {
    struct Mapping {
        std::string_view id;
        BlendMode mode;
    };
    static constexpr std::array<Mapping, 15> kModes{{
        {"normal",         BlendMode::Normal},
        {"multiply",       BlendMode::Multiply},
        {"screen",         BlendMode::Screen},
        {"add",            BlendMode::Add},
        {"overlay",        BlendMode::Overlay},
        {"darken",         BlendMode::Darken},
        {"lighten",        BlendMode::Lighten},
        {"dodge",          BlendMode::ColourDodge},
        {"burn",           BlendMode::ColourBurn},
        {"hard_light",     BlendMode::HardLight},
        // Krita ships both the SVG soft light and the Photoshop one; they are
        // near enough that mapping both is better than losing one.
        {"soft_light_svg", BlendMode::SoftLight},
        {"soft_light",     BlendMode::SoftLight},
        {"soft_light_ifs_illusions", BlendMode::SoftLight},
        {"difference",     BlendMode::Difference},
        {"exclusion",      BlendMode::Exclusion},
    }};
    for (const Mapping& mapping : kModes)
        if (mapping.id == op) return mapping.mode;
    return BlendMode::Normal;
}

// ----------------------------------------------------------------------- LZF
// Krita compresses each tile with LZF. This is a decompressor for the
// published LZF stream format — a control byte followed by either a literal
// run or a back reference — written from that description.

bool lzfDecompress(const unsigned char* in, std::size_t inSize,
                   unsigned char* out, std::size_t outSize) {
    std::size_t read = 0;
    std::size_t written = 0;

    while (read < inSize) {
        const unsigned control = in[read++];
        if (control < 32) {
            // A literal run of control + 1 bytes.
            const std::size_t run = control + 1u;
            if (read + run > inSize || written + run > outSize) return false;
            std::memcpy(out + written, in + read, run);
            read    += run;
            written += run;
            continue;
        }

        // A back reference: length in the top three bits, distance in the low
        // five plus one more byte.
        std::size_t length = control >> 5;
        if (length == 7) {
            if (read >= inSize) return false;
            length += in[read++];
        }
        if (read >= inSize) return false;
        const std::size_t distance = ((control & 0x1Fu) << 8) + in[read++] + 1u;
        length += 2;

        if (distance > written || written + length > outSize) return false;
        // One byte at a time, deliberately: the reference may overlap the
        // write cursor, which is how LZF encodes a repeated run.
        for (std::size_t i = 0; i < length; ++i, ++written)
            out[written] = out[written - distance];
    }
    return written == outSize;
}

// ---------------------------------------------------------------- layer data
// A layer file is a short ASCII header followed by its tiles:
//
//     VERSION 2
//     TILEWIDTH 64
//     TILEHEIGHT 64
//     PIXELSIZE 4
//     DATA 3
//     0,0,LZF,378
//     <378 bytes>
//     ...
//
// The tile coordinates are canvas pixels and may be negative.

std::string_view readLine(std::string_view data, std::size_t& pos) {
    const std::size_t end = data.find('\n', pos);
    if (end == std::string_view::npos) { pos = data.size(); return {}; }
    const std::string_view line = data.substr(pos, end - pos);
    pos = end + 1;
    return line;
}

struct TileHeader {
    int tileWidth  = 0;
    int tileHeight = 0;
    int pixelSize  = 0;
    int count      = -1;
    std::size_t dataStart = 0;
};

std::optional<TileHeader> parseTileHeader(std::string_view data) {
    TileHeader header;
    std::size_t pos = 0;
    // Five keys, but read defensively rather than positionally.
    for (int line = 0; line < 8 && header.count < 0; ++line) {
        const std::string_view text = readLine(data, pos);
        const std::size_t space = text.find(' ');
        if (space == std::string_view::npos) return std::nullopt;

        const std::string_view key = text.substr(0, space);
        const int value = std::atoi(std::string(text.substr(space + 1)).c_str());
        if      (key == "VERSION")    { if (value != 2) return std::nullopt; }
        else if (key == "TILEWIDTH")  header.tileWidth  = value;
        else if (key == "TILEHEIGHT") header.tileHeight = value;
        else if (key == "PIXELSIZE")  header.pixelSize  = value;
        else if (key == "DATA")       header.count      = value;
        else return std::nullopt;
    }
    if (header.count < 0 || header.tileWidth <= 0 || header.tileHeight <= 0 ||
        header.tileWidth > 1024 || header.tileHeight > 1024)
        return std::nullopt;

    header.dataStart = pos;
    return header;
}

/// Turns one decompressed tile into straight-alpha RGBA8.
///
/// Two things here were established by writing a file from Krita with known
/// colours and reading the bytes back, because neither is in the format
/// description: the channels are stored as PLANES (every blue byte, then every
/// green, and so on) rather than interleaved, and an RGBA8 pixel is B, G, R, A
/// in memory. The alpha is straight, not premultiplied — an opaque-red fill
/// exported to ORA came back as 255,0,0,128 at half alpha, not 128,0,0,128.
std::vector<unsigned char> planarBgraToRgba(const std::vector<unsigned char>& raw,
                                            std::size_t pixels) {
    std::vector<unsigned char> straight(pixels * 4);
    const unsigned char* blue  = raw.data();
    const unsigned char* green = blue  + pixels;
    const unsigned char* red   = green + pixels;
    const unsigned char* alpha = red   + pixels;
    for (std::size_t i = 0; i < pixels; ++i) {
        straight[i * 4 + 0] = red[i];
        straight[i * 4 + 1] = green[i];
        straight[i * 4 + 2] = blue[i];
        straight[i * 4 + 3] = alpha[i];
    }
    return straight;
}

/// Krita's layer types, in the words an artist would use. The nodetype is
/// Krita's own vocabulary and saying "adjustmentlayer" at someone is not a
/// message, it is a leak.
std::string describeNodeType(const std::string& type) {
    if (type == "adjustmentlayer") return "filter layers";
    if (type == "generatorlayer")  return "fill layers";
    if (type == "clonelayer")      return "clone layers";
    if (type == "filelayer")       return "file layers";
    if (type == "shapelayer")      return "vector layers";
    if (type == "colorizemask" || type.ends_with("mask")) return "masks";
    if (type.empty())              return "that layer type";
    return type;
}

/// Fills the whole canvas with one colour, for a layer whose default pixel is
/// not transparent — Krita's layers are unbounded, and the tiles it stores are
/// only the parts that differ from that default.
void fillCanvas(Layer& layer, StraightRgba8 colour,
                std::int32_t canvasW, std::int32_t canvasH) {
    const PremulRgba8 premul = colour.premultiply();
    for (std::int32_t ty = 0; ty <= tileIndex(canvasH - 1); ++ty) {
        for (std::int32_t tx = 0; tx <= tileIndex(canvasW - 1); ++tx) {
            Tile& tile = layer.tileFor(TileKey{tx, ty});
            const std::int32_t right  = std::min(TILE_SIZE, canvasW - tx * TILE_SIZE);
            const std::int32_t bottom = std::min(TILE_SIZE, canvasH - ty * TILE_SIZE);
            for (std::int32_t y = 0; y < bottom; ++y)
                for (std::int32_t x = 0; x < right; ++x) tile.setPixel(x, y, premul);
        }
    }
}

// -------------------------------------------------------------------- import

struct Importer {
    mz_zip_archive& zip;
    Document& doc;
    std::string layerPrefix;   // "<document name>/layers/"

    /// Onto the document, which is what carries these across the engine/app
    /// boundary and into the status bar (#40). It used to be stderr, which
    /// nobody who launched Sable from a desktop icon was ever going to read.
    void warn(std::string message) const {
        doc.warnings.push_back(std::move(message));
    }

    [[nodiscard]] std::string entry(const std::string& name) const {
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip, name.c_str(), &size, 0);
        if (data == nullptr) return {};
        std::string out(static_cast<const char*>(data), size);
        mz_free(data);
        return out;
    }

    /// Krita's embedded ICC profile (D-034), which it writes to
    /// `<document>/annotations/icc` — raw profile bytes, no wrapper. Found by
    /// suffix for the same reason `layerFile` falls back to one: the directory
    /// is named after whatever the document was called when it was saved.
    ///
    /// Falls back to the `iCCP` chunk of `mergedimage.png`, which is where a
    /// KRA written by something other than Krita is likeliest to have put it.
    [[nodiscard]] IccProfile icc() const {
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
            if (!std::string_view(stat.m_filename).ends_with("/annotations/icc"))
                continue;
            std::size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (data == nullptr) break;
            IccProfile profile;
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            profile.data.assign(bytes, bytes + size);
            mz_free(data);
            if (!profile.empty()) return profile;
            break;
        }
        const std::string merged = entry("mergedimage.png");
        return iccProfileFromPng(merged.data(), merged.size());
    }

    /// The document name in maindoc.xml is normally the directory the layers
    /// live in, but it is written by whoever saved the file. Fall back to a
    /// search so a renamed or oddly-named document still opens.
    [[nodiscard]] std::string layerFile(const std::string& name) const {
        if (std::string found = entry(layerPrefix + name); !found.empty())
            return found;

        const std::string suffix = "/layers/" + name;
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
            if (std::string_view(stat.m_filename).ends_with(suffix)) {
                std::size_t size = 0;
                void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
                if (data == nullptr) return {};
                std::string out(static_cast<const char*>(data), size);
                mz_free(data);
                return out;
            }
        }
        return {};
    }

    void readTiles(Layer& layer, const std::string& filename,
                   std::int32_t offsetX, std::int32_t offsetY) {
        // A Krita layer is unbounded, and the tiles in its data file are only
        // the parts that differ from its default pixel — one BGRA pixel in a
        // file beside it. It is transparent almost always; when it is not, the
        // layer covers the canvas and the tiles paint over that.
        const std::string fallback = layerFile(filename + ".defaultpixel");
        if (fallback.size() == 4 && static_cast<unsigned char>(fallback[3]) != 0)
            fillCanvas(layer,
                       StraightRgba8{static_cast<std::uint8_t>(fallback[2]),
                                     static_cast<std::uint8_t>(fallback[1]),
                                     static_cast<std::uint8_t>(fallback[0]),
                                     static_cast<std::uint8_t>(fallback[3])},
                       doc.width, doc.height);

        const std::string data = layerFile(filename);
        if (data.empty()) return;                 // an empty layer is legal

        const std::optional<TileHeader> header = parseTileHeader(data);
        if (!header.has_value() || header->pixelSize != 4) {
            warn("layer \"" + layer.name + "\" arrives empty — its pixel data is not "
                 "in the 8-bit RGBA tile format Sable reads");
            return;
        }

        const auto pixels = static_cast<std::size_t>(header->tileWidth) *
                            static_cast<std::size_t>(header->tileHeight);
        std::vector<unsigned char> raw(pixels * 4);
        std::size_t pos = header->dataStart;

        for (int i = 0; i < header->count; ++i) {
            const std::string_view line = readLine(data, pos);
            if (line.empty()) return;

            // "<x>,<y>,LZF,<bytes>"
            std::int32_t x = 0, y = 0, size = 0;
            char compression[8] = {};
            if (std::sscanf(std::string(line).c_str(), "%d,%d,%7[^,],%d", &x, &y,
                            compression, &size) != 4)
                return;
            if (size <= 0 || pos + static_cast<std::size_t>(size) > data.size()) return;

            const auto* blob = reinterpret_cast<const unsigned char*>(data.data() + pos);
            pos += static_cast<std::size_t>(size);
            if (std::string_view(compression) != "LZF") {
                warn("layer \"" + layer.name + "\" arrives empty — its tiles use " +
                     compression + " compression, and Sable reads LZF");
                return;
            }

            // The first byte says whether what follows is compressed at all;
            // Krita stores the tile raw when LZF would make it bigger.
            const bool compressed = blob[0] != 0;
            if (compressed) {
                if (!lzfDecompress(blob + 1, static_cast<std::size_t>(size) - 1,
                                   raw.data(), raw.size()))
                    continue;                     // one bad tile, not a bad file
            } else {
                if (static_cast<std::size_t>(size) - 1 < raw.size()) continue;
                std::memcpy(raw.data(), blob + 1, raw.size());
            }

            blitStraightImage(layer, planarBgraToRgba(raw, pixels).data(),
                              header->tileWidth, header->tileHeight,
                              x + offsetX, y + offsetY, doc.width, doc.height);
        }
        dropBlankTiles(layer);
    }

    /// maindoc.xml lists the TOPMOST layer first; Document::layers is bottom
    /// first, hence the reverse walk.
    void addLayers(const XmlNode& layers, std::optional<LayerId> parent) {
        for (auto entry = layers.children.rbegin(); entry != layers.children.rend();
             ++entry) {
            if (entry->name != "layer") continue;

            const std::string name = entry->attributeOr("name", "Layer");
            const std::string type = entry->attributeOr("nodetype", "");
            const bool folder = type == "grouplayer";
            if (!folder && type != "paintlayer") {
                // Filter layers, clone layers, file layers, vector layers: the
                // rest of the file still opens (US: a partial import beats no
                // import).
                warn("skipping layer \"" + name + "\" — Sable does not support " +
                     describeNodeType(type));
                continue;
            }

            Layer layer;
            layer.id      = doc.nextLayerId++;
            layer.kind    = folder ? LayerKind::Folder : LayerKind::Raster;
            layer.name    = name;
            layer.opacity = std::clamp(
                static_cast<float>(parseInt(entry->attributeOr("opacity", "255"))) / 255.0f,
                0.0f, 1.0f);
            layer.blend   = blendFromKrita(entry->attributeOr("compositeop", "normal"));
            layer.visible = entry->attributeOr("visible", "1") != "0";
            layer.locked  = entry->attributeOr("locked", "0") != "0";
            layer.parent  = parent;

            const LayerId id = layer.id;
            if (!folder) {
                const std::string space = entry->attributeOr("colorspacename", "RGBA");
                if (space != "RGBA")
                    warn("layer \"" + name + "\" arrives empty — it is in the " + space +
                         " colour space, and Sable reads 8-bit RGBA");
                else
                    readTiles(layer, entry->attributeOr("filename", ""),
                              parseInt(entry->attributeOr("x", "0")),
                              parseInt(entry->attributeOr("y", "0")));
            }
            doc.layers.push_back(std::move(layer));

            if (folder) {
                if (const XmlNode* children = entry->child("layers"))
                    addLayers(*children, id);
            }
        }
    }
};

}  // namespace

std::expected<Document, Error> readKrita(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return fail(ErrorKind::NotFound, path.string() + " does not exist");

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.string().c_str(), 0))
        return fail(ErrorKind::Malformed,
                    path.string() + " is not a readable ZIP archive, so it cannot "
                    "be a Krita document");
    struct ZipGuard {
        mz_zip_archive* z;
        ~ZipGuard() { mz_zip_reader_end(z); }
    } guard{&zip};

    Document doc;
    Importer importer{zip, doc, {}};

    const std::string manifest = importer.entry("maindoc.xml");
    if (manifest.empty())
        return fail(ErrorKind::Malformed, path.string() + " has no maindoc.xml");

    const std::optional<XmlNode> document = parseXml(manifest);
    if (!document.has_value() || document->name != "DOC")
        return fail(ErrorKind::Malformed,
                    "the maindoc.xml in " + path.string() + " is not a Krita document");

    const XmlNode* image = document->child("IMAGE");
    if (image == nullptr)
        return fail(ErrorKind::Malformed,
                    "the maindoc.xml in " + path.string() + " describes no image");

    // Refuse rather than misread: a 16-bit or CMYK document would decode as
    // noise if it were pushed through the 8-bit RGBA path below.
    const std::string space = image->attributeOr("colorspacename", "RGBA");
    if (space != "RGBA")
        return fail(ErrorKind::UnsupportedVersion,
                    path.string() + " is a " + space + " document, and Sable reads "
                    "8-bit RGBA (" + space + " would have to be converted in Krita "
                    "first)");

    doc.width  = parseInt(image->attributeOr("width", "0"));
    doc.height = parseInt(image->attributeOr("height", "0"));
    if (doc.width <= 0 || doc.height <= 0 || doc.width > 65536 || doc.height > 65536)
        return fail(ErrorKind::Malformed,
                    path.string() + " gives an implausible canvas size");
    doc.dpi = static_cast<std::uint32_t>(
        std::max<std::int32_t>(1, parseInt(image->attributeOr("x-res", "72"))));

    // `colorspacename` above says RGBA — which channels there are, not what
    // they mean. The profile beside it is what says that (D-034), and Krita
    // writes one into every document it saves, including its sRGB default.
    adoptColourProfile(doc, importer.icc());

    // Krita's canvas background is a projection colour rather than a layer, and
    // Sable has no equivalent that survives export, so an imported document
    // keeps its transparency — the same choice the ORA importer makes.
    doc.background = StraightRgba8{0, 0, 0, 0};

    importer.layerPrefix = image->attributeOr("name", "") + "/layers/";

    const XmlNode* layers = image->child("layers");
    if (layers == nullptr)
        return fail(ErrorKind::Malformed, path.string() + " lists no layers");
    importer.addLayers(*layers, std::nullopt);

    if (doc.layers.empty())
        return fail(ErrorKind::Malformed,
                    path.string() + " has no layers Sable can read");

    doc.activeLayer = doc.layers.back().id;
    doc.dirty = false;
    return doc;
}

}  // namespace sbl
