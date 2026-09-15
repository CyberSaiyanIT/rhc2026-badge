// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "adc.h"

#include <tactility/freertos/freertos.h>
#include <tactility/error.h>

struct Device;

/**
 * @brief API for ADC controller drivers.
 */
struct AdcControllerApi {
    /**
     * @brief Reads the raw conversion result of an ADC channel.
     * @param[in] device the ADC controller device
     * @param[in] channel the channel index to read
     * @param[out] out_raw the raw conversion result
     * @param[in] timeout the maximum time to wait for the operation to complete
     * @retval ERROR_NONE when the read operation was successful
     * @retval ERROR_OUT_OF_RANGE when the channel index is not configured
     * @retval ERROR_TIMEOUT when the operation timed out
     */
    error_t (*read_raw)(struct Device* device, uint8_t channel, int* out_raw, TickType_t timeout);

    /**
     * @brief Reads a channel as a voltage at the pin, corrected by the controller's calibration.
     *
     * Optional: NULL on a controller that cannot calibrate, whose callers scale read_raw()
     * themselves and so only approximate the per-chip reference and non-linear response.
     *
     * @param[in] device the ADC controller device
     * @param[in] channel the channel index to read
     * @param[out] out_millivolts the voltage at the pin
     * @param[in] timeout the maximum time to wait for the operation to complete
     * @retval ERROR_NOT_SUPPORTED when this controller has no calibration
     * @retval ERROR_NONE when the read operation was successful
     */
    error_t (*read_millivolts)(struct Device* device, uint8_t channel, int* out_millivolts, TickType_t timeout);
};

/**
 * @brief Reads the raw conversion result of an ADC channel using the specified controller.
 * @param[in] device the ADC controller device
 * @param[in] channel the channel index to read
 * @param[out] out_raw the raw conversion result
 * @param[in] timeout the maximum time to wait for the operation to complete
 * @retval ERROR_NONE when the read operation was successful
 */
error_t adc_controller_read_raw(struct Device* device, uint8_t channel, int* out_raw, TickType_t timeout);

/**
 * @brief Reads the raw conversion result of the ADC channel described by the given spec.
 * @param[in] spec the channel spec, as acquired from a devicetree phandle reference (e.g. `<&adc0 3>`)
 * @param[out] out_raw the raw conversion result
 * @param[in] timeout the maximum time to wait for the operation to complete
 * @retval ERROR_NONE when the read operation was successful
 */
error_t adc_channel_read_raw(const struct AdcChannelSpec* spec, int* out_raw, TickType_t timeout);

/**
 * @brief Reads a calibrated voltage from an ADC channel using the specified controller.
 * @retval ERROR_NOT_SUPPORTED when this controller has no calibration
 */
error_t adc_controller_read_millivolts(struct Device* device, uint8_t channel, int* out_millivolts, TickType_t timeout);

/**
 * @brief Reads a calibrated voltage from the ADC channel described by the given spec.
 * @retval ERROR_NOT_SUPPORTED when this controller has no calibration
 */
error_t adc_channel_read_millivolts(const struct AdcChannelSpec* spec, int* out_millivolts, TickType_t timeout);

extern const struct DeviceType ADC_CONTROLLER_TYPE;

#ifdef __cplusplus
}
#endif
