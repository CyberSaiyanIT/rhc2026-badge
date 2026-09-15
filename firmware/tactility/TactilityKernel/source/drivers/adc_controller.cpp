// SPDX-License-Identifier: Apache-2.0
#include <tactility/drivers/adc_controller.h>
#include <tactility/device.h>

#define ADC_CONTROLLER_DRIVER_API(driver) ((struct AdcControllerApi*)driver->api)

extern "C" {

error_t adc_controller_read_raw(Device* device, uint8_t channel, int* out_raw, TickType_t timeout) {
    const auto* driver = device_get_driver(device);
    return ADC_CONTROLLER_DRIVER_API(driver)->read_raw(device, channel, out_raw, timeout);
}

error_t adc_channel_read_raw(const struct AdcChannelSpec* spec, int* out_raw, TickType_t timeout) {
    return adc_controller_read_raw(spec->adc_controller, spec->channel, out_raw, timeout);
}

error_t adc_controller_read_millivolts(Device* device, uint8_t channel, int* out_millivolts, TickType_t timeout) {
    const auto* driver = device_get_driver(device);
    const auto* api = ADC_CONTROLLER_DRIVER_API(driver);
    if (api->read_millivolts == nullptr) {
        return ERROR_NOT_SUPPORTED;
    }
    return api->read_millivolts(device, channel, out_millivolts, timeout);
}

error_t adc_channel_read_millivolts(const struct AdcChannelSpec* spec, int* out_millivolts, TickType_t timeout) {
    return adc_controller_read_millivolts(spec->adc_controller, spec->channel, out_millivolts, timeout);
}

const struct DeviceType ADC_CONTROLLER_TYPE {
    .name = "adc-controller"
};

}
