// ICC colour management (D-034, superseding D-105): what a document's numbers
// mean, and what has to happen to them on the way to a particular screen.
//
// Deliberately free of `canvas.hpp` — a profile is bytes and a document is
// pixels, and keeping the edge one-way is what lets `Document` hold an
// `IccProfile` without a circular include.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sbl {

struct Document;   // canvas.hpp; see adoptColourProfile below

/// The raw bytes of an embedded ICC profile.
///
/// **Empty means untagged, and untagged means sRGB.** That is not a guess we
/// are papering over: it is what D-105 decided, what every file Sable has
/// written so far contains, and what a new document still is. The empty case
/// therefore has to be free — see `convertToDisplay`.
struct IccProfile {
    std::vector<std::uint8_t> data;

    [[nodiscard]] bool empty() const noexcept { return data.empty(); }
    friend bool operator==(const IccProfile&, const IccProfile&) = default;
};

/// Whether these bytes parse as a profile lcms2 can build a transform from.
///
/// Importers call this before storing anything. Every profile Sable meets came
/// out of a file some other program wrote, and a truncated or foreign blob in
/// the profile slot must arrive as "untagged, and here is why" rather than be
/// carried to the compositor and fail there, once per tile, with nowhere to
/// report it.
[[nodiscard]] bool isUsableProfile(const IccProfile& profile);

/// The profile's own description, for the status bar and the manifest.
/// "sRGB" for an untagged profile, because that is what untagged means.
[[nodiscard]] std::string profileDescription(const IccProfile& profile);

/// Records what an importer found, or says why it did not, on `doc.warnings`.
///
/// One implementation for PSD, ORA, KRA and `.sable` alike. Three importers
/// each deciding for themselves what "the profile is rubbish" means is how one
/// of them ends up keeping it, and a document holding a profile nothing can
/// build a transform from is a canvas that silently stops being managed.
///
/// An empty or unusable profile leaves the document untagged — sRGB, exactly
/// what it was before this feature existed.
void adoptColourProfile(Document& doc, IccProfile profile);

/// What the monitor is.
///
/// **Unset by default, and unset means "convert nothing".** The engine links no
/// display server (D-003) and cannot discover the monitor's profile by itself;
/// no portable API hands it over. Assuming sRGB and converting to it would
/// change every existing painting's appearance on the strength of a guess,
/// which is precisely the outcome #53 forbids. So conversion starts when
/// somebody says what the display actually is, and not before.
///
/// Set it from the application, or from the `SABLE_DISPLAY_PROFILE` environment
/// variable naming an `.icc` file, which is what `displayProfile()` falls back
/// to the first time it is asked.
///
/// Process-wide and unsynchronised, like `setPaintBackend`: set it on the UI
/// thread between frames. Nothing off the UI thread converts for display.
///
/// Changing it invalidates every composited tile the caller is holding —
/// `CanvasView::markAllDirty()` — because those pixels were converted for the
/// previous monitor and this function has no way to reach them.
void setDisplayProfile(IccProfile profile);

/// Empty when no display profile is in force. Reads `SABLE_DISPLAY_PROFILE`
/// once, on the first call, unless `setDisplayProfile` has already been called.
[[nodiscard]] const IccProfile& displayProfile();

/// Converts `count` premultiplied RGBA8 pixels in place, from `source` to the
/// display profile. Returns false — having touched nothing at all — when there
/// is no work: no display profile set, `source` byte-identical to it, or either
/// one unusable.
///
/// "Touched nothing at all" is the load-bearing part. An untagged document on a
/// default build must come out of the compositor bit for bit as it did before
/// this file existed, so the no-work path is an early return and not a
/// round trip through an identity transform that could round a channel.
///
/// Premultiplied in, premultiplied out: a colour transform is nonlinear, so the
/// pixels are unpremultiplied first and multiplied back after. That round trip
/// costs up to a step or two on a semi-transparent pixel; it is only reached
/// when a conversion is happening anyway, and the conversion moves the colour
/// far further than the rounding does.
[[nodiscard]] bool convertToDisplay(const IccProfile& source, void* premulRgba8,
                                    std::size_t count);

}  // namespace sbl
