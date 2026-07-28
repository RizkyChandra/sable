// OpenRaster (.ora): the interchange format Krita, GIMP, MyPaint and Drawpile
// all read. Structurally a cousin of .sable — a ZIP with an XML manifest, PNG
// pixels and a merged composite — so it needs no dependency Sable did not
// already have.
//
// The one real difference from .sable: an ORA layer is a single PNG at an x/y
// offset, not a sparse tile map. Importing blits into the tile map, exporting
// computes each layer's bounding box.
#pragma once

#include <expected>
#include <filesystem>

#include "sbl/canvas.hpp"
#include "sbl/io.hpp"

namespace sbl {

/// Document::path is left alone — the registry owns it (D-024).
[[nodiscard]] std::expected<Document, Error> readOpenRaster(
    const std::filesystem::path& path);

/// Writes stack.xml, one PNG per raster layer, mergedimage.png (which is
/// exactly flatten()) and a thumbnail. Never modifies the document.
[[nodiscard]] std::expected<void, Error> writeOpenRaster(
    const Document& doc, const std::filesystem::path& path);

}  // namespace sbl
