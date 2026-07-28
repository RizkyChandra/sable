#include "sbl/text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <system_error>

// stb_truetype is a public-domain single header (D-003's "vendored single
// file" rule, fetched rather than copied in like everything else). It is the
// same rasteriser Dear ImGui bundles, so this adds a well-worn dependency
// rather than a new kind of one — but it is fetched from upstream, because the
// engine must build with SABLE_BUILD_APP=OFF and ImGui is not there then.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace sbl {
namespace {

std::unexpected<Error> fail(ErrorKind kind, std::string detail) {
    return std::unexpected(Error{kind, std::move(detail)});
}

/// One codepoint, advancing `i`. Malformed input yields U+FFFD and moves on by
/// a byte: a text tool must never loop or crash on a broken clipboard.
std::uint32_t decode(std::string_view s, std::size_t& i) noexcept {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = byte(i);
    const auto continues = [&](std::size_t k) {
        return k < s.size() && (byte(k) & 0xC0) == 0x80;
    };
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0 && continues(i + 1)) {
        const std::uint32_t cp = ((c & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && continues(i + 1) && continues(i + 2)) {
        const std::uint32_t cp = ((c & 0x0Fu) << 12) | ((byte(i + 1) & 0x3Fu) << 6) |
                                 (byte(i + 2) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && continues(i + 1) && continues(i + 2) && continues(i + 3)) {
        const std::uint32_t cp = ((c & 0x07u) << 18) | ((byte(i + 1) & 0x3Fu) << 12) |
                                 ((byte(i + 2) & 0x3Fu) << 6) | (byte(i + 3) & 0x3Fu);
        i += 4;
        return cp;
    }
    ++i;
    return 0xFFFD;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

// ----------------------------------------------------------------- font face

struct FontFace::Impl {
    std::vector<unsigned char> bytes;   // stbtt reads outlines straight out of this
    stbtt_fontinfo info{};
    std::string family;
    std::filesystem::path path;
};

FontFace::FontFace() : impl_(std::make_unique<Impl>()) {}
FontFace::FontFace(FontFace&&) noexcept = default;
FontFace& FontFace::operator=(FontFace&&) noexcept = default;
FontFace::~FontFace() = default;

const std::string& FontFace::familyName() const noexcept { return impl_->family; }
const std::filesystem::path& FontFace::path() const noexcept { return impl_->path; }

std::expected<FontFace, Error> FontFace::load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return fail(ErrorKind::NotFound, path.string() + " does not exist");

    FontFace face;
    const auto size = static_cast<std::size_t>(std::filesystem::file_size(path, ec));
    if (ec || size == 0)
        return fail(ErrorKind::Io, "could not read the font " + path.string());

    face.impl_->bytes.resize(size);
    if (FILE* in = std::fopen(path.string().c_str(), "rb"); in != nullptr) {
        const std::size_t got = std::fread(face.impl_->bytes.data(), 1, size, in);
        std::fclose(in);
        face.impl_->bytes.resize(got);
    } else {
        return fail(ErrorKind::Permission, "could not open the font " + path.string());
    }
    if (face.impl_->bytes.empty())
        return fail(ErrorKind::Io, "the font " + path.string() + " is empty");

    // Index 0 of a collection (.ttc). Picking a face out of a collection is a
    // preference dialog of its own; the first one is what every other
    // application shows by default.
    const int offset = stbtt_GetFontOffsetForIndex(face.impl_->bytes.data(), 0);
    if (offset < 0 ||
        stbtt_InitFont(&face.impl_->info, face.impl_->bytes.data(), offset) == 0)
        return fail(ErrorKind::Malformed,
                    path.string() + " is not a font Sable can read");

    face.impl_->path = path;

    // Name ID 1 is the family. The Microsoft record is UTF-16BE, which is the
    // only one a CJK font reliably fills in with something readable.
    int nameLength = 0;
    const char* name = stbtt_GetFontNameString(
        &face.impl_->info, &nameLength, STBTT_PLATFORM_ID_MICROSOFT,
        STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
    if (name != nullptr && nameLength > 1) {
        for (int i = 0; i + 1 < nameLength; i += 2) {
            const auto hi = static_cast<unsigned char>(name[i]);
            const auto lo = static_cast<unsigned char>(name[i + 1]);
            appendUtf8(face.impl_->family, static_cast<std::uint32_t>(hi << 8 | lo));
        }
    }
    if (face.impl_->family.empty()) face.impl_->family = path.stem().string();
    return face;
}

bool FontFace::hasGlyph(std::uint32_t codepoint) const {
    return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(codepoint)) != 0;
}

std::size_t FontFace::firstMissingGlyph(std::string_view utf8) const {
    for (std::size_t i = 0; i < utf8.size();) {
        const std::size_t start = i;
        const std::uint32_t cp = decode(utf8, i);
        if (cp != '\n' && !hasGlyph(cp)) return start;
    }
    return std::string_view::npos;
}

double FontFace::ascent(float sizePx) const {
    int a = 0, d = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &a, &d, &gap);
    return a * static_cast<double>(stbtt_ScaleForPixelHeight(&impl_->info, sizePx));
}

double FontFace::descent(float sizePx) const {
    int a = 0, d = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &a, &d, &gap);
    return -d * static_cast<double>(stbtt_ScaleForPixelHeight(&impl_->info, sizePx));
}

double FontFace::lineHeight(float sizePx) const {
    int a = 0, d = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &a, &d, &gap);
    return (a - d + gap) * static_cast<double>(
        stbtt_ScaleForPixelHeight(&impl_->info, sizePx));
}

double FontFace::advanceWidth(std::string_view utf8, float sizePx) const {
    const double scale = stbtt_ScaleForPixelHeight(&impl_->info, sizePx);
    double pen = 0.0;
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const std::uint32_t cp = decode(utf8, i);
        if (cp == '\n') continue;
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&impl_->info, static_cast<int>(cp), &advance, &bearing);
        if (previous != 0)
            pen += stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(previous),
                                                 static_cast<int>(cp)) * scale;
        pen += advance * scale;
        previous = cp;
    }
    return pen;
}

// --------------------------------------------------------------------- layout

TextLayout layoutText(const FontFace& face, const TextContent& content) {
    TextLayout out;
    out.ascent     = face.ascent(content.sizePx);
    out.descent    = face.descent(content.sizePx);
    // The artist's spacing multiplies the font's own idea of a line, so 1.0 is
    // "what the type designer intended" rather than "glyphs touching".
    out.lineHeight = face.lineHeight(content.sizePx) *
                     static_cast<double>(std::max(content.lineSpacing, 0.1f));

    std::size_t begin = 0;
    for (std::size_t i = 0; i <= content.utf8.size(); ++i) {
        if (i != content.utf8.size() && content.utf8[i] != '\n') continue;

        TextLine line;
        line.begin    = begin;
        line.end      = i;
        line.baseline = content.y +
                        static_cast<double>(out.lines.size()) * out.lineHeight;
        line.width    = face.advanceWidth(
            std::string_view(content.utf8).substr(begin, i - begin), content.sizePx);
        switch (content.align) {
            case TextAlign::Left:   line.x = content.x; break;
            case TextAlign::Centre: line.x = content.x - line.width * 0.5; break;
            case TextAlign::Right:  line.x = content.x - line.width; break;
        }
        out.lines.push_back(line);
        begin = i + 1;
    }
    return out;
}

std::size_t lineOf(const TextLayout& layout, std::size_t byteOffset) {
    for (std::size_t i = 0; i < layout.lines.size(); ++i)
        if (byteOffset <= layout.lines[i].end) return i;
    return layout.lines.empty() ? 0 : layout.lines.size() - 1;
}

// ---------------------------------------------------------------- rasterising

namespace {

/// Blends one glyph's coverage into the layer, clipped to the canvas.
///
/// Coverage scales an already-premultiplied colour, which is the only order
/// that leaves an antialiased edge the right brightness (D-004).
void blitCoverage(Layer& layer, const unsigned char* bitmap, int bw, int bh,
                  std::int32_t left, std::int32_t top, PremulRgba8 colour,
                  std::int32_t docWidth, std::int32_t docHeight) {
    // One lookup per tile rather than per pixel: a 48 px glyph is ~2000 pixels
    // and they nearly all land in the same tile.
    TileKey cached{0, 0};
    Tile* tile = nullptr;

    for (int gy = 0; gy < bh; ++gy) {
        const std::int32_t py = top + gy;
        if (py < 0 || py >= docHeight) continue;
        for (int gx = 0; gx < bw; ++gx) {
            const unsigned coverage = bitmap[static_cast<std::size_t>(gy) * bw + gx];
            if (coverage == 0) continue;
            const std::int32_t px = left + gx;
            if (px < 0 || px >= docWidth) continue;

            const TileKey key{tileIndex(px), tileIndex(py)};
            if (tile == nullptr || key != cached) {
                cached = key;
                tile   = &layer.tileFor(key);
            }
            const int tx = px - key.first  * TILE_SIZE;
            const int ty = py - key.second * TILE_SIZE;

            // Widened before coverage is applied, not after: the whole of a
            // glyph's antialiasing lives in this multiply, and doing it at
            // eight bits on a 16-bit layer would put stair-steps back into
            // exactly the edges the layer's depth was chosen to smooth.
            const auto scale = [&](std::uint16_t c) {
                return static_cast<std::uint16_t>((c * coverage + 127) / 255);
            };
            const PremulRgba16 wide = widen(colour);
            const PremulRgba16 src{scale(wide.r), scale(wide.g), scale(wide.b),
                                   scale(wide.a)};
            tile->setPixel(tx, ty, over(src, tile->pixel(tx, ty)));
        }
    }
}

}  // namespace

UndoRecord drawTextLayer(Layer& layer, const TextContent& content,
                         const FontFace& face, std::int32_t docWidth,
                         std::int32_t docHeight) {
    UndoRecord rec;
    rec.label = "Text";

    // Every tile goes: the text owns all of them, so "what was here before" is
    // the whole layer. Moved, not cloned — this runs on every keystroke.
    for (auto& [key, tile] : layer.tiles)
        rec.tiles.push_back(TileSnapshot{layer.id, key, std::move(tile)});
    layer.tiles.clear();

    const PremulRgba8 colour = content.colour.premultiply();
    const TextLayout layout = layoutText(face, content);
    const stbtt_fontinfo* info = &face.impl_->info;
    const auto scale = static_cast<double>(
        stbtt_ScaleForPixelHeight(info, content.sizePx));

    for (const TextLine& line : layout.lines) {
        // The same walk `advanceWidth` makes, so a caret placed by that
        // function cannot drift from the glyphs it is sitting between.
        double pen = line.x;
        std::uint32_t previous = 0;
        std::size_t i = line.begin;
        while (i < line.end) {
            const std::uint32_t cp = decode(content.utf8, i);
            const auto glyph = static_cast<int>(cp);
            if (previous != 0)
                pen += stbtt_GetCodepointKernAdvance(
                    info, static_cast<int>(previous), glyph) * scale;

            const auto ix = static_cast<std::int32_t>(std::floor(pen));
            const auto iy = static_cast<std::int32_t>(std::floor(line.baseline));
            // Subpixel horizontally only. Text sits on a baseline the eye reads
            // as a straight line, and a vertical shift per glyph is the one
            // place hinting-free rasterising looks visibly wrong.
            int bw = 0, bh = 0, xoff = 0, yoff = 0;
            unsigned char* bitmap = stbtt_GetCodepointBitmapSubpixel(
                info, static_cast<float>(scale), static_cast<float>(scale),
                static_cast<float>(pen - ix), 0.0f, glyph, &bw, &bh, &xoff, &yoff);
            if (bitmap != nullptr) {
                blitCoverage(layer, bitmap, bw, bh, ix + xoff, iy + yoff, colour,
                             docWidth, docHeight);
                stbtt_FreeBitmap(bitmap, nullptr);
            }

            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(info, glyph, &advance, &bearing);
            pen += advance * scale;
            previous = cp;
        }
    }

    // Anything newly created is part of the same change: undoing has to remove
    // it, which is what an empty `before` means.
    const std::size_t existing = rec.tiles.size();
    for (const auto& [key, tile] : layer.tiles) {
        const bool known = std::any_of(
            rec.tiles.begin(), rec.tiles.begin() + static_cast<std::ptrdiff_t>(existing),
            [&](const TileSnapshot& s) { return s.key == key; });
        if (!known) rec.tiles.push_back(TileSnapshot{layer.id, key, std::nullopt});
    }
    return rec;
}

// ------------------------------------------------------------- system fonts

namespace {

std::vector<FontEntry> g_fonts;
bool g_scanned = false;

bool isFontFile(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ttf" || ext == ".ttc" || ext == ".otf";
}

void scanInto(std::vector<FontEntry>& out, const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;

    auto it = std::filesystem::recursive_directory_iterator(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return;
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        // A font directory with thousands of files is a real thing on a
        // developer's machine, and every entry costs a combo box row.
        if (out.size() >= 2000) break;
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
        if (!isFontFile(it->path())) continue;
        // The file stem, not the family name: the name lives inside the file,
        // and opening two thousand fonts to build a list is not something an
        // artist should wait for. The real family name is read on load.
        out.push_back(FontEntry{it->path().stem().string(), it->path()});
    }
}

}  // namespace

void refreshSystemFonts() {
    g_fonts.clear();
    g_scanned = false;
}

const std::vector<FontEntry>& systemFonts() {
    if (g_scanned) return g_fonts;
    g_scanned = true;

    std::vector<std::filesystem::path> roots{
        "/usr/share/fonts", "/usr/local/share/fonts",
        "/System/Library/Fonts", "/Library/Fonts",
        "C:/Windows/Fonts",
    };
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        roots.emplace_back(std::filesystem::path(home) / ".local/share/fonts");
        roots.emplace_back(std::filesystem::path(home) / ".fonts");
        roots.emplace_back(std::filesystem::path(home) / "Library/Fonts");
    }
    if (const char* dir = std::getenv("SABLE_FONT_DIR"); dir != nullptr && *dir != '\0')
        roots.emplace_back(dir);

    for (const std::filesystem::path& root : roots) scanInto(g_fonts, root);

    std::ranges::sort(g_fonts, {}, &FontEntry::name);
    const auto duplicates = std::ranges::unique(g_fonts, {}, &FontEntry::name);
    g_fonts.erase(duplicates.begin(), duplicates.end());
    return g_fonts;
}

// ----------------------------------------------------------- UTF-8 boundaries

std::size_t utf8Prev(std::string_view text, std::size_t at) noexcept {
    if (at == 0) return 0;
    std::size_t i = std::min(at, text.size());
    do { --i; } while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80);
    return i;
}

std::size_t utf8Next(std::string_view text, std::size_t at) noexcept {
    if (at >= text.size()) return text.size();
    std::size_t i = at + 1;
    while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
    return i;
}

}  // namespace sbl
