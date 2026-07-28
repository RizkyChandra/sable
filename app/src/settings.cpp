#include "settings.hpp"

#include <SDL3/SDL.h>

#include <charconv>
#include <cstdio>
#include <sstream>
#include <string_view>

namespace {

/// SDL_GetPrefPath returns the platform's per-user *data* directory and
/// creates it. On Linux that is $XDG_DATA_HOME (~/.local/share), not
/// XDG_CONFIG_HOME — worth knowing before hunting for the file by hand.
std::string settingsPath() {
    char* base = SDL_GetPrefPath("sable", "sable");
    if (base == nullptr) return {};
    std::string path = std::string(base) + "settings.conf";
    SDL_free(base);
    return path;
}

float parseFloat(std::string_view text, float fallback) {
    float value = fallback;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    if (std::from_chars(first, last, value).ec != std::errc{}) return fallback;
    return value;
}

}  // namespace

float Settings::getFloat(const std::string& key, float fallback) const {
    const auto it = values.find(key);
    return it == values.end() ? fallback : parseFloat(it->second, fallback);
}

int Settings::getInt(const std::string& key, int fallback) const {
    const auto it = values.find(key);
    if (it == values.end()) return fallback;
    int value = fallback;
    const auto* first = it->second.data();
    const auto* last = first + it->second.size();
    if (std::from_chars(first, last, value).ec != std::errc{}) return fallback;
    return value;
}

std::string Settings::getString(const std::string& key,
                                const std::string& fallback) const {
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

void Settings::set(const std::string& key, const std::string& value) {
    values[key] = value;
}

void Settings::setFloat(const std::string& key, float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", static_cast<double>(value));
    values[key] = buffer;
}

void Settings::setInt(const std::string& key, int value) {
    values[key] = std::to_string(value);
}

Settings loadSettings() {
    Settings settings;
    const std::string path = settingsPath();
    if (path.empty()) return settings;

    std::size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (data == nullptr) return settings;      // first run, not a failure

    std::istringstream stream(std::string(static_cast<const char*>(data), size));
    SDL_free(data);

    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t split = line.find('=');
        if (split == std::string::npos) continue;
        settings.values[line.substr(0, split)] = line.substr(split + 1);
    }
    return settings;
}

void saveSettings(const Settings& settings) {
    const std::string path = settingsPath();
    if (path.empty()) return;

    std::string out = "# Sable preferences. Edited by the application.\n";
    for (const auto& [key, value] : settings.values) out += key + "=" + value + "\n";

    // Best effort: a preferences file we cannot write is not worth a dialog,
    // and certainly not worth failing the session over.
    if (!SDL_SaveFile(path.c_str(), out.data(), out.size()))
        SDL_Log("could not save preferences to %s: %s", path.c_str(), SDL_GetError());
}

void storeProfile(Settings& settings, const std::string& prefix,
                  const sbl::TabletProfile& profile) {
    settings.setFloat(prefix + ".rawMin", profile.rawMin);
    settings.setFloat(prefix + ".rawMax", profile.rawMax);
    settings.setFloat(prefix + ".smoothing", profile.smoothing);

    std::string points;
    for (const auto& [x, y] : profile.curve.points) {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%g,%g ",
                      static_cast<double>(x), static_cast<double>(y));
        points += buffer;
    }
    settings.set(prefix + ".curve", points);
}

sbl::TabletProfile readProfile(const Settings& settings, const std::string& prefix) {
    sbl::TabletProfile profile;
    profile.rawMin    = settings.getFloat(prefix + ".rawMin", 0.0f);
    profile.rawMax    = settings.getFloat(prefix + ".rawMax", 1.0f);
    profile.smoothing = settings.getFloat(prefix + ".smoothing", 0.0f);

    const std::string points = settings.getString(prefix + ".curve", "");
    if (points.empty()) return profile;

    sbl::PressureCurve curve;
    std::istringstream stream(points);
    std::string pair;
    while (stream >> pair) {
        const std::size_t comma = pair.find(',');
        if (comma == std::string::npos) continue;
        curve.points.emplace_back(parseFloat(std::string_view(pair).substr(0, comma), 0.0f),
                                  parseFloat(std::string_view(pair).substr(comma + 1), 0.0f));
    }
    // A truncated or hand-mangled file must not produce a curve that maps
    // every pressure to zero. Two points is the minimum that means anything.
    if (curve.points.size() >= 2) {
        curve.normalise();
        profile.curve = std::move(curve);
    }
    return profile;
}
