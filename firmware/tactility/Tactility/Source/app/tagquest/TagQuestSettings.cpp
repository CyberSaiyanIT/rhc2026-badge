#include "TagQuestSettings.h"

#include <Tactility/file/PropertiesFile.h>

#include <app/paths.h>

#include <tactility/log.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>

#ifdef ESP_PLATFORM
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#endif

namespace tt::app::tagquest {

constexpr auto* TAG = "TagQuestSettings";

namespace {

constexpr auto* APP_ID = "TagQuest";
constexpr auto* KEY_BADGE_ID = "badgeId";
constexpr auto* KEY_USERNAME = "username";

std::string settingsFilePath() {
    char path[256];
    if (app_paths_get_user_data_path(APP_ID, "tagquest.properties", path, sizeof(path)) != ERROR_NONE) {
        return "";
    }
    return path;
}

std::string toHex(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size * 2);
    char buffer[3];
    for (size_t i = 0; i < size; i++) {
        snprintf(buffer, sizeof(buffer), "%02x", data[i]);
        out += buffer;
    }
    return out;
}

/**
 * Derives a badge id from every hardware identity the chip has, hashed together.
 *
 * The three sources are laid out at fixed offsets and full width - 128 + 64 + 48 = 240 bits - so
 * one that cannot be read contributes its zero bytes rather than shortening the input. That keeps
 * the digest reproducible on a given badge whatever subset is available, which a variable-length
 * concatenation would not.
 *
 * Hashing rather than emitting the raw values gives one fixed 64-character id regardless of which
 * sources answered, and avoids putting the factory MAC on the wire verbatim.
 */
std::string deriveBadgeId() {
#ifdef ESP_PLATFORM
    uint8_t material[30] = {};
    bool have_unique_id = false;
    bool have_flash_id = false;
    bool have_mac = false;

    // Optional in the literal sense: un-burned parts read back as zeroes, which this treats the
    // same as absent.
    if (esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, material, 16 * 8) == ESP_OK) {
        have_unique_id = std::any_of(material, material + 16, [](uint8_t b) { return b != 0; });
    }

    uint64_t flash_id = 0;
    // Unsupported on flash parts without opcode 0x4B.
    if (esp_flash_read_unique_chip_id(nullptr, &flash_id) == ESP_OK && flash_id != 0) {
        for (size_t i = 0; i < 8; i++) {
            material[16 + i] = static_cast<uint8_t>(flash_id >> (56 - i * 8));
        }
        have_flash_id = true;
    }

    if (esp_efuse_mac_get_default(material + 24) == ESP_OK) {
        have_mac = std::any_of(material + 24, material + 30, [](uint8_t b) { return b != 0; });
    }

    // Every source absent would hash a block of zeroes into one id shared by every such badge,
    // which is worse than having none.
    if (!have_unique_id && !have_flash_id && !have_mac) {
        LOG_E(TAG, "No source of a unique badge id");
        return "";
    }

    uint8_t digest[32];
    mbedtls_sha256(material, sizeof(material), digest, 0);
    return toHex(digest, sizeof(digest));
#else
    // The simulator has no per-device identity to derive from, and a fixed value keeps the rest of
    // the app exercisable there.
    return "simulator";
#endif
}

} // namespace

bool isValidUsername(const std::string& username) {
    if (username.empty() || username.size() > USERNAME_MAX_LENGTH) {
        return false;
    }
    return std::all_of(username.begin(), username.end(), [](unsigned char c) {
        return std::isalnum(c) != 0;
    });
}

Settings loadOrCreate() {
    Settings settings;

    const auto path = settingsFilePath();
    if (!path.empty()) {
        std::map<std::string, std::string> properties;
        if (file::loadPropertiesFile(path, properties)) {
            settings.badgeId = properties[KEY_BADGE_ID];
            settings.username = properties[KEY_USERNAME];
        }
    }

    if (!isValidUsername(settings.username)) {
        settings.username.clear();
    }

    // Preferred over re-deriving even though the derivation is stable: a future change to it would
    // otherwise silently orphan every badge's leaderboard history.
    if (settings.badgeId.empty()) {
        settings.badgeId = deriveBadgeId();
        if (!settings.badgeId.empty()) {
            save(settings);
        }
    }

    return settings;
}

bool save(const Settings& settings) {
    const auto path = settingsFilePath();
    if (path.empty()) {
        return false;
    }

    std::map<std::string, std::string> properties;
    properties[KEY_BADGE_ID] = settings.badgeId;
    properties[KEY_USERNAME] = settings.username;

    return file::savePropertiesFile(path, properties);
}

} // namespace tt::app::tagquest
