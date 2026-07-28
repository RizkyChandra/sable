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
/// 2 added `vanishing_points`. 3 added `selection`, and `selection.png` beside
/// the tiles for the coverage mask of a lasso or wand selection. 4 added text
/// layers: `"kind": "text"` and a `text` object beside it.
///
/// Everything each bump adds is optional, so v1, v2 and v3 files still load
/// unchanged — the bumps exist so an OLDER Sable refuses a file whose contents
/// it would silently drop on the next save. For text that matters most: an
/// older Sable would open a v4 file, drop the words, and write back a picture
/// the artist can no longer edit.
///
/// 5 added 16-bit colour (D-023): `colour.depth` is 16 and the tile PNGs are
/// 16 bits per channel. This is the first bump that is NOT written
/// unconditionally — see below.
inline constexpr int SABLE_FORMAT_VERSION = 5;

/// What an 8-bit document declares, which is the last version whose contents
/// an older Sable can read in full.
///
/// The bumps above were all unconditional because every one of them added
/// something an ordinary document might contain. Depth is different: it is a
/// per-document choice, and the overwhelming majority of documents will never
/// make it. Writing 5 on all of them would lock every 8-bit painting out of an
/// older Sable in exchange for nothing, so the version says what the file
/// actually needs — and a 16-bit file, which an older Sable genuinely would
/// misread, still gets refused by name.
inline constexpr int SABLE_FORMAT_VERSION_8BIT = 4;

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
