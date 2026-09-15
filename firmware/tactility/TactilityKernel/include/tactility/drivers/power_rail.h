// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include <tactility/device.h>
#include <tactility/error.h>

/**
 * @brief A switchable power rail that consumers turn on while they need it.
 *
 * Consumers look a rail up by its devicetree node name. Reference counting, switching order and
 * settling times are the driver's concern; every enable must be matched by one disable.
 */
struct PowerRailApi {
    /**
     * @brief Turns the rail on, or keeps it on for one more consumer.
     * @param[in] device the rail device
     * @return ERROR_NONE if the rail is on when the call returns
     */
    error_t (*enable)(struct Device* device);

    /**
     * @brief Releases this consumer's hold on the rail, turning it off when it was the last one.
     * @param[in] device the rail device
     * @return ERROR_NONE if successful
     */
    error_t (*disable)(struct Device* device);

    /**
     * @brief Indicates whether the rail is currently on.
     * @param[in] device the rail device
     * @param[out] enabled receives the rail state
     * @return ERROR_NONE if successful
     */
    error_t (*is_enabled)(struct Device* device, bool* enabled);
};

/** @see PowerRailApi::enable */
error_t power_rail_enable(struct Device* device);

/** @see PowerRailApi::disable */
error_t power_rail_disable(struct Device* device);

/** @see PowerRailApi::is_enabled */
error_t power_rail_is_enabled(struct Device* device, bool* enabled);

/**
 * @brief Vetoes a rail coming up, before the driver is asked to switch anything.
 * @param[in] device the rail that is about to be enabled
 * @param[in] context the value passed to power_rail_set_enable_guard()
 * @return true when the rail may come up
 */
typedef bool (*PowerRailEnableGuardFn)(struct Device* device, void* context);

/**
 * @brief Installs the veto consulted by every power_rail_enable() call.
 *
 * The kernel has no power policy, so the layer that owns one installs it here. A driver raises its
 * own supply through the same call, so guarding an upstream rail covers everything it feeds.
 *
 * @param[in] guard the veto, or NULL to remove the current one
 * @param[in] context passed back to the veto unchanged
 */
void power_rail_set_enable_guard(PowerRailEnableGuardFn guard, void* context);

extern const struct DeviceType POWER_RAIL_TYPE;

#ifdef __cplusplus
}
#endif
