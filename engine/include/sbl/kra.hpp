// Krita (.kra) import.
//
// ----------------------------------------------------------------------------
// Licensing — read this before touching this file
// ----------------------------------------------------------------------------
// Krita is GPL. Its FILE FORMAT is documented, and implementing a reader from
// that documentation is fine; copying, adapting or translating Krita's source
// code is not, because it would make Sable a derivative work and force it to
// relicense (D-020).
//
// Everything in kra.cpp was written from the published description of the
// container — a ZIP holding maindoc.xml plus one tiled binary file per layer —
// and from inspecting .kra files written by Krita 6.0.3 with `unzip`, exactly
// the way Sable's own format was designed to be debuggable. Where a detail was
// not in the documentation (the channel order of an RGBA8 tile, which
// compositeop id maps to which blend mode) it was established by having Krita
// write a file and reading the bytes back, and the comment at that spot says
// so. No Krita source was read.
//
// Import only, and paint layers only. Krita has filter layers, clone layers,
// masks, vector layers and animation; those are skipped with a message rather
// than failing the load.
#pragma once

#include <expected>
#include <filesystem>

#include "sbl/canvas.hpp"
#include "sbl/io.hpp"

namespace sbl {

/// Document::path is left alone — the registry owns it, and it must stay empty
/// for an import so that Ctrl+S cannot write a .sable archive over the
/// artist's .kra (D-024).
[[nodiscard]] std::expected<Document, Error> readKrita(
    const std::filesystem::path& path);

}  // namespace sbl
