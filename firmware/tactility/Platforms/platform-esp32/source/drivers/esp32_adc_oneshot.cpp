// SPDX-License-Identifier: Apache-2.0
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>

#include <new>

#include <tactility/error_esp32.h>
#include <tactility/driver.h>
#include <tactility/drivers/adc_controller.h>
#include <tactility/drivers/esp32_adc_oneshot.h>
#include <tactility/log.h>
#include <tactility/time.h>

#define TAG "esp32_adc_oneshot"

#define GET_CONFIG(device) ((Esp32AdcOneshotConfig*)device->config)
#define GET_INTERNAL(device) ((Esp32AdcOneshotInternal*)device_get_driver_data(device))
#define GET_HANDLE(device) (GET_INTERNAL(device)->handle)

/**
 * A raw count becomes a voltage only through a per-chip reference and a non-linear response, both
 * read from eFuse. One `cali` handle per channel, since the scheme binds to its attenuation and
 * bit width; NULL when this chip carries no data for that combination.
 */
struct Esp32AdcOneshotInternal {
    adc_oneshot_unit_handle_t handle;
    adc_cali_handle_t* cali;
    size_t channel_count;
};

static adc_cali_handle_t create_calibration(adc_unit_t unit, const Esp32AdcOneshotChannelConfig& channel_config) {
    adc_cali_handle_t handle = nullptr;
    esp_err_t esp_error;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t config = {
        .unit_id = unit,
        .chan = channel_config.channel,
        .atten = channel_config.atten,
        .bitwidth = channel_config.bitwidth,
    };
    esp_error = adc_cali_create_scheme_curve_fitting(&config, &handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t config = {
        .unit_id = unit,
        .atten = channel_config.atten,
        .bitwidth = channel_config.bitwidth,
    };
    esp_error = adc_cali_create_scheme_line_fitting(&config, &handle);
#else
    esp_error = ESP_ERR_NOT_SUPPORTED;
#endif
    if (esp_error != ESP_OK) {
        LOG_W(TAG, "No calibration for channel %d: %s", (int)channel_config.channel, esp_err_to_name(esp_error));
        return nullptr;
    }
    return handle;
}

static void destroy_calibration(adc_cali_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(handle);
#endif
}

static const Esp32AdcOneshotChannelConfig* find_channel_config(const Esp32AdcOneshotConfig* dts_config, uint8_t channel_index) {
    if (channel_index >= dts_config->channel_count) {
        return nullptr;
    }
    return &dts_config->channels[channel_index];
}

extern "C" {

static error_t read_raw(Device* device, uint8_t channel, int* out_raw, TickType_t timeout) {
    if (xPortInIsrContext()) return ERROR_ISR_STATUS;
    auto* dts_config = GET_CONFIG(device);
    auto* channel_config = find_channel_config(dts_config, channel);
    if (channel_config == nullptr) {
        return ERROR_OUT_OF_RANGE;
    }

    esp_err_t esp_error = adc_oneshot_read(GET_HANDLE(device), channel_config->channel, out_raw);
    if (esp_error != ESP_OK) {
        LOG_E(TAG, "read(channel=%u) failed: %s", channel, esp_err_to_name(esp_error));
    }
    return esp_err_to_error(esp_error);
}

static error_t read_millivolts(Device* device, uint8_t channel, int* out_millivolts, TickType_t timeout) {
    auto* internal = GET_INTERNAL(device);
    if (channel >= internal->channel_count || internal->cali[channel] == nullptr) {
        return ERROR_NOT_SUPPORTED;
    }

    int raw;
    error_t error = read_raw(device, channel, &raw, timeout);
    if (error != ERROR_NONE) {
        return error;
    }

    esp_err_t esp_error = adc_cali_raw_to_voltage(internal->cali[channel], raw, out_millivolts);
    if (esp_error != ESP_OK) {
        LOG_E(TAG, "cali(channel=%u) failed: %s", channel, esp_err_to_name(esp_error));
    }
    return esp_err_to_error(esp_error);
}

static error_t start(Device* device) {
    LOG_I(TAG, "start %s", device->name);
    auto* dts_config = GET_CONFIG(device);

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = dts_config->unit_id,
        .clk_src = dts_config->clk_src,
        .ulp_mode = dts_config->ulp_mode,
    };

    adc_oneshot_unit_handle_t handle;
    esp_err_t esp_error = adc_oneshot_new_unit(&init_config, &handle);
    if (esp_error != ESP_OK) {
        LOG_E(TAG, "Failed to create ADC unit %d: %s", (int)dts_config->unit_id, esp_err_to_name(esp_error));
        return ERROR_RESOURCE;
    }

    for (size_t i = 0; i < dts_config->channel_count; i++) {
        const auto& channel_config = dts_config->channels[i];
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = channel_config.atten,
            .bitwidth = channel_config.bitwidth,
        };
        esp_error = adc_oneshot_config_channel(handle, channel_config.channel, &chan_cfg);
        if (esp_error != ESP_OK) {
            LOG_E(TAG, "Failed to configure channel %d: %s", (int)channel_config.channel, esp_err_to_name(esp_error));
            adc_oneshot_del_unit(handle);
            return ERROR_RESOURCE;
        }
    }

    auto* internal = new(std::nothrow) Esp32AdcOneshotInternal { handle, nullptr, dts_config->channel_count };
    if (internal == nullptr) {
        adc_oneshot_del_unit(handle);
        return ERROR_OUT_OF_MEMORY;
    }
    internal->cali = new(std::nothrow) adc_cali_handle_t[dts_config->channel_count]();
    if (internal->cali == nullptr) {
        delete internal;
        adc_oneshot_del_unit(handle);
        return ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < dts_config->channel_count; i++) {
        internal->cali[i] = create_calibration(dts_config->unit_id, dts_config->channels[i]);
    }

    device_set_driver_data(device, internal);
    return ERROR_NONE;
}

static error_t stop(Device* device) {
    LOG_I(TAG, "stop %s", device->name);
    auto* internal = GET_INTERNAL(device);
    for (size_t i = 0; i < internal->channel_count; i++) {
        destroy_calibration(internal->cali[i]);
    }
    delete[] internal->cali;

    esp_err_t esp_error = adc_oneshot_del_unit(internal->handle);
    if (esp_error != ESP_OK) {
        LOG_E(TAG, "Deleting ADC unit failed: %s", esp_err_to_name(esp_error));
    }
    delete internal;
    device_set_driver_data(device, nullptr);
    return ERROR_NONE;
}

static constexpr AdcControllerApi ESP32_ADC_ONESHOT_API = {
    .read_raw = read_raw,
    .read_millivolts = read_millivolts
};

extern Module platform_esp32_module;

Driver esp32_adc_oneshot_driver = {
    .name = "esp32_adc_oneshot",
    .compatible = (const char*[]) { "espressif,esp32-adc-oneshot", nullptr },
    .start_device = start,
    .stop_device = stop,
    .api = &ESP32_ADC_ONESHOT_API,
    .device_type = &ADC_CONTROLLER_TYPE,
    .owner = &platform_esp32_module,
    .internal = nullptr
};

} // extern "C"
