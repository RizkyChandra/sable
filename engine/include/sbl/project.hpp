// The .sable project file (D-011): a ZIP container holding a JSON manifest and
// one PNG per non-empty tile.
//
//   document.json                          manifest
//   thumbnail.png                          256 px preview for file browsers
//   layers/<layerId>/tiles/<tx>_<ty>.png   straight alpha, one per tile
//
// A ZIP of PNGs plus JSON is inspectable with `unzip` and any image viewer,
// which makes save/load bugs debuggable without writing a parser first.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>

#include "sbl/canvas.hpp"
#include "sbl/io.hpp"

namespace sbl {

/// Present from the first release so v1 files stay loadable.
inline constexpr int SABLE_FORMAT_VERSION = 1;

/// Undo history is deliberately not saved.
[[nodiscard]] std::expected<void, Error> saveProject(
    const Document& doc, const std::filesystem::path& path);

[[nodiscard]] std::expected<Document, Error> loadProject(
    const std::filesystem::path& path);

/// Where recovery data goes (D-013). Under the XDG state directory, NEVER over
/// the artist's own file — this is the one failure mode that destroys work
/// permanently.
[[nodiscard]] std::filesystem::path recoveryDirectory();

/// Writes a recovery copy plus a small JSON note pointing at the original path.
/// `originalPath` may be empty for a document that has never been saved.
[[nodiscard]] std::expected<std::filesystem::path, Error> writeRecovery(
    const Document& doc, const std::filesystem::path& originalPath);

struct RecoveryEntry {
    std::filesystem::path recoveryFile;   // the .sable copy under the state dir
    std::filesystem::path originalPath;   // empty if it was never saved
    std::uint64_t savedAtEpochSeconds = 0;
};

/// Every recovery file currently on disk. Restoring is an explicit user
/// action, so this only reports — it never opens anything by itself.
[[nodiscard]] std::vector<RecoveryEntry> listRecoveries();

/// Called once the artist has saved properly, or declined the offer.
void clearRecovery(const std::filesystem::path& recoveryFile);

}  // namespace sbl
