// Errors, flattening, and PNG export.
// D-012: every engine API that can fail returns std::expected<T, Error>.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "sbl/canvas.hpp"

namespace sbl {

class PaintBackend;      // sbl/backend.hpp; see flatten's second overload

enum class ErrorKind { NotFound, Permission, Malformed, UnsupportedVersion, Io };

struct Error {
    ErrorKind   kind = ErrorKind::Io;
    std::string detail;      // shown to the user, so write it for one
};

[[nodiscard]] std::string_view describe(ErrorKind k) noexcept;

/// Composites the background and every visible layer into a straight-alpha
/// buffer, row-major, exactly width * height pixels.
///
/// Straight alpha is the point: this is the PremulRgba8 -> StraightRgba8
/// conversion, and getting it wrong greys every soft edge (US-07.3).
[[nodiscard]] std::vector<StraightRgba8> flatten(const Document& doc);

/// The same, through a named backend instead of the process default.
///
/// This exists for the one caller that must not take the default: the autosave
/// worker composites its thumbnail on a thread with no device context, so it
/// asks `cpuBackend()` by name. Everything else should use the overload above.
[[nodiscard]] std::vector<StraightRgba8> flatten(const Document& doc,
                                                 PaintBackend& backend);

/// One rectangle of what flatten() produces, still premultiplied, w * h pixels
/// with buffer pixel 0 at canvas pixel (x, y). Pixels outside the document are
/// transparent; inside, the background is already underneath.
///
/// The canvas view composites a tile at a time through this, so screen and
/// export cannot disagree about blend modes, clipping or groups (#1). Keep it
/// the only implementation of the compositing rules — a second one drifts.
[[nodiscard]] std::vector<PremulRgba8> compositeRect(const Document& doc,
                                                    std::int32_t x, std::int32_t y,
                                                    std::int32_t w, std::int32_t h);

/// Never modifies the document, and never clears the dirty flag (US-07.5).
///
/// Carries `doc.colourProfile` out in the PNG's `iCCP` chunk when there is one
/// (D-034), so the numbers Sable wrote are read back as the colours it meant.
[[nodiscard]] std::expected<void, Error> exportPng(
    const Document& doc, const std::filesystem::path& path);

/// The profile in a PNG's `iCCP` chunk, or an empty one if it carries none.
///
/// Parses the chunk rather than decoding the image: this is called on a
/// `mergedimage.png` an ORA or KRA container happens to hold, and inflating a
/// whole canvas to read a header would be the expensive way to learn nothing
/// most of the time. Never throws and never trusts a length — a PNG is a file
/// somebody else wrote.
[[nodiscard]] IccProfile iccProfileFromPng(const void* bytes, std::size_t size);

/// The colour actually visible at a canvas pixel: every visible layer
/// composited over the background, then unpremultiplied (US-13.3, US-13.4).
///
/// Composites one pixel rather than calling flatten() and indexing it — the
/// colour picker runs on a click, and flattening 4000 x 4000 to read four
/// bytes is the kind of thing that gets noticed on the target hardware.
///
/// Not `noexcept` for the same reason as `applyDab`: a backend that cannot
/// read the pixel back has to be able to record why.
[[nodiscard]] StraightRgba8 pickColour(const Document& doc,
                                       std::int32_t x, std::int32_t y);

}  // namespace sbl
