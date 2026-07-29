#include "sbl/colour.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>

#include "lcms2.h"

// Everything lcms2 touches is behind this file. `colour.hpp` names no lcms2
// type, so a future replacement — or a build without it — is one file's work
// rather than a search through the compositor (D-034).

#include "sbl/canvas.hpp"   // PremulRgba8, and its rounding, not a second copy

namespace sbl {
namespace {

struct ProfileDeleter {
    void operator()(void* p) const noexcept { cmsCloseProfile(p); }
};
struct TransformDeleter {
    void operator()(void* t) const noexcept { cmsDeleteTransform(t); }
};
using ProfileHandle   = std::unique_ptr<void, ProfileDeleter>;
using TransformHandle = std::unique_ptr<void, TransformDeleter>;

[[nodiscard]] ProfileHandle open(const IccProfile& profile) {
    if (profile.empty()) return nullptr;
    return ProfileHandle{cmsOpenProfileFromMem(
        profile.data.data(), static_cast<cmsUInt32Number>(profile.data.size()))};
}

/// The process-wide display profile, and the transforms built against it.
///
/// The cache is keyed on the source profile's bytes and emptied whenever the
/// display changes, so it holds one entry per distinct document profile — a
/// number in the low single digits even once #50 lands several documents at
/// once. Without it `cmsCreateTransform` would run once per 256 x 256 tile per
/// repaint, which is milliseconds of profile parsing to convert a tenth of a
/// megapixel.
struct DisplayState {
    IccProfile profile;
    bool       resolved = false;   // has the environment fallback been tried?
    std::map<std::vector<std::uint8_t>, TransformHandle> transforms;
};

DisplayState& display() {
    static DisplayState state;
    return state;
}

/// Relative colorimetric with black point compensation.
///
/// Not perceptual: perceptual only means anything when the profile carries a
/// perceptual table, and lcms2 falls back to colorimetric when it does not — so
/// asking for it buys inconsistency between one artist's profile and the next.
/// Relative colorimetric leaves every in-gamut colour exactly where it is,
/// which for a wide-gamut display showing an sRGB document means the whole
/// document, and black point compensation is what keeps the shadows from
/// crushing on a display whose black is not the document's.
constexpr cmsUInt32Number kIntent = INTENT_RELATIVE_COLORIMETRIC;
constexpr cmsUInt32Number kFlags =
    cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA;

/// Null when no conversion is called for, which is the common case and must
/// stay cheap. Never throws through lcms2's C frames.
[[nodiscard]] cmsHTRANSFORM displayTransform(const IccProfile& source) {
    const IccProfile& dst = displayProfile();
    if (dst.empty()) return nullptr;
    // Byte-identical profiles are the same profile. Cheaper than asking lcms2,
    // and it means a document tagged with the very profile the monitor uses
    // goes through no transform at all rather than through a near-identity one
    // that could still round a channel.
    if (source.data == dst.data) return nullptr;

    DisplayState& state = display();
    if (const auto it = state.transforms.find(source.data);
        it != state.transforms.end())
        return it->second.get();

    // An untagged document is sRGB (D-105, kept by D-034), and that is a
    // profile lcms2 can build for us rather than one we have to ship.
    ProfileHandle src = source.empty()
        ? ProfileHandle{cmsCreate_sRGBProfile()} : open(source);
    ProfileHandle out = open(dst);
    TransformHandle t;
    if (src != nullptr && out != nullptr)
        t.reset(cmsCreateTransform(src.get(), TYPE_RGBA_8, out.get(), TYPE_RGBA_8,
                                   kIntent, kFlags));

    // A null handle is cached too: a profile that cannot build a transform will
    // not start being able to, and retrying per tile turns one bad file into a
    // permanently slow canvas.
    cmsHTRANSFORM raw = t.get();
    state.transforms.emplace(source.data, std::move(t));
    return raw;
}

}  // namespace

bool isUsableProfile(const IccProfile& profile) {
    if (profile.empty()) return true;      // untagged is sRGB, which is usable
    const ProfileHandle p = open(profile);
    if (p == nullptr) return false;
    // Sable is an RGB painting program (D-004). A CMYK or Lab profile parses
    // perfectly well and would convert perfectly well, and then every tool in
    // the UI would still be handing it RGB. Refusing it here is what keeps the
    // failure at the import, where there is a warning to put it in.
    return cmsGetColorSpace(p.get()) == cmsSigRgbData;
}

std::string profileDescription(const IccProfile& profile) {
    if (profile.empty()) return "sRGB";
    const ProfileHandle p = open(profile);
    if (p == nullptr) return "unknown";
    char buffer[256]{};
    const cmsUInt32Number n = cmsGetProfileInfoASCII(
        p.get(), cmsInfoDescription, "en", "US", buffer, sizeof buffer);
    if (n == 0) return "unknown";
    // cmsGetProfileInfoASCII returns the byte count INCLUDING the terminator,
    // and a description with an embedded NUL would otherwise become a string
    // with one in the middle.
    return std::string(buffer);
}

void adoptColourProfile(Document& doc, IccProfile profile) {
    if (profile.empty()) return;               // untagged: nothing to say
    if (isUsableProfile(profile)) {
        doc.colourProfile = std::move(profile);
        return;
    }
    // Worded for the artist (#40), and it says what they will actually see:
    // the file claimed a colour space and the picture on screen is not in it.
    doc.warnings.push_back(
        "This file's colour profile is one Sable cannot use, so it has been "
        "opened as sRGB and its colours may not match the original.");
}

void setDisplayProfile(IccProfile profile) {
    DisplayState& state = display();
    state.profile  = std::move(profile);
    state.resolved = true;
    // Every cached transform ends at the old display. Keeping them would show
    // the next frame through the previous monitor's profile.
    state.transforms.clear();
}

const IccProfile& displayProfile() {
    DisplayState& state = display();
    if (!state.resolved) {
        state.resolved = true;
        // The engine cannot ask the window system what the monitor is (D-003),
        // so an environment variable is how a wide-gamut user says so until the
        // application grows a setting for it. Read once: re-reading per frame
        // would make a colour-managed canvas depend on a file the artist could
        // delete mid-stroke.
        if (const char* path = std::getenv("SABLE_DISPLAY_PROFILE");
            path != nullptr && path[0] != '\0') {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            // 128 bytes is an ICC header; anything smaller is not a profile.
            // The cap is there so a typo naming a video file does not read it.
            if (!ec && size >= 128 && size <= (16u << 20)) {
                std::ifstream in(path, std::ios::binary);
                IccProfile candidate;
                candidate.data.assign(std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>());
                if (in.good() || in.eof()) {
                    if (isUsableProfile(candidate) && !candidate.empty())
                        state.profile = std::move(candidate);
                }
            }
        }
    }
    return state.profile;
}

bool convertToDisplay(const IccProfile& source, void* premulRgba8,
                      std::size_t count) {
    if (premulRgba8 == nullptr || count == 0) return false;
    cmsHTRANSFORM t = displayTransform(source);
    if (t == nullptr) return false;

    // In place, in three passes over the same buffer, so a tile costs no
    // allocation: the canvas view runs this on every dirty tile of every
    // repaint, and a 256 KB scratch buffer per tile would be felt.
    //
    // Through `std::uint8_t*` rather than by casting the buffer between
    // PremulRgba8 and StraightRgba8. The two are layout-identical and the cast
    // would work; it is also exactly the aliasing D-004 made the type system
    // refuse, and a byte pointer is the one alias the language actually allows.
    auto* bytes = static_cast<std::uint8_t*>(premulRgba8);
    for (std::size_t i = 0; i < count * 4; i += 4) {
        const StraightRgba8 s =
            PremulRgba8{bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3]}
                .unpremultiply();
        bytes[i] = s.r; bytes[i + 1] = s.g; bytes[i + 2] = s.b; bytes[i + 3] = s.a;
    }
    cmsDoTransform(t, bytes, bytes, static_cast<cmsUInt32Number>(count));
    for (std::size_t i = 0; i < count * 4; i += 4) {
        const PremulRgba8 p =
            StraightRgba8{bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3]}
                .premultiply();
        bytes[i] = p.r; bytes[i + 1] = p.g; bytes[i + 2] = p.b; bytes[i + 3] = p.a;
    }
    return true;
}

}  // namespace sbl
