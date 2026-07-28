// Persisted preferences: brush sizes, stabilizer level, colour, and tablet
// calibration. US-09.6 and US-12.3 both require settings to survive a restart.
//
// A flat key=value file rather than JSON. nlohmann/json is not fetched yet
// (D-014) and a preferences file does not justify pulling it in — this is
// thirty lines of parsing against a dependency plus a schema.
#pragma once

#include <map>
#include <string>

#include "sbl/input.hpp"

struct Settings {
    std::map<std::string, std::string> values;

    [[nodiscard]] float       getFloat(const std::string& key, float fallback) const;
    [[nodiscard]] int         getInt(const std::string& key, int fallback) const;
    [[nodiscard]] std::string getString(const std::string& key,
                                        const std::string& fallback) const;

    void set(const std::string& key, const std::string& value);
    void setFloat(const std::string& key, float value);
    void setInt(const std::string& key, int value);
};

/// Missing or unreadable is not an error — it is a first run. Never throws,
/// never fails the launch.
[[nodiscard]] Settings loadSettings();
/// Best effort. A preferences file that cannot be written must not cost the
/// artist their session.
void saveSettings(const Settings& settings);

/// Calibration round-trips through the same file. The curve is stored as
/// "x,y x,y ..." so it stays greppable when someone has to debug it by hand.
void storeProfile(Settings& settings, const std::string& prefix,
                  const sbl::TabletProfile& profile);
[[nodiscard]] sbl::TabletProfile readProfile(const Settings& settings,
                                             const std::string& prefix);
