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
///
/// 2 added `vanishing_points`. Everything it adds is optional, so a v1 file
/// still loads unchanged — the bump exists so an OLDER Sable refuses a file
/// whose perspective guides it would silently drop on the next save.
inline constexpr int SABLE_FORMAT_VERSION = 2;

/// Undo history is deliberately not saved.
[[nodiscard]] std::expected<void, Error> saveProject(
    const Document& doc, const std::filesystem::path& path);

[[nodiscard]] std::expected<Document, Error> loadProject(
    const std::filesystem::path& path);

/// Where recovery data goes (D-013). NEVER over the artist's own file — this
/// is the one failure mode that destroys work permanently.
///
/// Defaults to the XDG state directory. The engine links no SDL (D-003), so it
/// cannot ask the platform where user data belongs; on Windows and macOS the
/// XDG guess is wrong and would land in a temporary directory that the system
/// is free to erase. The application therefore supplies the real location at
/// startup via setRecoveryDirectory().
[[nodiscard]] std::filesystem::path recoveryDirectory();

/// Overrides the location. Pass an empty path to return to the default.
void setRecoveryDirectory(std::filesystem::path directory);

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
