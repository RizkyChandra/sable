#include "sbl/format.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <system_error>

#include "miniz.h"
#include "miniz_zip.h"
#include "sbl/project.hpp"

namespace sbl {
namespace {

std::unexpected<Error> fail(ErrorKind kind, std::string detail) {
    return std::unexpected(Error{kind, std::move(detail)});
}

/// Lowercase, without the dot. Extensions are compared, never displayed, so
/// ASCII lowering is enough — no format in this registry has a non-ASCII one.
std::string extensionOf(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool handles(const Format& format, const std::string& extension) {
    return std::ranges::find(format.extensions, extension) != format.extensions.end();
}

bool looksLikeSable(const std::filesystem::path& path) {
    return hasZipEntry(path, "document.json");
}

/// The built-in table. An importer adds one entry here and nothing else.
std::vector<Format> builtinFormats() {
    std::vector<Format> all;
    all.push_back(Format{
        .id = "sable", .label = "Sable project", .extensions = {"sable"},
        .nativeProject = true,
        .read = &loadProject, .write = &saveProject, .sniff = &looksLikeSable});
    all.push_back(Format{
        .id = "png", .label = "PNG image", .extensions = {"png"},
        .nativeProject = false,
        .read = nullptr, .write = &exportPng, .sniff = nullptr});
    return all;
}

std::vector<Format>& registry() {
    static std::vector<Format> all = builtinFormats();
    return all;
}

/// Extension first, content second (the header says why).
const Format* readerFor(const std::filesystem::path& path) {
    const std::string ext = extensionOf(path);

    std::vector<const Format*> byExtension;
    for (const Format& format : formats())
        if (format.read != nullptr && handles(format, ext)) byExtension.push_back(&format);

    // The common case: the extension matches and the content agrees, or the
    // format has no sniff and is taken at its word.
    for (const Format* format : byExtension)
        if (format->sniff == nullptr || format->sniff(path)) return format;

    // A renamed file — a .kra called .sable, say. Content is the only truth
    // left, so ask every format that can answer.
    for (const Format& format : formats())
        if (format.read != nullptr && format.sniff != nullptr && format.sniff(path))
            return &format;

    // The extension matched but nothing recognised the content: hand it to the
    // reader the name promised, so the artist gets a specific complaint about
    // the file rather than a generic one about the extension.
    return byExtension.empty() ? nullptr : byExtension.front();
}

const Format* writerFor(const std::filesystem::path& path) {
    const std::string ext = extensionOf(path);
    for (const Format& format : formats())
        if (format.write != nullptr && handles(format, ext)) return &format;
    return nullptr;
}

/// "sable, png" — for an error message that says what would have worked.
std::string extensionList(FormatUse use) {
    std::string out;
    for (const Format& format : formats()) {
        if (use == FormatUse::Read ? format.read == nullptr : format.write == nullptr) continue;
        for (const std::string& ext : format.extensions) {
            if (!out.empty()) out += ", ";
            out += '.' + ext;
        }
    }
    return out;
}

}  // namespace

const std::vector<Format>& formats() { return registry(); }

void registerFormat(Format format) { registry().push_back(std::move(format)); }

std::expected<Document, Error> importDocument(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return fail(ErrorKind::NotFound, path.string() + " does not exist");

    const Format* format = readerFor(path);
    if (format == nullptr)
        return fail(ErrorKind::Malformed,
                    "Sable cannot open " + path.string() +
                    ". It reads " + extensionList(FormatUse::Read) + ".");

    std::expected<Document, Error> doc = format->read(path);
    if (!doc.has_value()) return doc;

    // The trap, closed here rather than trusted to every importer: doSave()
    // writes straight to Document::path with the .sable writer, so a reader
    // that left "painting.psd" there would have Ctrl+S overwrite the artist's
    // PSD with a ZIP. Only the native project format owns its path; an import
    // has none, which sends Ctrl+S to Save As where it belongs.
    if (format->nativeProject) doc->path = path;
    else                       doc->path.clear();
    return doc;
}

std::expected<void, Error> exportDocument(const Document& doc,
                                          const std::filesystem::path& path) {
    const Format* format = writerFor(path);
    if (format == nullptr)
        return fail(ErrorKind::Malformed,
                    "Sable cannot write " + path.string() +
                    ". It writes " + extensionList(FormatUse::Write) + ".");
    return format->write(doc, path);
}

std::vector<DialogFilter> dialogFilters(FormatUse use, bool native) {
    std::vector<DialogFilter> filters;
    for (const Format& format : formats()) {
        if (format.nativeProject != native) continue;
        if (use == FormatUse::Read ? format.read == nullptr : format.write == nullptr) continue;

        std::string pattern;
        for (const std::string& ext : format.extensions) {
            if (!pattern.empty()) pattern += ';';
            pattern += ext;
        }
        filters.push_back(DialogFilter{format.label, std::move(pattern)});
    }
    return filters;
}

std::string readMagic(const std::filesystem::path& path, std::size_t count) {
    std::string head;
    FILE* in = std::fopen(path.string().c_str(), "rb");
    if (in == nullptr) return head;
    head.resize(count);
    head.resize(std::fread(head.data(), 1, count, in));
    std::fclose(in);
    return head;
}

bool hasZipEntry(const std::filesystem::path& path, const char* entry) {
    // Cheap enough to run on every candidate: miniz reads the central
    // directory, not the file bodies.
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.string().c_str(), 0)) return false;
    const bool found = mz_zip_reader_locate_file(&zip, entry, nullptr, 0) >= 0;
    mz_zip_reader_end(&zip);
    return found;
}

}  // namespace sbl
