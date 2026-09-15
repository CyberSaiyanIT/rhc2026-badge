#include <drivers/romhack_power_rail.h>

#include <tactility/concurrent/mutex.h>
#include <tactility/delay.h>
#include <tactility/device.h>
#include <tactility/driver.h>
#include <tactility/drivers/gpio_controller.h>
#include <tactility/drivers/power_rail.h>
#include <tactility/error.h>
#include <tactility/log.h>
#include <tactility/time.h>

static constexpr const char* TAG = "RomhackPowerRail";

#define GET_CONFIG(device) (static_cast<const RomhackPowerRailConfig*>((device)->config))

struct RomhackPowerRailInternal {
    GpioDescriptor* descriptor;
    Mutex mutex;
    /** Number of outstanding enable() calls; the switch is only driven on the 0<->1 transitions. */
    uint32_t consumers;
    /** When the rail was last switched off, for honouring min_off_time_us on the next enable. */
    uint64_t off_since_micros;
};

static error_t start(Device* device) {
    const auto* config = GET_CONFIG(device);

    auto* descriptor = gpio_descriptor_acquire(config->pin.gpio_controller, config->pin.pin, config->pin.flags | GPIO_FLAG_DIRECTION_OUTPUT, GPIO_OWNER_GPIO);
    if (descriptor == nullptr) {
        LOG_E(TAG, "Failed to acquire GPIO descriptor for %s", device->name);
        return ERROR_RESOURCE;
    }

    // Boot with the rail off; every load on it is optional. The pin briefly reads the AW9523B's
    // reset value of 0 between becoming an output and being driven, harmless while the boost is off.
    if (gpio_descriptor_set_level(descriptor, false) != ERROR_NONE) {
        LOG_E(TAG, "Failed to switch off %s", device->name);
        gpio_descriptor_release(descriptor);
        return ERROR_RESOURCE;
    }

    auto* internal = new RomhackPowerRailInternal { .descriptor = descriptor, .mutex = {}, .consumers = 0, .off_since_micros = get_micros_since_boot() };
    mutex_construct(&internal->mutex);
    device_set_driver_data(device, internal);
    return ERROR_NONE;
}

static error_t stop(Device* device) {
    auto* internal = static_cast<RomhackPowerRailInternal*>(device_get_driver_data(device));
    gpio_descriptor_set_level(internal->descriptor, false);
    gpio_descriptor_release(internal->descriptor);
    mutex_destruct(&internal->mutex);
    delete internal;
    return ERROR_NONE;
}

extern "C" {

static error_t rail_enable(Device* device) {
    auto* internal = static_cast<RomhackPowerRailInternal*>(device_get_driver_data(device));
    const auto* config = GET_CONFIG(device);

    if (config->supply != nullptr && !device_is_ready(config->supply)) {
        LOG_E(TAG, "Supply of %s is unavailable", device->name);
        return ERROR_RESOURCE;
    }

    mutex_lock(&internal->mutex);
    error_t result = ERROR_NONE;
    if (internal->consumers == 0) {
        // A load held in reset only sees one if the rail stayed low long enough; a user toggling a
        // feature off and on is otherwise fast enough to skip it.
        if (config->min_off_time_us > 0) {
            const uint64_t off_for = get_micros_since_boot() - internal->off_since_micros;
            if (off_for < config->min_off_time_us) {
                delay_micros((uint32_t) (config->min_off_time_us - off_for));
            }
        }
        if (config->supply != nullptr) {
            result = power_rail_enable(config->supply);
        }
        if (result == ERROR_NONE) {
            result = gpio_descriptor_set_level(internal->descriptor, true);
            if (result != ERROR_NONE && config->supply != nullptr) {
                power_rail_disable(config->supply);
            }
        }
        if (result == ERROR_NONE && config->startup_delay_us > 0) {
            delay_micros(config->startup_delay_us);
        }
    }
    if (result == ERROR_NONE) {
        internal->consumers++;
    } else {
        LOG_E(TAG, "Failed to switch on %s", device->name);
    }
    mutex_unlock(&internal->mutex);
    return result;
}

static error_t rail_disable(Device* device) {
    auto* internal = static_cast<RomhackPowerRailInternal*>(device_get_driver_data(device));
    const auto* config = GET_CONFIG(device);

    mutex_lock(&internal->mutex);
    if (internal->consumers == 0) {
        mutex_unlock(&internal->mutex);
        return ERROR_INVALID_STATE;
    }

    error_t result = ERROR_NONE;
    if (internal->consumers == 1) {
        result = gpio_descriptor_set_level(internal->descriptor, false);
        internal->off_since_micros = get_micros_since_boot();
        if (config->supply != nullptr) {
            error_t supply_result = power_rail_disable(config->supply);
            if (result == ERROR_NONE) {
                result = supply_result;
            }
        }
    }
    // Dropped even on a failed switch, or the rail stays on for the life of the board.
    internal->consumers--;
    mutex_unlock(&internal->mutex);
    return result;
}

static error_t rail_is_enabled(Device* device, bool* enabled) {
    auto* internal = static_cast<RomhackPowerRailInternal*>(device_get_driver_data(device));
    mutex_lock(&internal->mutex);
    *enabled = internal->consumers > 0;
    mutex_unlock(&internal->mutex);
    return ERROR_NONE;
}

static const PowerRailApi power_rail_api = {
    .enable = rail_enable,
    .disable = rail_disable,
    .is_enabled = rail_is_enabled,
};

extern Module romhack2026_badge_module;

Driver romhack_power_rail_driver = {
    .name = "romhack_power_rail",
    .compatible = (const char*[]) { "romhack,power-rail", nullptr },
    .start_device = start,
    .stop_device = stop,
    .api = &power_rail_api,
    .device_type = &POWER_RAIL_TYPE,
    .owner = &romhack2026_badge_module,
    .internal = nullptr
};

}
