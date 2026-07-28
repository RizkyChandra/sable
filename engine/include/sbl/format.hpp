// The file format registry: one place that maps a file to a reader or a
// writer, so adding PSD, ORA or KRA touches one table instead of the app.
//
// Dispatch is by extension first and by content second. Extension alone is not
// enough here by design: .sable, .ora and .kra are all ZIP containers, so a
// file with the wrong name would otherwise be read by the wrong parser and
// fail with a confusing message instead of the right one.
//
// ----------------------------------------------------------------------------
// Writing an importer
// ----------------------------------------------------------------------------
//   1. Write a free function `std::expected<Document, Error> readThing(path)`
//      in engine/src/. Return an Error with a `detail` an artist can act on
//      (D-012); never throw, never return a half-filled Document.
//   2. Leave Document::path alone. The registry sets it, and only for the
//      native project format — see nativeProject below.
//   3. Write a sniff if the format shares an extension with another, or if
//      being renamed is plausible: `hasZipEntry(path, "mimetype")` and
//      `readMagic(path, 4)` cover both container families we support.
//   4. Add one Format to builtinFormats() in engine/src/format.cpp.
// Nothing in the app changes: the File menu and the dialog filters are built
// from this registry.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "sbl/canvas.hpp"
#include "sbl/io.hpp"

namespace sbl {

/// Plain function pointers rather than std::function: every reader and writer
/// is a free function, and this keeps the registry allocation-free.
using ReadFn  = std::expected<Document, Error> (*)(const std::filesystem::path&);
using WriteFn = std::expected<void, Error> (*)(const Document&, const std::filesystem::path&);
using SniffFn = bool (*)(const std::filesystem::path&);

struct Format {
    std::string id;                        // "sable", "png", "psd"
    std::string label;                     // shown in the file dialog
    std::vector<std::string> extensions;   // lowercase, no dot; first one is the default

    /// True only for Sable's own project format.
    ///
    /// This is the difference between Save and Export, and between Open and
    /// Import: a native file is the document, an imported one is a copy of
    /// someone else's file. It is also what keeps Ctrl+S honest — importDocument
    /// clears Document::path for everything else, because doSave() writes
    /// straight to that path with the .sable writer, and an importer that left
    /// "painting.psd" there would destroy the artist's PSD.
    bool nativeProject = false;

    ReadFn  read  = nullptr;               // null: this format cannot be opened
    WriteFn write = nullptr;               // null: this format cannot be written

    /// Cheap content check: does this file look like this format? Optional, but
    /// it is the only thing that can tell two ZIP containers apart.
    SniffFn sniff = nullptr;
};

/// Every registered format, built-ins first, in registration order.
[[nodiscard]] const std::vector<Format>& formats();

/// Adds a format at runtime. The built-ins register themselves on first use;
/// this exists for tests and for anything loaded later.
void registerFormat(Format format);

/// Opens any readable file, choosing the reader by extension and then by
/// content. Document::path is set only for the native project format.
[[nodiscard]] std::expected<Document, Error> importDocument(
    const std::filesystem::path& path);

/// Writes through the format matching the extension. Never touches the
/// document, dirty flag included (US-07.5).
[[nodiscard]] std::expected<void, Error> exportDocument(
    const Document& doc, const std::filesystem::path& path);

/// One entry of a file dialog's filter list. `pattern` is semicolon-separated
/// and extensionless, which is exactly what SDL_DialogFileFilter wants.
struct DialogFilter {
    std::string label;
    std::string pattern;      // "sable", "jpg;jpeg"
};

enum class FormatUse { Read, Write };

/// The filters for one dialog. `native` picks the Open/Save side (Sable's own
/// project) or the Import/Export side (everyone else's formats).
[[nodiscard]] std::vector<DialogFilter> dialogFilters(FormatUse use, bool native);

// --------------------------------------------------------------- sniff helpers

/// The first `count` bytes, or fewer if the file is shorter. Empty if it cannot
/// be read at all — a sniff must never throw or crash on rubbish input.
[[nodiscard]] std::string readMagic(const std::filesystem::path& path, std::size_t count);

/// True if `path` is a ZIP archive containing `entry`. How the three ZIP-based
/// formats tell each other apart: document.json, mimetype, maindoc.xml.
[[nodiscard]] bool hasZipEntry(const std::filesystem::path& path, const char* entry);

}  // namespace sbl
