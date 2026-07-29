#include "sbl/psd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// A hand-written reader and writer rather than a library.
//
// Sable needs the layer section from both directions, and the permissively
// licensed PSD libraries all read or write, never both. A dependency would
// therefore have left half the job to do anyway, and brought a second
// file-access and allocator abstraction with it. Adobe publishes the
// specification, and the part Sable uses is small.
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

// --------------------------------------------------------- image resources

/// The ICC profile out of the image resource section, or nothing (D-034).
///
/// Resources are a flat list of `8BIM`, a 16-bit id, a Pascal-string name and a
/// length, each of the last two padded to an even byte. 1039 is the profile.
/// The cursor is left at `end` whatever happens: a resource section Sable
/// cannot walk must not desynchronise the layer section behind it, which is the
/// whole file. That is why this takes a copy of the cursor rather than the
/// cursor itself.
[[nodiscard]] IccProfile readIccResource(Cursor scan, std::size_t end) {
    while (scan.ok() && scan.pos() + 12 <= end) {
        if (scan.text(4) != "8BIM") break;
        const std::uint16_t id = scan.u16();

        const std::uint8_t nameLen = scan.u8();
        scan.skip(nameLen);
        if ((nameLen + 1u) % 2u != 0u) scan.skip(1);   // Pascal string, even-padded

        const std::uint32_t size = scan.u32();
        if (!scan.ok() || size > scan.remaining()) break;
        if (id == 1039) {
            const std::uint8_t* data = scan.take(size);
            if (data == nullptr) break;
            IccProfile profile;
            profile.data.assign(data, data + size);
            return profile;
        }
        scan.skip(size);
        if (size % 2u != 0u) scan.skip(1);             // and so is the payload
    }
    return {};
}

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

std::string_view keyForBlend(BlendMode mode) noexcept {
    for (const BlendKey& entry : kBlendKeys)
        if (entry.mode == mode) return entry.key;
    return "norm";
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

/// A PSD layer mask: a greyscale plane that multiplies the layer's alpha.
///
/// It maps onto `sbl::LayerMask` almost field for field (#48) — a rectangle of
/// coverage plus a default colour for everything outside it — which is what
/// makes both directions of the import short. It used to be baked into the
/// pixels (D-027), because there was nowhere to put it.
struct MaskPlane {
    std::int32_t top = 0, left = 0, bottom = 0, right = 0;
    /// PSD's "default colour": what the mask reads as beyond its own rectangle.
    std::uint8_t outside = 0;
    bool present = false;                  // a mask block was read
    bool enabled = true;                   // PSD's "layer mask disabled" flag
    std::vector<std::uint8_t> plane;       // width() * height() bytes, once read

    [[nodiscard]] std::size_t width()  const noexcept {
        return static_cast<std::size_t>(right - left);
    }
    [[nodiscard]] std::size_t height() const noexcept {
        return static_cast<std::size_t>(bottom - top);
    }
    /// A mask with no pixel data cannot say anything about the layer, and
    /// applying its default colour alone could blank a layer outright on a file
    /// that only meant to say "there is a mask here".
    [[nodiscard]] bool usable() const noexcept {
        return present && enabled && !plane.empty();
    }
    /// The mask's value at a canvas pixel.
    [[nodiscard]] std::uint8_t at(std::int32_t cx, std::int32_t cy) const noexcept {
        if (cx < left || cy < top || cx >= right || cy >= bottom) return outside;
        return plane[static_cast<std::size_t>(cy - top) * width() +
                     static_cast<std::size_t>(cx - left)];
    }
};

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

    MaskPlane mask;       // channel -2, the mask the artist painted
    MaskPlane realMask;   // channel -3, the one Photoshop renders from a path

    [[nodiscard]] std::size_t width()  const noexcept {
        return static_cast<std::size_t>(right - left);
    }
    [[nodiscard]] std::size_t height() const noexcept {
        return static_cast<std::size_t>(bottom - top);
    }
};

/// A rectangle from an untrusted file, held to the same bounds as a layer's.
bool plausibleRect(const MaskPlane& m) noexcept {
    for (const std::int32_t edge : {m.top, m.left, m.bottom, m.right})
        if (edge < -kMaxDimension || edge > kMaxDimension) return false;
    if (m.bottom < m.top || m.right < m.left) return false;
    return m.width() * m.height() <= kMaxPlanePixels;
}

/// The layer mask / adjustment layer data block: 0, 20 or 36 bytes of it.
///
/// The 36-byte form describes a second mask as well — the one Photoshop renders
/// from a vector path, which arrives as channel -3 and supersedes the painted
/// one when both are there.
bool readMaskBlock(Cursor& in, Record& rec) {
    const std::uint32_t length = in.u32();
    if (!in.ok() || length > in.remaining()) return false;
    const std::size_t end = in.pos() + length;

    // 18 = rectangle, default colour, flags. The 20-byte form pads to four.
    if (length >= 18) {
        rec.mask.top    = in.i32();
        rec.mask.left   = in.i32();
        rec.mask.bottom = in.i32();
        rec.mask.right  = in.i32();
        rec.mask.outside = in.u8();
        const std::uint8_t flags = in.u8();
        rec.mask.enabled = (flags & 0x02) == 0;
        rec.mask.present = in.ok() && plausibleRect(rec.mask);

        // Bit 4 says variable-length mask parameters follow, which would move
        // everything after them. Rather than guess at the offsets, keep the
        // painted mask and leave the vector one — the layer is still masked,
        // just by the half of the pair this reader is sure of.
        if (length >= 36 && (flags & 0x10) == 0) {
            const std::uint8_t realFlags = in.u8();
            rec.realMask.enabled = (realFlags & 0x02) == 0;
            rec.realMask.outside = in.u8();
            rec.realMask.top     = in.i32();
            rec.realMask.left    = in.i32();
            rec.realMask.bottom  = in.i32();
            rec.realMask.right   = in.i32();
            rec.realMask.present = in.ok() && plausibleRect(rec.realMask);
        }
    }

    in.seek(end);
    return in.ok();
}

bool readExtraData(Cursor& in, Record& rec) {
    const std::uint32_t extraLength = in.u32();
    if (!in.ok() || extraLength > in.remaining()) return false;
    const std::size_t extraEnd = in.pos() + extraLength;

    if (!readMaskBlock(in, rec)) return false;
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

/// Writes a PSD mask plane into a Sable layer mask (#48, superseding D-027).
///
/// The two models line up exactly, which is why this is short: PSD stores a
/// rectangle of greyscale plus a default colour for everything outside it, and
/// `LayerMask` stores sparse tiles plus `outside`. So the plane goes in at the
/// coverage the file holds and nothing is expanded to canvas size to say
/// "unchanged" — a mask on a 200-pixel highlight stays 200 pixels.
void writeMask(Layer& layer, const MaskPlane& plane, std::int32_t canvasW,
               std::int32_t canvasH) {
    LayerMask mask;
    mask.outside = plane.outside;
    mask.enabled = plane.enabled;

    const std::int32_t x0 = std::max(0, plane.left);
    const std::int32_t y0 = std::max(0, plane.top);
    const std::int32_t x1 = std::min(canvasW, plane.right);
    const std::int32_t y1 = std::min(canvasH, plane.bottom);

    TileKey cachedKey{0, 0};
    Tile* cached = nullptr;
    for (std::int32_t cy = y0; cy < y1; ++cy) {
        for (std::int32_t cx = x0; cx < x1; ++cx) {
            const std::uint8_t cov = plane.at(cx, cy);
            const TileKey key{tileIndex(cx), tileIndex(cy)};
            if (cached == nullptr || key != cachedKey) {
                cachedKey = key;
                cached    = &mask.tileFor(key);
            }
            // Opaque grey: coverage is the red channel, and an alpha of
            // anything less would make the brush's own `over` behave as though
            // part of the mask had been erased.
            cached->setPixel(cx - key.first * TILE_SIZE, cy - key.second * TILE_SIZE,
                             PremulRgba8{cov, cov, cov, 255});
        }
    }
    layer.mask = std::move(mask);
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

    // Image resources. Walked for the ICC profile (D-034) and then skipped
    // wholesale exactly as before, so a resource section this reader cannot
    // make sense of costs a profile and nothing else.
    const std::uint32_t resourceLength = in.u32();
    const std::size_t resourceEnd = in.pos() + std::min<std::size_t>(
        resourceLength, in.ok() ? in.remaining() : 0);
    IccProfile embedded = in.ok() ? readIccResource(in, resourceEnd) : IccProfile{};
    in.skip(resourceLength);
    if (!in.ok()) return fail(ErrorKind::Malformed, path.string() + " is truncated");

    Document doc;
    doc.width  = width;
    doc.height = height;
    // What the file says its numbers mean. Not applied to the pixels: the
    // document keeps Photoshop's values and the conversion happens on the way
    // to the screen, so opening a file never repaints it.
    adoptColourProfile(doc, std::move(embedded));
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

        const std::size_t w = rec.width();
        const std::size_t h = rec.height();
        std::array<std::vector<std::uint8_t>, 4> rgba;   // r, g, b, a

        // Read for a GROUP as well as for an ordinary layer (#48). This used to
        // skip a group's channels outright, because the only thing in them
        // Sable could have used was a mask it had nowhere to put; a folder can
        // carry one now, and a group record's own w and h are zero, so the
        // colour planes below are still skipped exactly as they were.
        for (const auto& [id, length] : rec.channels) {
            const std::size_t blockStart = in.pos();
            if (length > in.remaining())
                return fail(ErrorKind::Malformed,
                            path.string() + " has a truncated layer channel");
            const std::size_t blockEnd = blockStart + static_cast<std::size_t>(length);

            // 0..2 are R, G, B and -1 is alpha. -2 is the mask the artist
            // painted and -3 the one Photoshop renders from a vector path;
            // both carry their own rectangle, from the mask block above.
            const int slot = id >= 0 && id <= 2 ? id : (id == -1 ? 3 : -1);
            MaskPlane* mask = id == -2 ? &rec.mask
                            : id == -3 ? &rec.realMask
                                       : nullptr;
            if (mask != nullptr && !mask->present) mask = nullptr;

            const std::size_t pw = mask != nullptr ? mask->width()  : w;
            const std::size_t ph = mask != nullptr ? mask->height() : h;
            if ((slot >= 0 || mask != nullptr) && pw > 0 && ph > 0) {
                const std::uint16_t compression = in.u16();
                if (compression > 1)
                    return fail(ErrorKind::UnsupportedVersion,
                                path.string() + " uses ZIP-compressed layer data, "
                                "which Sable cannot read yet.");
                if (!readPlanes(in, compression, 1, pw, ph, blockEnd, plane))
                    return fail(ErrorKind::Malformed,
                                path.string() + ": layer \"" + layer.name +
                                "\" has unreadable pixel data");
                if (mask != nullptr) mask->plane = std::move(plane);
                else rgba[static_cast<std::size_t>(slot)] = std::move(plane);
                plane.clear();
            }
            in.seek(blockEnd);
            if (!in.ok())
                return fail(ErrorKind::Malformed,
                            path.string() + " has a truncated layer channel");
        }

        // #48, superseding D-027: the mask keeps its own channel. It used to be
        // multiplied into the alpha here because `Layer` had nowhere to put it,
        // which made the drawing right and the mask gone; now the import is
        // faithful in both. The vector-derived mask still wins when both are
        // present — Photoshop writes channel -3 as the combination of the two.
        if (const MaskPlane& m = rec.realMask.usable() ? rec.realMask : rec.mask;
            m.present && !m.plane.empty())
            writeMask(layer, m, doc.width, doc.height);

        if (rec.section == 1 || rec.section == 2) {
            layer.kind = LayerKind::Folder;
            if (openGroups.empty())
                return fail(ErrorKind::Malformed,
                            path.string() + " has an unbalanced layer group");
            const OpenGroup group = openGroups.back();
            openGroups.pop_back();
            layer.id     = group.id;
            layer.parent = parentNow();
            doc.layers.insert(doc.layers.begin() +
                                  static_cast<std::ptrdiff_t>(group.at),
                              std::move(layer));
            continue;
        }
        layer.id     = doc.nextLayerId++;
        layer.parent = parentNow();

        if (w > 0 && h > 0 && !rgba[0].empty() && !rgba[1].empty() && !rgba[2].empty())
            writeRect(layer, rec.left, rec.top, w, h, rgba[0].data(), rgba[1].data(),
                      rgba[2].data(), rgba[3].empty() ? nullptr : rgba[3].data(),
                      doc.width, doc.height);

        doc.layers.push_back(std::move(layer));
    }

    // No warning about masks any more, and that is the point of #48: what
    // #35 had to apologise for — "the masks can no longer be edited" — is not
    // true of this reader. Nothing is lost, so there is nothing to say.

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

// -------------------------------------------------------------------- write

namespace {

/// PSD's own canvas limit. Beyond it the format is PSB, which Sable does not
/// write either — better to say so than to produce a file Photoshop rejects.
constexpr std::int32_t kMaxPsdDimension = 30000;

/// Deep enough for any real layer tree, and the thing that stops a document
/// with a corrupt parent chain recursing until the stack runs out.
constexpr int kMaxNesting = 32;

struct Writer {
    std::vector<unsigned char> out;

    void u8(std::uint32_t v)  { out.push_back(static_cast<unsigned char>(v & 0xFF)); }
    void u16(std::uint32_t v) { u8(v >> 8); u8(v); }
    void u32(std::uint32_t v) { u16(v >> 16); u16(v); }
    void i32(std::int32_t v)  { u32(static_cast<std::uint32_t>(v)); }
    void pad(std::size_t n)   { out.insert(out.end(), n, 0); }
    void text(std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }
    void bytes(const std::vector<std::uint8_t>& b) { out.insert(out.end(), b.begin(), b.end()); }

    /// Backfills a 4-byte length written before a section whose size was not
    /// known yet. Cheaper and much clearer than measuring everything twice.
    [[nodiscard]] std::size_t reserveLength() { u32(0); return out.size() - 4; }
    void fillLength(std::size_t at) {
        const auto length = static_cast<std::uint32_t>(out.size() - at - 4);
        for (int i = 0; i < 4; ++i)
            out[at + static_cast<std::size_t>(i)] =
                static_cast<unsigned char>((length >> (24 - 8 * i)) & 0xFF);
    }
};

/// UTF-8 in, UTF-16BE code units out, for PSD's 'luni' layer name. Malformed
/// input becomes U+FFFD rather than being rejected — an odd byte in a layer
/// name must not cost the artist the export.
std::vector<std::uint16_t> utf16From(std::string_view text) {
    std::vector<std::uint16_t> units;
    units.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<std::uint8_t>(text[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t len = 1;

        if (lead < 0x80)               { cp = lead; }
        else if ((lead & 0xE0) == 0xC0) { len = 2; cp = lead & 0x1Fu; }
        else if ((lead & 0xF0) == 0xE0) { len = 3; cp = lead & 0x0Fu; }
        else if ((lead & 0xF8) == 0xF0) { len = 4; cp = lead & 0x07u; }

        if (len > 1) {
            if (i + len > text.size()) { cp = 0xFFFD; len = 1; }
            for (std::size_t k = 1; k < len; ++k) {
                const auto cont = static_cast<std::uint8_t>(text[i + k]);
                if ((cont & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; break; }
                cp = (cp << 6) | (cont & 0x3Fu);
            }
        }
        i += len;

        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x10000) {
            units.push_back(static_cast<std::uint16_t>(cp));
        } else {
            cp -= 0x10000;
            units.push_back(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            units.push_back(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return units;
}

/// PackBits, the other half of unpackBits above.
void packBitsRow(const std::uint8_t* row, std::size_t n, std::vector<std::uint8_t>& out) {
    std::size_t i = 0;
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n && row[i + run] == row[i] && run < 128) ++run;

        if (run >= 3) {
            out.push_back(static_cast<std::uint8_t>(257 - run));   // -(run - 1)
            out.push_back(row[i]);
            i += run;
        } else {
            // A literal run, ended by the next run of three — below three a run
            // costs more to encode than to copy.
            const std::size_t start = i;
            std::size_t literal = 0;
            while (i < n && literal < 128) {
                if (i + 2 < n && row[i] == row[i + 1] && row[i] == row[i + 2]) break;
                ++i;
                ++literal;
            }
            out.push_back(static_cast<std::uint8_t>(literal - 1));
            out.insert(out.end(), row + start, row + start + literal);
        }
    }
}

/// RLE-compresses `planes` rows of `w` bytes into PSD's layout: every row's
/// packed length first, then the rows. One function for both places PSD uses
/// it — a layer channel is one plane, the merged image is all four under a
/// single shared table.
struct Packed {
    std::vector<std::uint8_t> counts;   // two bytes per row
    std::vector<std::uint8_t> data;
};

Packed packPlanes(const std::uint8_t* planes, std::size_t w, std::size_t h,
                  std::size_t count) {
    Packed packed;
    packed.counts.reserve(2 * h * count);
    packed.data.reserve(w * h * count / 2);

    std::vector<std::uint8_t> row;
    for (std::size_t r = 0; r < h * count; ++r) {
        row.clear();
        packBitsRow(planes + r * w, w, row);
        packed.counts.push_back(static_cast<std::uint8_t>(row.size() >> 8));
        packed.counts.push_back(static_cast<std::uint8_t>(row.size() & 0xFF));
        packed.data.insert(packed.data.end(), row.begin(), row.end());
    }
    return packed;
}

/// One layer as PSD sees it. Groups become two of these: a header and the
/// hidden marker that closes them.
struct OutRecord {
    std::int32_t top = 0, left = 0, bottom = 0, right = 0;
    std::string name;
    std::string_view blendKey = "norm";
    std::uint8_t opacity = 255;
    bool clipping = false;
    bool visible = true;
    bool preserveOpacity = false;
    std::optional<std::uint32_t> section;      // 'lsct' type, for groups
    /// Alpha, red, green, blue, each already carrying its compression tag.
    std::array<std::vector<std::uint8_t>, 4> channels;

    /// The layer mask (#48), which PSD stores as a fifth channel with an id of
    /// -2 plus a rectangle and a default colour of its own — the same shape
    /// `LayerMask` has, which is why nothing has to be expanded to canvas size
    /// on the way out.
    bool maskPresent = false;
    bool maskEnabled = true;
    std::uint8_t maskOutside = 255;
    std::int32_t maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
    std::vector<std::uint8_t> maskChannel;
};

/// Wraps one already-planar channel in the compression tag and row table PSD
/// expects, in place.
void packChannel(std::vector<std::uint8_t>& out, const std::uint8_t* plane,
                 std::size_t w, std::size_t h) {
    const Packed packed = packPlanes(plane, w, h, 1);
    out.assign({0, 1});                              // compression 1: RLE
    out.insert(out.end(), packed.counts.begin(), packed.counts.end());
    out.insert(out.end(), packed.data.begin(), packed.data.end());
}

std::uint8_t opacityByte(float opacity) noexcept {
    return static_cast<std::uint8_t>(
        std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
}

/// An empty channel: just the compression tag, which is what Photoshop writes
/// for a group marker or a layer that has never been painted on.
std::vector<std::uint8_t> emptyChannel() { return {0, 0}; }

void fillCommon(OutRecord& rec, const Layer& layer) {
    rec.name            = layer.name;
    rec.blendKey        = keyForBlend(layer.blend);
    rec.opacity         = opacityByte(layer.opacity);
    rec.clipping        = layer.clipToBelow;
    rec.visible         = layer.visible;
    rec.preserveOpacity = layer.preserveOpacity;
}

/// The canvas rectangle a layer's painted tiles cover, clipped to the document.
///
/// Tile granularity rather than the exact painted bounds: a 256-pixel margin
/// costs a little file size and saves scanning every pixel twice, and the
/// alpha channel makes the margin invisible either way.
void boundsOf(const Layer& layer, std::int32_t canvasW, std::int32_t canvasH,
              OutRecord& rec) {
    bool any = false;
    for (const auto& [key, tile] : layer.tiles) {
        if (tile.isFullyTransparent()) continue;
        const std::int32_t x0 = std::max(0, key.first  * TILE_SIZE);
        const std::int32_t y0 = std::max(0, key.second * TILE_SIZE);
        const std::int32_t x1 = std::min(canvasW, (key.first  + 1) * TILE_SIZE);
        const std::int32_t y1 = std::min(canvasH, (key.second + 1) * TILE_SIZE);
        if (x0 >= x1 || y0 >= y1) continue;         // wholly outside the canvas

        if (!any) { rec.left = x0; rec.top = y0; rec.right = x1; rec.bottom = y1; any = true; }
        rec.left   = std::min(rec.left,   x0);
        rec.top    = std::min(rec.top,    y0);
        rec.right  = std::max(rec.right,  x1);
        rec.bottom = std::max(rec.bottom, y1);
    }
    if (!any) rec.left = rec.top = rec.right = rec.bottom = 0;
}

/// Straight-alpha planes for a layer's rectangle, in PSD's channel order.
///
/// D-004 at the boundary again, in the other direction: the engine holds
/// premultiplied pixels and PSD wants straight ones, so unpremultiply() is the
/// only crossing.
void encodeChannels(const Layer& layer, OutRecord& rec) {
    const auto w = static_cast<std::size_t>(rec.right - rec.left);
    const auto h = static_cast<std::size_t>(rec.bottom - rec.top);
    if (w == 0 || h == 0) {
        for (std::vector<std::uint8_t>& channel : rec.channels) channel = emptyChannel();
        return;
    }

    // One buffer holding A, R, G, B back to back, which is the order PSD's
    // channel list will name them in.
    std::vector<std::uint8_t> planes(4 * w * h, 0);
    for (std::size_t y = 0; y < h; ++y) {
        const std::int32_t cy = rec.top + static_cast<std::int32_t>(y);
        const std::int32_t ty = tileIndex(cy);
        for (std::size_t x = 0; x < w; ++x) {
            const std::int32_t cx = rec.left + static_cast<std::int32_t>(x);
            const Tile* tile = layer.find(TileKey{tileIndex(cx), ty});
            if (tile == nullptr) continue;

            // Sable writes 8-bit PSD, so this is an export boundary and the
            // narrowing is the honest one (D-023).
            const StraightRgba8 c = narrow(
                tile->pixel(cx - tileIndex(cx) * TILE_SIZE, cy - ty * TILE_SIZE)
                    .unpremultiply());
            const std::size_t i = y * w + x;
            planes[i]                 = c.a;
            planes[w * h + i]         = c.r;
            planes[2 * w * h + i]     = c.g;
            planes[3 * w * h + i]     = c.b;
        }
    }

    for (std::size_t channel = 0; channel < 4; ++channel)
        packChannel(rec.channels[channel], planes.data() + channel * w * h, w, h);
}

/// The mask as PSD's channel -2 (#48), or nothing at all when the layer has no
/// mask.
///
/// The rectangle is the mask's own tiles UNION the layer's rectangle, so every
/// pixel the layer actually has is covered by real mask data and `outside`
/// speaks only for the emptiness beyond. Taking the mask's tiles alone would
/// leave a masked layer's own pixels reading PSD's default colour, which is the
/// one place this could get the picture wrong rather than merely large.
void encodeMask(const Layer& layer, OutRecord& rec, std::int32_t canvasW,
                std::int32_t canvasH) {
    if (!layer.mask.has_value()) return;

    std::int32_t left = rec.left, top = rec.top, right = rec.right, bottom = rec.bottom;
    for (const auto& [key, tile] : layer.mask->tiles) {
        const std::int32_t x0 = std::max(0, key.first  * TILE_SIZE);
        const std::int32_t y0 = std::max(0, key.second * TILE_SIZE);
        const std::int32_t x1 = std::min(canvasW, (key.first  + 1) * TILE_SIZE);
        const std::int32_t y1 = std::min(canvasH, (key.second + 1) * TILE_SIZE);
        if (x0 >= x1 || y0 >= y1) continue;
        if (right <= left || bottom <= top) { left = x0; top = y0; right = x1; bottom = y1; }
        left   = std::min(left,   x0);
        top    = std::min(top,    y0);
        right  = std::max(right,  x1);
        bottom = std::max(bottom, y1);
    }
    // Nothing painted and nothing masked: `outside` alone would still be honest,
    // but a zero-size plane is the sort of thing other readers disagree about,
    // and a layer with no pixels has nothing for a mask to hide anyway.
    if (right <= left || bottom <= top) return;

    const auto w = static_cast<std::size_t>(right - left);
    const auto h = static_cast<std::size_t>(bottom - top);
    std::vector<std::uint8_t> plane(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            plane[y * w + x] = maskCoverage(*layer.mask,
                                            left + static_cast<std::int32_t>(x),
                                            top  + static_cast<std::int32_t>(y));

    rec.maskPresent = true;
    rec.maskEnabled = layer.mask->enabled;
    rec.maskOutside = layer.mask->outside;
    rec.maskLeft = left;
    rec.maskTop = top;
    rec.maskRight = right;
    rec.maskBottom = bottom;
    packChannel(rec.maskChannel, plane.data(), w, h);
}

/// Walks one nesting level and appends its records bottom to top, which is the
/// order PSD stores them in. A group becomes its closing marker, its children,
/// then its header — the mirror of what readPsd() expects.
void emitLevel(const Document& doc, std::optional<LayerId> parent, int depth,
               std::vector<OutRecord>& out) {
    if (depth > kMaxNesting) return;

    for (const Layer& layer : doc.layers) {
        if (layer.parent != parent) continue;

        if (layer.kind == LayerKind::Folder) {
            OutRecord divider;
            divider.name    = "</Layer group>";
            divider.section = 3;
            for (std::vector<std::uint8_t>& channel : divider.channels)
                channel = emptyChannel();
            out.push_back(std::move(divider));

            emitLevel(doc, layer.id, depth + 1, out);

            OutRecord header;
            fillCommon(header, layer);
            header.section = 1;                      // an open folder
            for (std::vector<std::uint8_t>& channel : header.channels)
                channel = emptyChannel();
            // A group's mask rides on its header record, which is where
            // Photoshop puts one and where `readPsd` looks for it.
            encodeMask(layer, header, doc.width, doc.height);
            out.push_back(std::move(header));
        } else {
            OutRecord rec;
            fillCommon(rec, layer);
            boundsOf(layer, doc.width, doc.height, rec);
            encodeChannels(layer, rec);
            encodeMask(layer, rec, doc.width, doc.height);
            out.push_back(std::move(rec));
        }
    }
}

void writeRecord(Writer& w, const OutRecord& rec) {
    w.i32(rec.top);
    w.i32(rec.left);
    w.i32(rec.bottom);
    w.i32(rec.right);

    static constexpr std::array<std::int16_t, 4> kIds{-1, 0, 1, 2};   // alpha, R, G, B
    w.u16(static_cast<std::uint16_t>(rec.maskPresent ? 5 : 4));
    for (std::size_t i = 0; i < 4; ++i) {
        w.u16(static_cast<std::uint16_t>(kIds[i]));
        w.u32(static_cast<std::uint32_t>(rec.channels[i].size()));
    }
    if (rec.maskPresent) {                            // -2: the layer mask (#48)
        w.u16(static_cast<std::uint16_t>(-2));
        w.u32(static_cast<std::uint32_t>(rec.maskChannel.size()));
    }

    w.text("8BIM");
    w.text(rec.blendKey);
    w.u8(rec.opacity);
    w.u8(rec.clipping ? 1 : 0);
    // Bit 3 says the flags come from Photoshop 5.0 or later; bit 1 set means
    // hidden, which reads backwards but is what the format says.
    w.u8(0x08u | (rec.visible ? 0u : 0x02u) | (rec.preserveOpacity ? 0x01u : 0u));
    w.u8(0);                                          // filler

    const std::size_t extraAt = w.reserveLength();
    if (rec.maskPresent) {
        // The 20-byte form: rectangle, default colour, flags, and two bytes of
        // padding to keep the block on a multiple of four. Bit 1 of the flags
        // is "mask disabled", which reads backwards and is what the format
        // says — `readMaskBlock` above decodes exactly this.
        w.u32(20);
        w.i32(rec.maskTop);
        w.i32(rec.maskLeft);
        w.i32(rec.maskBottom);
        w.i32(rec.maskRight);
        w.u8(rec.maskOutside);
        w.u8(rec.maskEnabled ? 0u : 0x02u);
        w.pad(2);
    } else {
        w.u32(0);                                     // layer mask data: none
    }
    w.u32(0);                                         // layer blending ranges: none

    // The legacy Pascal name, padded so the field is a multiple of four. ASCII
    // only by construction: 'luni' below carries the real one.
    std::string legacy;
    for (const char c : rec.name) {
        if (legacy.size() == 255) break;
        legacy += static_cast<unsigned char>(c) < 0x80 ? c : '?';
    }
    w.u8(static_cast<std::uint8_t>(legacy.size()));
    w.text(legacy);
    w.pad((4 - ((legacy.size() + 1) % 4)) % 4);

    const std::vector<std::uint16_t> units = utf16From(rec.name);
    w.text("8BIM");
    w.text("luni");
    w.u32(static_cast<std::uint32_t>(4 + 2 * units.size() + (units.size() % 2 ? 2 : 0)));
    w.u32(static_cast<std::uint32_t>(units.size()));
    for (const std::uint16_t unit : units) w.u16(unit);
    if (units.size() % 2) w.pad(2);                   // keep the block on four

    if (rec.section.has_value()) {
        w.text("8BIM");
        w.text("lsct");
        w.u32(4);
        w.u32(*rec.section);
    }
    w.fillLength(extraAt);
}

std::expected<void, Error> writeAll(const std::vector<unsigned char>& bytes,
                                    const std::filesystem::path& path) {
    FILE* out = std::fopen(path.string().c_str(), "wb");
    if (out == nullptr)
        return fail(ErrorKind::Permission, "could not create " + path.string());

    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), out);
    const bool closed = std::fclose(out) == 0;
    if (written == bytes.size() && closed) return {};

    // A half-written PSD in the artist's export folder is worse than none.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return fail(ErrorKind::Io, "could not write " + path.string());
}

}  // namespace

std::expected<void, Error> writePsd(const Document& doc,
                                    const std::filesystem::path& path) {
    if (doc.width <= 0 || doc.height <= 0)
        return fail(ErrorKind::Malformed, "canvas has no size");
    if (doc.width > kMaxPsdDimension || doc.height > kMaxPsdDimension)
        return fail(ErrorKind::Malformed,
                    "PSD cannot hold a canvas larger than 30000 pixels on a side");

    const auto w = static_cast<std::size_t>(doc.width);
    const auto h = static_cast<std::size_t>(doc.height);

    std::vector<OutRecord> records;
    // PSD has no document background, so an opaque one has to become a layer or
    // the file opens with the artwork floating on nothing everywhere the layers
    // do not cover. Bottom-most, and named the way Photoshop names its own.
    if (doc.background.a != 0) {
        OutRecord bg;
        bg.name   = "Background";
        bg.right  = doc.width;
        bg.bottom = doc.height;

        const std::array<std::uint8_t, 4> value{doc.background.a, doc.background.r,
                                                doc.background.g, doc.background.b};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const std::vector<std::uint8_t> flat(w * h, value[channel]);
            packChannel(bg.channels[channel], flat.data(), w, h);
        }
        records.push_back(std::move(bg));
    }
    emitLevel(doc, std::nullopt, 0, records);

    // PSD counts layers in a signed 16-bit field, so this is the format's limit
    // rather than an arbitrary one.
    if (records.size() > 32767)
        return fail(ErrorKind::Malformed, "PSD cannot hold more than 32767 layers");

    Writer out;
    out.text("8BPS");
    out.u16(1);                                       // PSD, not PSB
    out.pad(6);                                       // reserved
    out.u16(4);                                       // RGBA
    out.i32(doc.height);
    out.i32(doc.width);
    out.u16(8);                                       // D-023: 8 bits, for now
    out.u16(3);                                       // RGB
    out.u32(0);                                       // colour mode data: none

    // Image resources: only the resolution, so the artist's DPI survives the
    // trip. Everything else in this section is Photoshop's own bookkeeping.
    {
        const std::size_t resourcesAt = out.reserveLength();
        out.text("8BIM");
        out.u16(1005);                                // ResolutionInfo
        out.pad(2);                                   // empty Pascal name, padded
        out.u32(16);
        const std::uint32_t fixed = doc.dpi << 16;    // 16.16 fixed point
        for (int i = 0; i < 2; ++i) {
            out.u32(fixed);
            out.u16(1);                               // display unit: inches
            out.u16(1);
        }

        // And the ICC profile, when the document has one (D-034). Written only
        // then: an untagged document is sRGB, Photoshop already reads an
        // untagged PSD as its working space, and adding a resource block to
        // every export would change files nobody asked to have changed.
        if (!doc.colourProfile.empty()) {
            out.text("8BIM");
            out.u16(1039);                            // ICC profile
            out.pad(2);                               // empty Pascal name, padded
            const auto size = static_cast<std::uint32_t>(doc.colourProfile.data.size());
            out.u32(size);
            out.out.insert(out.out.end(), doc.colourProfile.data.begin(),
                           doc.colourProfile.data.end());
            // Every resource payload is padded to an even length, and a reader
            // that trusts the spec will be one byte out for the whole rest of
            // the section without this.
            if (size % 2u != 0u) out.pad(1);
        }
        out.fillLength(resourcesAt);
    }

    // Layer and mask information.
    {
        const std::size_t sectionAt = out.reserveLength();
        const std::size_t layerInfoAt = out.reserveLength();

        // Negative: the merged image at the end carries alpha.
        out.u16(static_cast<std::uint16_t>(-static_cast<std::int32_t>(records.size())));
        for (const OutRecord& rec : records) writeRecord(out, rec);
        for (const OutRecord& rec : records) {
            for (const std::vector<std::uint8_t>& channel : rec.channels) out.bytes(channel);
            // Last, because -2 is last in the channel list the record declared,
            // and the reader walks the two in the same order.
            if (rec.maskPresent) out.bytes(rec.maskChannel);
        }
        if (out.out.size() % 2 != 0) out.pad(1);      // the section is 2-aligned
        out.fillLength(layerInfoAt);

        out.u32(0);                                   // global layer mask info: none
        out.fillLength(sectionAt);
    }

    // The merged composite. Many viewers read only this, so it is not optional
    // — and flatten() is already exactly the buffer it wants (US-07.3).
    {
        const std::vector<StraightRgba8> flat = flatten(doc);
        if (flat.size() != w * h)
            return fail(ErrorKind::Malformed, "the canvas could not be flattened");

        std::vector<std::uint8_t> planes(4 * w * h);
        for (std::size_t i = 0; i < flat.size(); ++i) {
            planes[i]             = flat[i].r;
            planes[w * h + i]     = flat[i].g;
            planes[2 * w * h + i] = flat[i].b;
            planes[3 * w * h + i] = flat[i].a;
        }
        const Packed packed = packPlanes(planes.data(), w, h, 4);
        out.u16(1);                                   // RLE
        out.bytes(packed.counts);
        out.bytes(packed.data);
    }

    return writeAll(out.out, path);
}

}  // namespace sbl
