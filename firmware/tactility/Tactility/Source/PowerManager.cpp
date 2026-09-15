#include <Tactility/PowerManager.h>

#include <Tactility/file/File.h>
#include <Tactility/file/PropertiesFile.h>

#include <app/paths.h>

#include <tactility/device.h>
#include <tactility/drivers/power_rail.h>
#include <tactility/drivers/power_supply.h>
#include <tactility/log.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <string>

namespace tt::power {

namespace {

constexpr auto* TAG = "PowerManager";

/**
 * The rails the voltage limit applies to. The boost covers everything behind it; the reader sits
 * on its own switch and so is named separately.
 */
constexpr const char* GUARDED_RAIL_NAMES[] = { "boost5v", "rfid_power" };

bool isGuardedRail(const char* name) {
    if (name == nullptr) {
        return false;
    }
    for (const auto* guarded : GUARDED_RAIL_NAMES) {
        if (strcmp(name, guarded) == 0) {
            return true;
        }
    }
    return false;
}

constexpr auto* SETTINGS_KEY_OVERRIDE = "overrideLimits";

constexpr int SAMPLE_COUNT = 5;

std::atomic<bool> limitsOverridden = false;

/**
 * The settings filesystem is not mounted when the guard is installed, so they are read on first
 * use and retried until one succeeds. Until then the limits are enforced.
 */
std::atomic<bool> settingsLoaded = false;

std::string settingsFilePath() {
    char path[256];
    if (app_paths_get_user_data_path("tactility.power", "power.properties", path, sizeof(path)) != ERROR_NONE) {
        return {};
    }
    return path;
}

/**
 * @return a supply the caller must hand back with device_put(), or NULL
 *
 * The reference is taken inside the callback: device_for_each_of_type() only lends the pointer for
 * the duration of the call, and the samples below are read after it has returned.
 */
Device* findVoltageSupply() {
    Device* supply = nullptr;
    device_for_each_of_type(&POWER_SUPPLY_TYPE, &supply, [](Device* device, void* context) {
        if (device_is_ready(device) && power_supply_supports_property(device, POWER_SUPPLY_PROP_VOLTAGE)) {
            if (device_get(device) == ERROR_NONE) {
                *static_cast<Device**>(context) = device;
                return false;
            }
        }
        return true;
    });
    return supply;
}

void loadSettings() {
    if (settingsLoaded.load()) {
        return;
    }
    const auto path = settingsFilePath();
    std::map<std::string, std::string> properties;
    if (path.empty() || !file::loadPropertiesFile(path, properties)) {
        return;
    }
    const auto entry = properties.find(SETTINGS_KEY_OVERRIDE);
    if (entry != properties.end()) {
        limitsOverridden = entry->second == "1";
    }
    settingsLoaded = true;
}

bool guardRailEnable(Device* device, void* /*context*/) {
    loadSettings();
    if (limitsOverridden.load()) {
        return true;
    }
    if (!isGuardedRail(device->name)) {
        return true;
    }

    int millivolts = 0;
    if (!readBatteryMillivolts(millivolts)) {
        // No sensor means no policy to apply, rather than a board that can never use its boost.
        return true;
    }

    if (millivolts >= BOOST_MINIMUM_MV) {
        return true;
    }

    LOG_W(TAG, "Refusing %s at %d mV", device->name, millivolts);
    return false;
}

} // namespace

bool readBatteryMillivolts(int& outMillivolts) {
    Device* supply = findVoltageSupply();
    if (supply == nullptr) {
        return false;
    }

    int samples[SAMPLE_COUNT];
    int count = 0;
    // Kept only to name the rejected value in the log: with implausible readings hidden from the
    // UI, this line is the sole way to tell a sensor reading 300 mV from one reading 11000 mV.
    int rejected = 0;
    bool any_rejected = false;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        PowerSupplyPropertyValue value;
        if (power_supply_get_property(supply, POWER_SUPPLY_PROP_VOLTAGE, &value) != ERROR_NONE) {
            continue;
        }
        if (isPlausible(value.int_value)) {
            samples[count++] = value.int_value;
        } else {
            rejected = value.int_value;
            any_rejected = true;
        }
    }
    device_put(supply);

    if (count == 0) {
        if (any_rejected) {
            LOG_W(TAG, "Rejected battery reading of %d mV (expected %d-%d mV)", rejected, PLAUSIBLE_MIN_MV, PLAUSIBLE_MAX_MV);
        } else {
            LOG_W(TAG, "Battery supply returned no reading");
        }
        return false;
    }

    std::sort(samples, samples + count);
    outMillivolts = samples[count / 2];
    return true;
}

bool isLimitsOverridden() {
    loadSettings();
    return limitsOverridden.load();
}

void setLimitsOverridden(bool overridden) {
    limitsOverridden = overridden;
    settingsLoaded = true;

    const auto path = settingsFilePath();
    if (path.empty() || !file::findOrCreateParentDirectory(path, 0755)) {
        LOG_E(TAG, "Failed to save power settings");
        return;
    }
    std::map<std::string, std::string> properties;
    properties[SETTINGS_KEY_OVERRIDE] = overridden ? "1" : "0";
    file::savePropertiesFile(path, properties);
}

void init() {
    power_rail_set_enable_guard(guardRailEnable, nullptr);
    LOG_I(TAG, "Power rail guard installed");
}

} // namespace
