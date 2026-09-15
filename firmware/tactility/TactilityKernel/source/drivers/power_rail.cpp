// SPDX-License-Identifier: Apache-2.0
#include <tactility/drivers/power_rail.h>
#include <tactility/device.h>

#define POWER_RAIL_DRIVER_API(driver) ((PowerRailApi*)driver->api)

static PowerRailEnableGuardFn enable_guard = nullptr;
static void* enable_guard_context = nullptr;

extern "C" {

void power_rail_set_enable_guard(PowerRailEnableGuardFn guard, void* context) {
    enable_guard = guard;
    enable_guard_context = context;
}

error_t power_rail_enable(Device* device) {
    if (enable_guard != nullptr && !enable_guard(device, enable_guard_context)) {
        return ERROR_INVALID_STATE;
    }
    const auto* driver = device_get_driver(device);
    return POWER_RAIL_DRIVER_API(driver)->enable(device);
}

error_t power_rail_disable(Device* device) {
    const auto* driver = device_get_driver(device);
    return POWER_RAIL_DRIVER_API(driver)->disable(device);
}

error_t power_rail_is_enabled(Device* device, bool* enabled) {
    const auto* driver = device_get_driver(device);
    return POWER_RAIL_DRIVER_API(driver)->is_enabled(device, enabled);
}

const DeviceType POWER_RAIL_TYPE {
    .name = "power_rail"
};

}
