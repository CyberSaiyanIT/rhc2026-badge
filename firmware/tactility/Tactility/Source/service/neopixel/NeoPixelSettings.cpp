#include <Tactility/service/neopixel/NeoPixel.h>

#include <Tactility/file/File.h>
#include <Tactility/file/File.h>
#include <Tactility/file/PropertiesFile.h>

#include <app/paths.h>
#include <tactility/log.h>

#include <cstdlib>
#include <format>
#include <map>
#include <string>

namespace tt::service::neopixel {

namespace {

constexpr auto* TAG = "NeoPixelSettings";

/**
 * Bumped whenever a stored key changes meaning rather than merely being added. An older file is
 * ignored outright, since the sleep and standby prefixes would otherwise load onto wrong stages.
 */
constexpr int SETTINGS_VERSION = 2;

/**
 * Every value the Lighting app can change, and its starting value. Defaults live here rather than
 * in the service's State so first boot and "reset to defaults" cannot drift apart.
 */
struct Settings {
    Animation activeAnimation = Animation::Rainbow;
    // White, so a pattern that paints the stage colour reads as neutral until one is picked.
    uint8_t activeR = 255, activeG = 255, activeB = 255;
    ColorMode activeColorMode = ColorMode::Static;
    uint8_t activeBrightness = 40;
    uint8_t activeSpeed = 5;

    Animation standbyAnimation = Animation::Breathing;
    uint8_t standbyR = 255, standbyG = 255, standbyB = 255;
    ColorMode standbyColorMode = ColorMode::Static;
    // Dim and slow: this is what the badge does on a table, not what it does in a hand.
    uint8_t standbyBrightness = 40;
    uint8_t standbySpeed = 2;

    SleepAnimation sleepAnimation = SleepAnimation::Beacon;
    uint8_t sleepR = 255, sleepG = 255, sleepB = 255;
    ColorMode sleepColorMode = ColorMode::Static;
    uint8_t sleepBrightness = 40;
    uint8_t sleepIntervalSeconds = 3;
    uint8_t sleepMinutes = 5;

    // The same shape as waking, run backwards: it collapses and dims where waking grows and
    // brightens, so the pair reads as one motion undone.
    Transition wakeTransition = Transition::Bloom;
    Transition sleepTransition = Transition::Bloom;

    VuOrigin vuOrigin = VuOrigin::Center;
    VuPalette vuPalette = VuPalette::Classic;
    uint8_t vuBrightness = 40;
    uint8_t sleepSpeed = 5;
    uint8_t vuSpeed = 5;
    uint16_t vuInactiveSeconds = 1800;
    // Not black, so picking the Solid palette never reads as a dead strip.
    uint8_t vuR = 0, vuG = 255, vuB = 128;
    uint8_t vuSensitivity = 70;
    bool vuBassOnly = false;
    bool vuDecay = true;
    bool vuAutoGain = false;
    bool vuPeakHold = true;
    uint8_t vuPeakBrightness = 50;

    bool vuBeatFlash = true;
    bool vuBeatAuto = true;
    VuBeatSource vuBeatSource = VuBeatSource::Bass;
    uint8_t vuBeatSensitivity = 70;
    uint8_t vuFlashAttack = 0;
    uint8_t vuFlashDecay = 50;
};

constexpr Settings DEFAULTS {};

std::string settingsFilePath() {
    char path[256];
    if (app_paths_get_user_data_path("Lighting", "lighting.properties", path, sizeof(path)) != ERROR_NONE) {
        return {};
    }
    return path;
}

using Properties = std::map<std::string, std::string>;

int readInt(const Properties& properties, const char* key, int fallback, int min, int max) {
    const auto entry = properties.find(key);
    if (entry == properties.end()) {
        return fallback;
    }
    const long value = strtol(entry->second.c_str(), nullptr, 10);
    // A file edited by hand or written by an older build must not be able to index an enum out
    // of range, so everything is clamped rather than trusted.
    return (int) std::min<long>(std::max<long>(value, min), max);
}

Settings read() {
    Settings settings {};
    settings.activeAnimation = getActiveAnimation();
    getActiveColor(&settings.activeR, &settings.activeG, &settings.activeB);
    settings.activeBrightness = getActiveBrightness();
    settings.activeSpeed = getActiveSpeed();
    settings.activeColorMode = getActiveColorMode();

    settings.standbyAnimation = getStandbyAnimation();
    getStandbyColor(&settings.standbyR, &settings.standbyG, &settings.standbyB);
    settings.standbyBrightness = getStandbyBrightness();
    settings.standbySpeed = getStandbySpeed();
    settings.standbyColorMode = getStandbyColorMode();

    settings.sleepAnimation = getSleepAnimation();
    getSleepColor(&settings.sleepR, &settings.sleepG, &settings.sleepB);
    settings.sleepBrightness = getSleepBrightness();
    settings.sleepIntervalSeconds = getSleepIntervalSeconds();
    settings.sleepColorMode = getSleepColorMode();
    settings.sleepMinutes = getSleepMinutes();
    settings.wakeTransition = getWakeTransition();
    settings.sleepTransition = getSleepTransition();

    settings.vuOrigin = getVuOrigin();
    settings.vuPalette = getVuPalette();
    settings.vuBrightness = getVuBrightness();
    settings.sleepSpeed = getSleepSpeed();
    settings.vuSpeed = getVuSpeed();
    settings.vuInactiveSeconds = getVuInactiveSeconds();
    getVuColor(&settings.vuR, &settings.vuG, &settings.vuB);
    settings.vuSensitivity = getVuSensitivity();
    settings.vuBassOnly = isVuBassOnlyEnabled();
    settings.vuDecay = isVuDecayEnabled();
    settings.vuAutoGain = isVuAutoGainEnabled();
    settings.vuPeakHold = isVuPeakHoldEnabled();
    settings.vuPeakBrightness = getVuPeakBrightness();

    settings.vuBeatFlash = isVuBeatFlashEnabled();
    settings.vuBeatAuto = isVuBeatAutoEnabled();
    settings.vuBeatSource = getVuBeatSource();
    settings.vuBeatSensitivity = getVuBeatSensitivity();
    settings.vuFlashAttack = getVuFlashAttack();
    settings.vuFlashDecay = getVuFlashDecay();
    return settings;
}

void apply(const Settings& settings) {
    setActiveAnimation(settings.activeAnimation);
    setActiveColor(settings.activeR, settings.activeG, settings.activeB);
    setActiveBrightness(settings.activeBrightness);
    setActiveSpeed(settings.activeSpeed);
    setActiveColorMode(settings.activeColorMode);

    setStandbyAnimation(settings.standbyAnimation);
    setStandbyColor(settings.standbyR, settings.standbyG, settings.standbyB);
    setStandbyBrightness(settings.standbyBrightness);
    setStandbySpeed(settings.standbySpeed);
    setStandbyColorMode(settings.standbyColorMode);

    setSleepAnimation(settings.sleepAnimation);
    setSleepColor(settings.sleepR, settings.sleepG, settings.sleepB);
    setSleepBrightness(settings.sleepBrightness);
    setSleepIntervalSeconds(settings.sleepIntervalSeconds);
    setSleepColorMode(settings.sleepColorMode);
    setSleepMinutes(settings.sleepMinutes);
    setWakeTransition(settings.wakeTransition);
    setSleepTransition(settings.sleepTransition);

    setVuOrigin(settings.vuOrigin);
    setVuPalette(settings.vuPalette);
    setVuBrightness(settings.vuBrightness);
    setSleepSpeed(settings.sleepSpeed);
    setVuSpeed(settings.vuSpeed);
    setVuInactiveSeconds(settings.vuInactiveSeconds);
    setVuColor(settings.vuR, settings.vuG, settings.vuB);
    setVuSensitivity(settings.vuSensitivity);
    setVuBassOnlyEnabled(settings.vuBassOnly);
    setVuDecayEnabled(settings.vuDecay);
    setVuAutoGainEnabled(settings.vuAutoGain);
    setVuPeakHoldEnabled(settings.vuPeakHold);
    setVuPeakBrightness(settings.vuPeakBrightness);

    setVuBeatFlashEnabled(settings.vuBeatFlash);
    setVuBeatAutoEnabled(settings.vuBeatAuto);
    setVuBeatSource(settings.vuBeatSource);
    setVuBeatSensitivity(settings.vuBeatSensitivity);
    setVuFlashAttack(settings.vuFlashAttack);
    setVuFlashDecay(settings.vuFlashDecay);
}

bool write(const Settings& settings) {
    const auto path = settingsFilePath();
    if (path.empty()) {
        return false;
    }

    const Properties properties {
        { "version", std::format("{}", SETTINGS_VERSION) },
        { "activeAnimation", std::format("{}", (int) settings.activeAnimation) },
        { "activeRed", std::format("{}", settings.activeR) },
        { "activeGreen", std::format("{}", settings.activeG) },
        { "activeBlue", std::format("{}", settings.activeB) },
        { "activeBrightness", std::format("{}", settings.activeBrightness) },
        { "activeSpeed", std::format("{}", settings.activeSpeed) },
        { "activeColorMode", std::format("{}", (int) settings.activeColorMode) },
        { "standbyAnimation", std::format("{}", (int) settings.standbyAnimation) },
        { "standbyRed", std::format("{}", settings.standbyR) },
        { "standbyGreen", std::format("{}", settings.standbyG) },
        { "standbyBlue", std::format("{}", settings.standbyB) },
        { "standbyBrightness", std::format("{}", settings.standbyBrightness) },
        { "standbySpeed", std::format("{}", settings.standbySpeed) },
        { "standbyColorMode", std::format("{}", (int) settings.standbyColorMode) },
        { "sleepAnimation", std::format("{}", (int) settings.sleepAnimation) },
        { "sleepRed", std::format("{}", settings.sleepR) },
        { "sleepGreen", std::format("{}", settings.sleepG) },
        { "sleepBlue", std::format("{}", settings.sleepB) },
        { "sleepBrightness", std::format("{}", settings.sleepBrightness) },
        { "sleepIntervalSeconds", std::format("{}", settings.sleepIntervalSeconds) },
        { "sleepColorMode", std::format("{}", (int) settings.sleepColorMode) },
        { "sleepMinutes", std::format("{}", settings.sleepMinutes) },
        { "wakeTransition", std::format("{}", (int) settings.wakeTransition) },
        { "sleepTransition", std::format("{}", (int) settings.sleepTransition) },
        { "vuOrigin", std::format("{}", (int) settings.vuOrigin) },
        { "vuPalette", std::format("{}", (int) settings.vuPalette) },
        { "vuBrightness", std::format("{}", settings.vuBrightness) },
        { "sleepSpeed", std::format("{}", settings.sleepSpeed) },
        { "vuSpeed", std::format("{}", settings.vuSpeed) },
        { "vuInactiveSeconds", std::format("{}", settings.vuInactiveSeconds) },
        { "vuRed", std::format("{}", settings.vuR) },
        { "vuGreen", std::format("{}", settings.vuG) },
        { "vuBlue", std::format("{}", settings.vuB) },
        { "vuSensitivity", std::format("{}", settings.vuSensitivity) },
        { "vuBassOnly", settings.vuBassOnly ? "1" : "0" },
        { "vuDecay", settings.vuDecay ? "1" : "0" },
        { "vuAutoGain", settings.vuAutoGain ? "1" : "0" },
        { "vuPeakHold", settings.vuPeakHold ? "1" : "0" },
        { "vuPeakBrightness", std::format("{}", settings.vuPeakBrightness) },
        { "vuBeatFlash", settings.vuBeatFlash ? "1" : "0" },
        { "vuBeatAuto", settings.vuBeatAuto ? "1" : "0" },
        { "vuBeatSource", std::format("{}", (int) settings.vuBeatSource) },
        { "vuBeatSensitivity", std::format("{}", settings.vuBeatSensitivity) },
        { "vuFlashAttack", std::format("{}", settings.vuFlashAttack) },
        { "vuFlashDecay", std::format("{}", settings.vuFlashDecay) },
    };

    // The per-app data directory does not exist until something creates it, and savePropertiesFile
    // opens its temp file inside it rather than creating the path.
    if (!file::findOrCreateParentDirectory(path, 0755)) {
        LOG_E(TAG, "Failed to create the directory for %s", path.c_str());
        return false;
    }

    // Nothing has created the app's folder under the data partition yet, and a write into a
    // missing directory fails silently, losing every save.
    if (!file::findOrCreateParentDirectory(path, 0755)) {
        LOG_E(TAG, "Failed to create the directory for %s", path.c_str());
        return false;
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
    if (!path.empty() && file::loadPropertiesFile(path, properties) &&
        readInt(properties, "version", 0, 0, 99) == SETTINGS_VERSION) {
        constexpr int ANIMATION_MAX = (int) Animation::Count - 1;
        settings.activeAnimation = (Animation) readInt(properties, "activeAnimation", (int) settings.activeAnimation, 0, ANIMATION_MAX);
        settings.activeR = (uint8_t) readInt(properties, "activeRed", settings.activeR, 0, 255);
        settings.activeG = (uint8_t) readInt(properties, "activeGreen", settings.activeG, 0, 255);
        settings.activeB = (uint8_t) readInt(properties, "activeBlue", settings.activeB, 0, 255);
        settings.activeBrightness = (uint8_t) readInt(properties, "activeBrightness", settings.activeBrightness, 0, 100);
        settings.activeSpeed = (uint8_t) readInt(properties, "activeSpeed", settings.activeSpeed, 1, 20);
        constexpr int COLOR_MODE_MAX = (int) ColorMode::Cycle;
        settings.activeColorMode = (ColorMode) readInt(properties, "activeColorMode", (int) settings.activeColorMode, 0, COLOR_MODE_MAX);

        settings.standbyAnimation = (Animation) readInt(properties, "standbyAnimation", (int) settings.standbyAnimation, 0, ANIMATION_MAX);
        settings.standbyR = (uint8_t) readInt(properties, "standbyRed", settings.standbyR, 0, 255);
        settings.standbyG = (uint8_t) readInt(properties, "standbyGreen", settings.standbyG, 0, 255);
        settings.standbyB = (uint8_t) readInt(properties, "standbyBlue", settings.standbyB, 0, 255);
        settings.standbyBrightness = (uint8_t) readInt(properties, "standbyBrightness", settings.standbyBrightness, 0, 100);
        settings.standbySpeed = (uint8_t) readInt(properties, "standbySpeed", settings.standbySpeed, 1, 20);
        settings.standbyColorMode = (ColorMode) readInt(properties, "standbyColorMode", (int) settings.standbyColorMode, 0, COLOR_MODE_MAX);

        settings.sleepAnimation = (SleepAnimation) readInt(properties, "sleepAnimation", (int) settings.sleepAnimation, 0, (int) SleepAnimation::Count - 1);
        settings.sleepR = (uint8_t) readInt(properties, "sleepRed", settings.sleepR, 0, 255);
        settings.sleepG = (uint8_t) readInt(properties, "sleepGreen", settings.sleepG, 0, 255);
        settings.sleepB = (uint8_t) readInt(properties, "sleepBlue", settings.sleepB, 0, 255);
        settings.sleepBrightness = (uint8_t) readInt(properties, "sleepBrightness", settings.sleepBrightness, 0, 100);
        settings.sleepIntervalSeconds = (uint8_t) readInt(properties, "sleepIntervalSeconds", settings.sleepIntervalSeconds, 1, 60);
        settings.sleepColorMode = (ColorMode) readInt(properties, "sleepColorMode", (int) settings.sleepColorMode, 0, COLOR_MODE_MAX);
        settings.sleepMinutes = (uint8_t) readInt(properties, "sleepMinutes", settings.sleepMinutes, 0, 60);
        constexpr int TRANSITION_MAX = (int) Transition::Count - 1;
        settings.wakeTransition = (Transition) readInt(properties, "wakeTransition", (int) settings.wakeTransition, 0, TRANSITION_MAX);
        settings.sleepTransition = (Transition) readInt(properties, "sleepTransition", (int) settings.sleepTransition, 0, TRANSITION_MAX);

        settings.vuOrigin = (VuOrigin) readInt(properties, "vuOrigin", (int) settings.vuOrigin, 0, (int) VuOrigin::Cycle);
        settings.vuPalette = (VuPalette) readInt(properties, "vuPalette", (int) settings.vuPalette, 0, (int) VuPalette::Cycle);
        settings.vuBrightness = (uint8_t) readInt(properties, "vuBrightness", settings.vuBrightness, 0, 100);
        settings.sleepSpeed = (uint8_t) readInt(properties, "sleepSpeed", settings.sleepSpeed, 1, 20);
        settings.vuSpeed = (uint8_t) readInt(properties, "vuSpeed", settings.vuSpeed, 1, 20);
        settings.vuInactiveSeconds = (uint16_t) readInt(properties, "vuInactiveSeconds", settings.vuInactiveSeconds, 0, 7200);
        settings.vuR = (uint8_t) readInt(properties, "vuRed", settings.vuR, 0, 255);
        settings.vuG = (uint8_t) readInt(properties, "vuGreen", settings.vuG, 0, 255);
        settings.vuB = (uint8_t) readInt(properties, "vuBlue", settings.vuB, 0, 255);
        settings.vuSensitivity = (uint8_t) readInt(properties, "vuSensitivity", settings.vuSensitivity, 0, 100);
        settings.vuBassOnly = readInt(properties, "vuBassOnly", settings.vuBassOnly, 0, 1) != 0;
        settings.vuDecay = readInt(properties, "vuDecay", settings.vuDecay, 0, 1) != 0;
        settings.vuAutoGain = readInt(properties, "vuAutoGain", settings.vuAutoGain, 0, 1) != 0;
        settings.vuPeakHold = readInt(properties, "vuPeakHold", settings.vuPeakHold, 0, 1) != 0;
        settings.vuPeakBrightness = (uint8_t) readInt(properties, "vuPeakBrightness", settings.vuPeakBrightness, 0, 100);

        settings.vuBeatFlash = readInt(properties, "vuBeatFlash", settings.vuBeatFlash, 0, 1) != 0;
        settings.vuBeatAuto = readInt(properties, "vuBeatAuto", settings.vuBeatAuto, 0, 1) != 0;
        settings.vuBeatSource = (VuBeatSource) readInt(properties, "vuBeatSource", (int) settings.vuBeatSource, 0, (int) VuBeatSource::Treble);
        settings.vuBeatSensitivity = (uint8_t) readInt(properties, "vuBeatSensitivity", settings.vuBeatSensitivity, 0, 100);
        settings.vuFlashAttack = (uint8_t) readInt(properties, "vuFlashAttack", settings.vuFlashAttack, 0, 100);
        settings.vuFlashDecay = (uint8_t) readInt(properties, "vuFlashDecay", settings.vuFlashDecay, 0, 100);
    }

    apply(settings);
}

void saveSettings() {
    write(read());
}

void resetSettings() {
    apply(DEFAULTS);
    // Written from the constant rather than read back: apply() only queues the change, so a
    // read-back here would still see the values being replaced.
    write(DEFAULTS);
}

} // namespace tt::service::neopixel
