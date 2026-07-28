#include "sbl/psd.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// A hand-written reader rather than a library.
//
// Sable needs the layer section from both directions — the exporter (#7) writes
// the same records this reads — and the permissively licensed PSD libraries all
// read or write, never both. A dependency would therefore have left the writer
// to do anyway, and brought a second file-access and allocator abstraction with
// it. Adobe publishes the specification, and the part Sable uses is small.
//
// Everything here treats the file as hostile. Every length in a PSD is a
// number some other program wrote, so nothing is read without a bounds check
// and nothing is allocated from a length that has not been sanity-checked
// first: this is the one place in the engine that parses untrusted input.

namespace sbl {
namespace {

std::unexpected<Error> fail(ErrorKind kind, std::string detail) {
    return std::unexpected(Error{kind, std::move(detail)});
}

/// A layer plane no larger than this. RLE expands by up to 128x, so without a
/// cap a 10 MB file can ask for a gigabyte of memory.
constexpr std::size_t kMaxPlanePixels = 1u << 28;
/// Bigger than any canvas Sable will open, and small enough that width * height
/// cannot overflow.
constexpr std::int32_t kMaxDimension = 300000;

// ------------------------------------------------------------------- cursor

/// Big-endian, bounds-checked, and sticky: once a read runs off the end every
/// later read fails too, so a parse can be written straight through and checked
/// once at the end rather than after every field.
class Cursor {
public:
    explicit Cursor(const std::vector<unsigned char>& bytes) noexcept : b_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t pos() const noexcept { return at_; }
    [[nodiscard]] std::size_t size() const noexcept { return b_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return b_.size() - at_; }

    void seek(std::size_t p) noexcept {
        if (p > b_.size()) ok_ = false;
        else               at_ = p;
    }
    void skip(std::uint64_t n) noexcept {
        if (!ok_ || n > remaining()) { ok_ = false; return; }
        at_ += static_cast<std::size_t>(n);
    }

    std::uint8_t u8() noexcept {
        if (!ok_ || at_ >= b_.size()) { ok_ = false; return 0; }
        return b_[at_++];
    }
    std::uint16_t u16() noexcept {
        const std::uint32_t hi = u8();
        const std::uint32_t lo = u8();
        return static_cast<std::uint16_t>((hi << 8) | lo);
    }
    std::uint32_t u32() noexcept {
        const std::uint32_t hi = u16();
        const std::uint32_t lo = u16();
        return (hi << 16) | lo;
    }
    std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }

    std::string text(std::size_t n) {
        if (!ok_ || n > remaining()) { ok_ = false; return {}; }
        std::string out(reinterpret_cast<const char*>(b_.data() + at_), n);
        at_ += n;
        return out;
    }
    /// Reads straight out of the buffer without copying. Null when out of range.
    [[nodiscard]] const std::uint8_t* take(std::size_t n) noexcept {
        if (!ok_ || n > remaining()) { ok_ = false; return nullptr; }
        const std::uint8_t* p = b_.data() + at_;
        at_ += n;
        return p;
    }

private:
    const std::vector<unsigned char>& b_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

// -------------------------------------------------------------- blend modes

struct BlendKey {
    std::string_view key;      // PSD's four-character mode key
    BlendMode mode;
};

/// The modes that map. Everything else — dissolve, the linear and vivid
/// lights, the four HSL modes, pass-through groups — reads as Normal on
/// purpose: an artist who opens a PSD wants their pixels, not a lecture.
constexpr std::array<BlendKey, 13> kBlendKeys{{
    {"norm", BlendMode::Normal},      {"mul ", BlendMode::Multiply},
    {"scrn", BlendMode::Screen},      {"lddg", BlendMode::Add},
    {"over", BlendMode::Overlay},     {"dark", BlendMode::Darken},
    {"lite", BlendMode::Lighten},     {"div ", BlendMode::ColourDodge},
    {"idiv", BlendMode::ColourBurn},  {"hLit", BlendMode::HardLight},
    {"sLit", BlendMode::SoftLight},   {"diff", BlendMode::Difference},
    {"smud", BlendMode::Exclusion},
}};

BlendMode blendFromKey(std::string_view key) noexcept {
    for (const BlendKey& entry : kBlendKeys)
        if (entry.key == key) return entry.mode;
    return BlendMode::Normal;
}

// -------------------------------------------------------------------- names

/// PSD's 'luni' layer name is UTF-16BE; Sable's is UTF-8 everywhere else.
std::string utf8From(const std::vector<std::uint16_t>& units) {
    std::string out;
    out.reserve(units.size());
    const auto emit = [&out](std::uint32_t c) { out += static_cast<char>(c); };

    for (std::size_t i = 0; i < units.size(); ++i) {
        std::uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (units[++i] - 0xDC00u);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;                          // a lone surrogate is not text
        }

        if (cp < 0x80) {
            emit(cp);
        } else if (cp < 0x800) {
            emit(0xC0 | (cp >> 6));
            emit(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            emit(0xE0 | (cp >> 12));
            emit(0x80 | ((cp >> 6) & 0x3F));
            emit(0x80 | (cp & 0x3F));
        } else {
            emit(0xF0 | (cp >> 18));
            emit(0x80 | ((cp >> 12) & 0x3F));
            emit(0x80 | ((cp >> 6) & 0x3F));
            emit(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// -------------------------------------------------------------- compression

/// PackBits, which PSD calls compression 1. Fills exactly `count` bytes or
/// fails — a corrupt run must not walk off the end of the row it is filling.
bool unpackBits(Cursor& in, std::size_t packedEnd, std::uint8_t* out, std::size_t count) {
    std::size_t written = 0;
    while (written < count && in.ok() && in.pos() < packedEnd) {
        const auto n = static_cast<std::int8_t>(in.u8());
        if (n == -128) continue;                   // a no-op by the standard

        const std::size_t run = n >= 0 ? static_cast<std::size_t>(n) + 1
                                       : static_cast<std::size_t>(1 - n);
        if (run > count - written) return false;

        if (n >= 0) {
            const std::uint8_t* src = in.take(run);
            if (src == nullptr) return false;
            std::memcpy(out + written, src, run);
        } else {
            std::fill_n(out + written, run, in.u8());
        }
        written += run;
    }
    return in.ok() && written == count;
}

/// `planes` channel planes of `w * h` bytes each, appended to `out`.
///
/// One function for both layouts PSD uses: a layer channel is a single plane
/// with its own compression tag, and the merged image at the end of the file is
/// every channel under one tag with one shared table of row lengths. The only
/// difference is how many planes follow the tag.
bool readPlanes(Cursor& in, std::uint16_t compression, std::size_t planes,
                std::size_t w, std::size_t h, std::size_t blockEnd,
                std::vector<std::uint8_t>& out) {
    const std::size_t pixels = w * h;
    out.assign(planes * pixels, 0);
    if (pixels == 0) return true;

    if (compression == 0) {
        const std::uint8_t* raw = in.take(out.size());
        if (raw == nullptr) return false;
        std::memcpy(out.data(), raw, out.size());
        return true;
    }
    if (compression != 1) return false;            // 2/3 are zip; see the caller

    // Every row's packed length first, then the rows back to back.
    std::vector<std::uint32_t> rowBytes(planes * h);
    for (std::uint32_t& length : rowBytes) length = in.u16();
    if (!in.ok()) return false;

    for (std::size_t row = 0; row < rowBytes.size(); ++row) {
        const std::size_t start = in.pos();
        if (rowBytes[row] > in.remaining()) return false;
        if (!unpackBits(in, start + rowBytes[row], out.data() + row * w, w)) return false;
        in.seek(start + rowBytes[row]);            // resync: trailing slack is legal
    }
    return in.ok() && in.pos() <= blockEnd;
}

// ------------------------------------------------------------ layer records

/// One PSD layer record, before it becomes a Sable Layer. Groups arrive as
/// records too — see `section`.
struct Record {
    std::int32_t top = 0, left = 0, bottom = 0, right = 0;
    std::vector<std::pair<std::int16_t, std::uint64_t>> channels;   // id, byte length
    std::string blendKey = "norm";
    std::uint8_t opacity = 255;
    bool clipping = false;
    bool hidden = false;
    bool preserveOpacity = false;
    std::string name;

    /// PSD's 'lsct' section divider. 0 is an ordinary layer; 1 and 2 are the
    /// open and closed forms of a group's header; 3 is the hidden marker that
    /// closes one. File order is bottom to top, so a group reads as 3, then its
    /// children, then 1 or 2.
    std::uint32_t section = 0;

    [[nodiscard]] std::size_t width()  const noexcept {
        return static_cast<std::size_t>(right - left);
    }
    [[nodiscard]] std::size_t height() const noexcept {
        return static_cast<std::size_t>(bottom - top);
    }
};

bool readExtraData(Cursor& in, Record& rec) {
    const std::uint32_t extraLength = in.u32();
    if (!in.ok() || extraLength > in.remaining()) return false;
    const std::size_t extraEnd = in.pos() + extraLength;

    in.skip(in.u32());                             // layer mask: unsupported
    in.skip(in.u32());                             // layer blending ranges
    if (!in.ok()) return false;

    // The legacy name: a Pascal string in the system encoding, padded so the
    // whole field is a multiple of four. 'luni' below replaces it when present.
    const std::size_t nameLength = in.u8();
    rec.name = in.text(nameLength);
    in.skip((4 - ((nameLength + 1) % 4)) % 4);
    if (!in.ok()) return false;

    // Additional layer information: '8BIM'/'8B64', a four-character key, a
    // length, and the block. Unknown keys are skipped, which is most of them.
    while (in.ok() && in.pos() + 12 <= extraEnd) {
        const std::string signature = in.text(4);
        if (signature != "8BIM" && signature != "8B64") break;
        const std::string key = in.text(4);
        const std::uint32_t length = in.u32();
        if (!in.ok() || length > extraEnd - in.pos()) break;
        const std::size_t blockEnd = in.pos() + length;

        if (key == "lsct" && length >= 4) {
            rec.section = in.u32();
        } else if (key == "luni" && length >= 4) {
            const std::uint32_t count = in.u32();
            // Two bytes per unit, so the length field caps it before anything
            // is allocated.
            if (count <= (length - 4) / 2) {
                std::vector<std::uint16_t> units(count);
                for (std::uint16_t& unit : units) unit = in.u16();
                if (in.ok()) rec.name = utf8From(units);
            }
        }
        in.seek(blockEnd);
    }

    in.seek(extraEnd);
    return in.ok();
}

bool readRecord(Cursor& in, Record& rec) {
    rec.top    = in.i32();
    rec.left   = in.i32();
    rec.bottom = in.i32();
    rec.right  = in.i32();
    if (!in.ok()) return false;
    // An inverted or absurd rectangle is malformed, not merely empty. The
    // absolute bound matters as much as the size: writeRect() adds the origin
    // to a pixel offset, and a rectangle out at INT32_MAX would overflow that.
    for (const std::int32_t edge : {rec.top, rec.left, rec.bottom, rec.right})
        if (edge < -kMaxDimension || edge > kMaxDimension) return false;
    if (rec.bottom < rec.top || rec.right < rec.left) return false;
    if (rec.width() * rec.height() > kMaxPlanePixels) return false;

    const std::uint16_t channelCount = in.u16();
    if (!in.ok() || channelCount > 56) return false;
    for (std::uint16_t i = 0; i < channelCount; ++i) {
        const auto id     = static_cast<std::int16_t>(in.u16());
        const std::uint32_t length = in.u32();
        rec.channels.emplace_back(id, length);
    }

    if (in.text(4) != "8BIM") return false;        // blend mode signature
    rec.blendKey = in.text(4);
    rec.opacity  = in.u8();
    rec.clipping = in.u8() != 0;

    const std::uint8_t flags = in.u8();
    rec.preserveOpacity = (flags & 0x01) != 0;
    rec.hidden          = (flags & 0x02) != 0;     // bit set means hidden
    in.u8();                                       // filler

    return in.ok() && readExtraData(in, rec);
}

// ------------------------------------------------------------------- pixels

/// Writes one straight-alpha rectangle into a layer's sparse tiles.
///
/// D-004 at the boundary: PSD stores straight alpha, the engine stores
/// premultiplied, and StraightRgba8::premultiply() is the only crossing. Fully
/// transparent pixels are skipped so an empty region never materialises a tile.
void writeRect(Layer& layer, std::int32_t left, std::int32_t top,
               std::size_t w, std::size_t h, const std::uint8_t* r,
               const std::uint8_t* g, const std::uint8_t* b, const std::uint8_t* a,
               std::int32_t canvasW, std::int32_t canvasH) {
    // Clipped to the canvas: pixels outside it cannot be seen or edited, and an
    // unclipped rectangle from an untrusted file is unbounded memory.
    const std::int32_t x0 = std::max(0, -left), x1 =
        std::min(static_cast<std::int32_t>(w), canvasW - left);
    const std::int32_t y0 = std::max(0, -top), y1 =
        std::min(static_cast<std::int32_t>(h), canvasH - top);

    TileKey cachedKey{0, 0};
    Tile* cached = nullptr;

    for (std::int32_t y = y0; y < y1; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * w;
        for (std::int32_t x = x0; x < x1; ++x) {
            const std::size_t i = row + static_cast<std::size_t>(x);
            const std::uint8_t alpha = a != nullptr ? a[i] : 255;
            if (alpha == 0) continue;

            const std::int32_t cx = left + x;
            const std::int32_t cy = top + y;
            const TileKey key{tileIndex(cx), tileIndex(cy)};
            if (cached == nullptr || key != cachedKey) {
                cachedKey = key;
                cached    = &layer.tileFor(key);
            }
            cached->setPixel(cx - key.first * TILE_SIZE, cy - key.second * TILE_SIZE,
                             StraightRgba8{r[i], g[i], b[i], alpha}.premultiply());
        }
    }
}

// -------------------------------------------------------------------- input

std::expected<std::vector<unsigned char>, Error> readFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return fail(ErrorKind::NotFound, "could not read " + path.string());
    if (size > (std::uint64_t{1} << 31))
        return fail(ErrorKind::Malformed, path.string() + " is too large to open");

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    FILE* in = std::fopen(path.string().c_str(), "rb");
    if (in == nullptr) return fail(ErrorKind::Permission, "could not open " + path.string());
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), in);
    std::fclose(in);
    bytes.resize(got);
    return bytes;
}

std::string_view colourModeName(std::uint16_t mode) noexcept {
    switch (mode) {
        case 0: return "bitmap";
        case 1: return "greyscale";
        case 2: return "indexed-colour";
        case 4: return "CMYK";
        case 7: return "multichannel";
        case 8: return "duotone";
        case 9: return "Lab";
        default: break;
    }
    return "an unknown colour mode";
}

}  // namespace

// --------------------------------------------------------------------- read

std::expected<Document, Error> readPsd(const std::filesystem::path& path) {
    auto bytes = readFile(path);
    if (!bytes.has_value()) return std::unexpected(bytes.error());

    Cursor in(*bytes);
    if (in.text(4) != "8BPS")
        return fail(ErrorKind::Malformed, path.string() + " is not a PSD file");

    const std::uint16_t version = in.u16();
    if (version == 2)
        return fail(ErrorKind::UnsupportedVersion,
                    path.string() + " is a PSB (large document). Sable reads PSD; "
                    "re-save it as a PSD to open it here.");
    if (version != 1)
        return fail(ErrorKind::UnsupportedVersion,
                    path.string() + " uses PSD version " + std::to_string(version) +
                    ", which Sable does not know.");

    in.skip(6);                                    // reserved, always zero
    const std::uint16_t fileChannels = in.u16();
    const std::int32_t  height = in.i32();
    const std::int32_t  width  = in.i32();
    const std::uint16_t depth  = in.u16();
    const std::uint16_t colourMode = in.u16();
    if (!in.ok()) return fail(ErrorKind::Malformed, path.string() + " has a truncated header");

    // Refuse rather than produce garbage — the file is fine, Sable is not ready
    // for it, and saying so is the difference between a bug report and a bug.
    if (depth != 8)
        return fail(ErrorKind::UnsupportedVersion,
                    path.string() + " is " + std::to_string(depth) +
                    " bits per channel. Sable reads 8-bit PSD files; save a copy "
                    "as 8-bit to open it.");
    if (colourMode != 3)
        return fail(ErrorKind::UnsupportedVersion,
                    path.string() + " is " + std::string(colourModeName(colourMode)) +
                    ". Sable reads RGB PSD files; convert it to RGB to open it.");
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension)
        return fail(ErrorKind::Malformed, path.string() + " has an implausible canvas size");

    in.skip(in.u32());                             // colour mode data
    in.skip(in.u32());                             // image resources
    if (!in.ok()) return fail(ErrorKind::Malformed, path.string() + " is truncated");

    Document doc;
    doc.width  = width;
    doc.height = height;
    // PSD has no document background: whatever backs the artwork is a layer in
    // the file and stays one here, so nothing must be composited underneath or
    // the flattened result stops matching what Photoshop shows.
    doc.background = StraightRgba8{0, 0, 0, 0};

    const std::uint32_t layerMaskLength = in.u32();
    if (!in.ok() || layerMaskLength > in.remaining())
        return fail(ErrorKind::Malformed, path.string() + " has a truncated layer section");
    const std::size_t layerMaskEnd = in.pos() + layerMaskLength;

    std::vector<Record> records;
    std::size_t channelDataStart = 0;
    if (layerMaskLength >= 8) {
        const std::uint32_t layerInfoLength = in.u32();
        const std::size_t layerInfoEnd =
            std::min(layerMaskEnd, in.pos() + std::min<std::size_t>(layerInfoLength,
                                                                    in.remaining()));
        const auto rawCount = static_cast<std::int16_t>(in.u16());
        // A negative count is PSD's way of saying the merged image at the end
        // carries alpha. The layer records themselves are unaffected.
        const auto count = static_cast<std::size_t>(rawCount < 0 ? -rawCount : rawCount);

        records.reserve(std::min<std::size_t>(count, 4096));
        for (std::size_t i = 0; i < count; ++i) {
            Record rec;
            if (!readRecord(in, rec))
                return fail(ErrorKind::Malformed,
                            path.string() + " has a malformed layer record");
            records.push_back(std::move(rec));
        }
        channelDataStart = in.pos();
        if (channelDataStart > layerInfoEnd)
            return fail(ErrorKind::Malformed, path.string() + " has a truncated layer section");
    }

    // Groups: file order runs bottom to top, so a group's closing marker comes
    // first and its header last. The id is reserved when the marker is seen so
    // that children can point at a folder that does not exist yet.
    //
    // `at` is where the folder itself gets inserted once its header turns up:
    // Sable keeps a folder ahead of its children in Document::layers — that is
    // what addLayerAbove plus a parent produces, and what the layer panel
    // draws — while PSD writes the children first.
    struct OpenGroup {
        LayerId id;
        std::size_t at;
    };
    std::vector<OpenGroup> openGroups;
    const auto parentNow = [&openGroups]() -> std::optional<LayerId> {
        if (openGroups.empty()) return std::nullopt;
        return openGroups.back().id;
    };

    std::vector<std::uint8_t> plane;
    for (Record& rec : records) {
        if (rec.section == 3) {                    // the group ends here, going up
            openGroups.push_back(OpenGroup{doc.nextLayerId++, doc.layers.size()});
            // Its channel data still occupies space in the file.
            for (const auto& [id, length] : rec.channels) in.skip(length);
            continue;
        }

        Layer layer;
        layer.name    = rec.name.empty() ? "Layer" : rec.name;
        layer.opacity = static_cast<float>(rec.opacity) / 255.0f;
        layer.blend   = blendFromKey(rec.blendKey);
        layer.visible = !rec.hidden;
        layer.preserveOpacity = rec.preserveOpacity;
        layer.clipToBelow     = rec.clipping;

        if (rec.section == 1 || rec.section == 2) {
            layer.kind = LayerKind::Folder;
            if (openGroups.empty())
                return fail(ErrorKind::Malformed,
                            path.string() + " has an unbalanced layer group");
            const OpenGroup group = openGroups.back();
            openGroups.pop_back();
            layer.id     = group.id;
            layer.parent = parentNow();
            for (const auto& [id, length] : rec.channels) in.skip(length);

            doc.layers.insert(doc.layers.begin() +
                                  static_cast<std::ptrdiff_t>(group.at),
                              std::move(layer));
            continue;
        }
        layer.id     = doc.nextLayerId++;
        layer.parent = parentNow();

        const std::size_t w = rec.width();
        const std::size_t h = rec.height();
        std::array<std::vector<std::uint8_t>, 4> rgba;   // r, g, b, a

        for (const auto& [id, length] : rec.channels) {
            const std::size_t blockStart = in.pos();
            if (length > in.remaining())
                return fail(ErrorKind::Malformed,
                            path.string() + " has a truncated layer channel");
            const std::size_t blockEnd = blockStart + static_cast<std::size_t>(length);

            // 0..2 are R, G, B and -1 is alpha. -2 and -3 are masks, which
            // Sable has no home for yet, so they are skipped rather than
            // silently multiplied into the pixels.
            const int slot = id >= 0 && id <= 2 ? id : (id == -1 ? 3 : -1);
            if (slot >= 0 && w > 0 && h > 0) {
                const std::uint16_t compression = in.u16();
                if (compression > 1)
                    return fail(ErrorKind::UnsupportedVersion,
                                path.string() + " uses ZIP-compressed layer data, "
                                "which Sable cannot read yet.");
                if (!readPlanes(in, compression, 1, w, h, blockEnd, plane))
                    return fail(ErrorKind::Malformed,
                                path.string() + ": layer \"" + layer.name +
                                "\" has unreadable pixel data");
                rgba[static_cast<std::size_t>(slot)] = std::move(plane);
                plane.clear();
            }
            in.seek(blockEnd);
            if (!in.ok())
                return fail(ErrorKind::Malformed,
                            path.string() + " has a truncated layer channel");
        }

        if (w > 0 && h > 0 && !rgba[0].empty() && !rgba[1].empty() && !rgba[2].empty())
            writeRect(layer, rec.left, rec.top, w, h, rgba[0].data(), rgba[1].data(),
                      rgba[2].data(), rgba[3].empty() ? nullptr : rgba[3].data(),
                      doc.width, doc.height);

        doc.layers.push_back(std::move(layer));
    }

    // An unbalanced file would otherwise leave children pointing at a folder
    // that was never created, and levelOf() would never composite them.
    for (const OpenGroup& orphaned : openGroups)
        for (Layer& layer : doc.layers)
            if (layer.parent == orphaned.id) layer.parent = std::nullopt;

    // No layer section at all: a flattened PSD, which is most of what comes out
    // of an exporter. The merged image is then the only artwork in the file.
    if (doc.layers.empty()) {
        in.seek(layerMaskEnd);
        const std::uint16_t compression = in.u16();
        if (compression > 1)
            return fail(ErrorKind::UnsupportedVersion,
                        path.string() + " uses ZIP-compressed image data, which Sable "
                        "cannot read yet.");

        const std::size_t w = static_cast<std::size_t>(width);
        const std::size_t h = static_cast<std::size_t>(height);
        const std::size_t planes = std::min<std::size_t>(fileChannels, 4);
        std::vector<std::uint8_t> merged;
        if (planes < 3 || !readPlanes(in, compression, planes, w, h, in.size(), merged))
            return fail(ErrorKind::Malformed,
                        path.string() + " has no layers and no readable image");

        Layer layer;
        layer.id   = doc.nextLayerId++;
        layer.name = "Background";
        writeRect(layer, 0, 0, w, h, merged.data(), merged.data() + w * h,
                  merged.data() + 2 * w * h,
                  planes >= 4 ? merged.data() + 3 * w * h : nullptr, width, height);
        doc.layers.push_back(std::move(layer));
    }

    if (doc.layers.empty())
        return fail(ErrorKind::Malformed, path.string() + " contains no layers");

    doc.activeLayer = doc.layers.back().id;
    doc.dirty = false;
    // Document::path is the registry's business, not an importer's (D-024).
    return doc;
}

}  // namespace sbl
