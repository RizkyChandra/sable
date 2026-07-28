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
/// Blend modes Sable cannot do degrade to Normal rather than failing the read.
/// Depths other than 8 bits and colour modes other than RGB are refused with a
/// message naming what the file actually is, which is the honest answer until
/// 16-bit support (D-023) lands.
[[nodiscard]] std::expected<Document, Error> readPsd(const std::filesystem::path& path);

}  // namespace sbl
