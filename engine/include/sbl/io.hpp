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

/// Never modifies the document, and never clears the dirty flag (US-07.5).
[[nodiscard]] std::expected<void, Error> exportPng(
    const Document& doc, const std::filesystem::path& path);

/// The colour actually visible at a canvas pixel: every visible layer
/// composited over the background, then unpremultiplied (US-13.3, US-13.4).
///
/// Composites one pixel rather than calling flatten() and indexing it — the
/// colour picker runs on a click, and flattening 4000 x 4000 to read four
/// bytes is the kind of thing that gets noticed on the target hardware.
[[nodiscard]] StraightRgba8 pickColour(const Document& doc,
                                       std::int32_t x, std::int32_t y) noexcept;

}  // namespace sbl
