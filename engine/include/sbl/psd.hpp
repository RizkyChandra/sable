// Photoshop PSD interchange.
//
// PSD is what every other painting application reads, so it is the one format
// that decides whether work can get into and out of Sable at all.
//
// Registered in engine/src/format.cpp; nothing else should call these
// directly — importDocument()/exportDocument() are the front door (D-024).
#pragma once

#include <expected>
#include <filesystem>

#include "sbl/canvas.hpp"
#include "sbl/io.hpp"

namespace sbl {

/// Reads an 8-bit RGB PSD: canvas size, layer stack, names, opacity,
/// visibility, groups, blend modes and clipping.
///
/// The background is left fully transparent, because PSD has no notion of one:
/// whatever backs the artwork is a layer in the file and stays a layer here.
///
/// A layer mask is multiplied into that layer's alpha as it is read (D-027,
/// #35). Sable has nowhere to keep a mask, and an import that dropped it showed
/// content the file itself hides. The mask is therefore no longer editable
/// afterwards — what it hid is gone, not hidden. A mask on a GROUP still is
/// dropped: it applies to the folder's composited result, which is not any one
/// child's pixels.
///
/// Blend modes Sable cannot do degrade to Normal rather than failing the read.
/// Depths other than 8 bits and colour modes other than RGB are refused with a
/// message naming what the file actually is, which is the honest answer until
/// 16-bit support (D-023) lands.
[[nodiscard]] std::expected<Document, Error> readPsd(const std::filesystem::path& path);

/// Writes a layered PSD: the stack, names, opacity, visibility, groups, the
/// blend modes that map, and a flattened composite section — many viewers read
/// only that composite, so it is not optional.
///
/// Never modifies the document, dirty flag included (US-07.5); the const
/// reference is the guarantee.
///
/// A document background that is not fully transparent is written as an extra
/// bottom-most layer called "Background", because PSD has nowhere else to put
/// one. Opening the result in Sable therefore gives one more layer than was
/// exported, and identical pixels.
[[nodiscard]] std::expected<void, Error> writePsd(const Document& doc,
                                                  const std::filesystem::path& path);

}  // namespace sbl
