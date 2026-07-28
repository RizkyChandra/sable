// Fonts, text layout, and the glyph rasteriser behind the text tool (#20).
//
// Why this is engine code and not app code: the text an artist types becomes
// ordinary tiles, and tiles are the engine's business. Putting it here also
// means the layout maths is unit-tested headlessly, like the rest of the
// engine — the app is left with the parts only a window can do, which are the
// IME and the caret.
//
// D-002 named the text tool as where ImGui's input handling stops being good
// enough; nothing in this header knows what a key press is. Composition,
// candidate windows and the caret belong to SDL3 and live in app/src.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sbl/canvas.hpp"
#include "sbl/io.hpp"

namespace sbl {

/// One loaded font file.
///
/// Move-only and reference-counting nothing: a face owns the bytes the
/// rasteriser reads glyph outlines out of, so it must outlive every call that
/// takes it. Loading is a file read and a header parse — cheap enough to do on
/// a font change, far too expensive to do per keystroke, which is why the tool
/// keeps one alive for the editing session.
class FontFace {
public:
    /// Reads the whole file: `.ttf`, `.ttc` and `.otf`. A file the rasteriser
    /// cannot parse comes back as an Error naming it, never as a face that
    /// draws nothing.
    [[nodiscard]] static std::expected<FontFace, Error> load(
        const std::filesystem::path& path);

    FontFace(FontFace&&) noexcept;
    FontFace& operator=(FontFace&&) noexcept;
    FontFace(const FontFace&)            = delete;
    FontFace& operator=(const FontFace&) = delete;
    ~FontFace();

    [[nodiscard]] const std::string& familyName() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    /// Advance width of `utf8` on one line, in canvas pixels, kerning included.
    /// What the caret and the IME candidate window are positioned with.
    [[nodiscard]] double advanceWidth(std::string_view utf8, float sizePx) const;

    /// Whether the font actually carries this character.
    ///
    /// A missing glyph still has an advance and still draws — as the empty box
    /// everyone knows. Asking first is how the tool can say "this font has no
    /// Japanese in it" instead of leaving the artist to wonder why they typed
    /// three characters and got three rectangles.
    [[nodiscard]] bool hasGlyph(std::uint32_t codepoint) const;

    /// The first character of `utf8` this font has no glyph for, or npos.
    [[nodiscard]] std::size_t firstMissingGlyph(std::string_view utf8) const;

    /// Vertical metrics at `sizePx`, in canvas pixels.
    [[nodiscard]] double ascent(float sizePx) const;
    [[nodiscard]] double descent(float sizePx) const;   // positive, below the baseline
    [[nodiscard]] double lineHeight(float sizePx) const;

private:
    FontFace();
    struct Impl;
    std::unique_ptr<Impl> impl_;   // stb_truetype stays out of every other TU

    /// The one function that needs the glyph outlines themselves. Everything
    /// else about a face is metrics, and those are public above.
    friend UndoRecord drawTextLayer(Layer&, const TextContent&, const FontFace&,
                                    std::int32_t, std::int32_t);
};

/// One laid-out line. Byte offsets, because that is what an IME hands us and
/// what a caret has to be able to sit in the middle of.
struct TextLine {
    std::size_t begin = 0, end = 0;   // byte range in TextContent::utf8, LF excluded
    double x = 0.0;                   // left edge, after alignment
    double baseline = 0.0;
    double width = 0.0;
};

struct TextLayout {
    std::vector<TextLine> lines;      // never empty: an empty string is one empty line
    double lineHeight = 0.0;
    double ascent = 0.0, descent = 0.0;
};

/// Splits on LF and places every line. No pixels are touched, so the caret can
/// be drawn on a frame where nothing was typed.
[[nodiscard]] TextLayout layoutText(const FontFace& face, const TextContent& content);

/// Which line a byte offset falls on. The offset at a line break belongs to the
/// line it ends, which is where a caret at the end of a line has to appear.
[[nodiscard]] std::size_t lineOf(const TextLayout& layout, std::size_t byteOffset);

/// Replaces every pixel of `layer` with `content` drawn through `face`, and
/// returns the record that undoes it.
///
/// The old tiles are MOVED into the record rather than copied, so re-drawing on
/// every keystroke costs no allocation beyond the new glyphs. A caller that
/// wants one undo step for a whole editing session keeps the first record and
/// merges later ones into it — see `mergeTextRecord`.
///
/// Writes host tiles directly rather than going through PaintBackend: the
/// result is a layer full of freshly created tiles, which is the same state a
/// document has just after loading, and a device backend uploads those lazily.
/// Call `PaintBackend::readback` first if a backend may be holding the layer's
/// current pixels — the snapshots in the record are taken from the host copy.
[[nodiscard]] UndoRecord drawTextLayer(Layer& layer, const TextContent& content,
                                       const FontFace& face,
                                       std::int32_t docWidth, std::int32_t docHeight);

/// Folds `next` into `into` so the pair undoes as one step.
///
/// Only tiles `into` has not already recorded are taken, because the state
/// `into` holds is the one from before the session started — which is exactly
/// what "undo the whole text edit" has to restore.
void mergeTextRecord(UndoRecord& into, UndoRecord&& next);

/// A font file found on this machine.
struct FontEntry {
    std::string name;                 // family name, or the file stem if it has none
    std::filesystem::path path;
};

/// Every usable font in the platform's font directories, sorted by name and
/// de-duplicated.
///
/// Scans directories rather than asking fontconfig or DirectWrite: the engine
/// links no platform library (D-003), and a directory walk is a few lines
/// against a dependency with a build system of its own. The cost is that a font
/// installed somewhere unusual is not listed — the artist can still open the
/// file, because the document stores a path, not a name.
[[nodiscard]] const std::vector<FontEntry>& systemFonts();

/// Ignores the cache and looks again. For a font installed while Sable is open.
void refreshSystemFonts();

// ------------------------------------------------------------ UTF-8 boundaries
// A caret that can land inside a multi-byte character is a caret that can cut a
// CJK glyph in half, so the tool never moves by bytes.

/// The start of the character before `at`, or 0.
[[nodiscard]] std::size_t utf8Prev(std::string_view text, std::size_t at) noexcept;
/// The start of the character after `at`, or text.size().
[[nodiscard]] std::size_t utf8Next(std::string_view text, std::size_t at) noexcept;

}  // namespace sbl
