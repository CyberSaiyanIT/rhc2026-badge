#include <Tactility/service/music/Music.h>

#include <Tactility/file/PropertiesFile.h>

#include <app/paths.h>
#include <tactility/log.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <map>
#include <string>

namespace tt::service::music {

namespace {

constexpr auto* TAG = "MusicSettings";

/**
 * Every value the music player's Sound pages can change, and its starting value. Defaults live
 * here rather than in MusicService so first boot and "reset to defaults" cannot drift apart.
 */
struct Settings {
    int presetIndex = 0;
    int bandGains[EQUALIZER_BANDS] = {};
    int gainDb = 0;
    bool autoPreamp = true;

    Dynamics dynamics = Dynamics::Off;
    int speedPercent = 100;
    int pitchPercent = 100;
    int balancePercent = 0;
    bool monoDownmix = false;
    Fade fade = Fade::Short;

    bool vuSeeding = true;
};

constexpr Settings DEFAULTS {};

std::string settingsFilePath() {
    char path[256];
    if (app_paths_get_user_data_path("MusicPlayer", "sound.properties", path, sizeof(path)) != ERROR_NONE) {
        return {};
    }
    return path;
}

using Properties = std::map<std::string, std::string>;

int readInt(const Properties& properties, const std::string& key, int fallback, int min, int max) {
    const auto entry = properties.find(key);
    if (entry == properties.end()) {
        return fallback;
    }
    // A file edited by hand or written by an older build must not be able to index an enum out
    // of range, so everything is clamped rather than trusted.
    return (int) std::clamp(strtol(entry->second.c_str(), nullptr, 10), (long) min, (long) max);
}

Settings read() {
    Settings settings {};
    settings.presetIndex = getPresetIndex();
    for (int band = 0; band < EQUALIZER_BANDS; band++) {
        settings.bandGains[band] = getBandGainDb(band);
    }
    settings.gainDb = getGainDb();
    settings.autoPreamp = isAutoPreampEnabled();

    settings.dynamics = getDynamics();
    settings.speedPercent = getSpeedPercent();
    settings.pitchPercent = getPitchPercent();
    settings.balancePercent = getBalancePercent();
    settings.monoDownmix = isMonoDownmixEnabled();
    settings.fade = getFade();

    settings.vuSeeding = isVuSeedingEnabled();
    return settings;
}

void apply(const Settings& settings) {
    // Before the bands: setting a preset overwrites every one of them.
    setPresetIndex(settings.presetIndex);
    for (int band = 0; band < EQUALIZER_BANDS; band++) {
        setBandGainDb(band, settings.bandGains[band]);
    }
    setGainDb(settings.gainDb);
    setAutoPreampEnabled(settings.autoPreamp);

    setDynamics(settings.dynamics);
    setSpeedPercent(settings.speedPercent);
    setPitchPercent(settings.pitchPercent);
    setBalancePercent(settings.balancePercent);
    setMonoDownmixEnabled(settings.monoDownmix);
    setFade(settings.fade);

    setVuSeedingEnabled(settings.vuSeeding);
}

bool write(const Settings& settings) {
    const auto path = settingsFilePath();
    if (path.empty()) {
        return false;
    }

    Properties properties {
        { "preset", std::format("{}", settings.presetIndex) },
        { "gainDb", std::format("{}", settings.gainDb) },
        { "autoPreamp", settings.autoPreamp ? "1" : "0" },
        { "dynamics", std::format("{}", (int) settings.dynamics) },
        { "speedPercent", std::format("{}", settings.speedPercent) },
        { "pitchPercent", std::format("{}", settings.pitchPercent) },
        { "balancePercent", std::format("{}", settings.balancePercent) },
        { "monoDownmix", settings.monoDownmix ? "1" : "0" },
        { "fade", std::format("{}", (int) settings.fade) },
        { "vuSeeding", settings.vuSeeding ? "1" : "0" },
    };
    for (int band = 0; band < EQUALIZER_BANDS; band++) {
        properties[std::format("band{}", band)] = std::format("{}", settings.bandGains[band]);
    }

    if (!file::savePropertiesFile(path, properties)) {
        LOG_E(TAG, "Failed to save %s", path.c_str());
        return false;
    }
    return true;
}

} // namespace

void loadSettings() {
    Settings settings = DEFAULTS;

    Properties properties;
    const auto path = settingsFilePath();
    if (!path.empty() && file::loadPropertiesFile(path, properties)) {
        settings.presetIndex = readInt(properties, "preset", settings.presetIndex, 0, getPresetCount() - 1);
        for (int band = 0; band < EQUALIZER_BANDS; band++) {
            settings.bandGains[band] = readInt(properties, std::format("band{}", band), 0, -13, 13);
        }
        settings.gainDb = readInt(properties, "gainDb", settings.gainDb, -20, 20);
        settings.autoPreamp = readInt(properties, "autoPreamp", settings.autoPreamp, 0, 1) != 0;

        settings.dynamics = (Dynamics) readInt(properties, "dynamics", (int) settings.dynamics, 0, (int) Dynamics::Multiband);
        settings.speedPercent = readInt(properties, "speedPercent", settings.speedPercent, 50, 200);
        settings.pitchPercent = readInt(properties, "pitchPercent", settings.pitchPercent, 50, 200);
        settings.balancePercent = readInt(properties, "balancePercent", settings.balancePercent, -100, 100);
        settings.monoDownmix = readInt(properties, "monoDownmix", settings.monoDownmix, 0, 1) != 0;
        settings.fade = (Fade) readInt(properties, "fade", (int) settings.fade, 0, (int) Fade::Long);

        settings.vuSeeding = readInt(properties, "vuSeeding", settings.vuSeeding, 0, 1) != 0;
    }

    apply(settings);
}

void saveSettings() {
    write(read());
}

void resetSettings() {
    apply(DEFAULTS);
    write(DEFAULTS);
}

} // namespace tt::service::music
