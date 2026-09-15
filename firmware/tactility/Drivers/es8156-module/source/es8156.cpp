// SPDX-License-Identifier: Apache-2.0
#include <drivers/es8156.h>

#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_codec_adapters.h>
#include <tactility/drivers/i2c_controller.h>
#include <tactility/drivers/i2s_controller.h>
#include <tactility/delay.h>
#include <tactility/log.h>

#include <es8156_dac.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

#include <cmath>

#define TAG "ES8156"

namespace {

// Volume curve maps 1..100 to roughly -49.5dB..0dB; native ES8156 range is wide enough
// that we expose a flat 0..100 percent scale to callers and let esp_codec_dev convert.
constexpr uint32_t NATIVE_SAMPLE_RATE = 44100;

constexpr uint8_t REG_RESET = 0x00;
/**
 * REG_RESET's power-on value, holding the digital core, DAC and master clock in reset. Not
 * released here: esp_codec_dev's setup ends by writing this register outright.
 */
constexpr uint8_t RESET_ASSERTED = 0x1c;

/**
 * Puts the codec back where a cold boot would leave it. Its registers survive a CPU reset, and
 * esp_codec_dev's setup rewrites the clock path under an already-running core: a pop, then silence.
 */
bool reset_codec(const audio_codec_ctrl_if_t* ctrl_if) {
    uint8_t value = RESET_ASSERTED;
    if (ctrl_if->write_reg(ctrl_if, REG_RESET, 1, &value, 1) != ESP_CODEC_DEV_OK) {
        return false;
    }
    // Long enough for the state machine to have settled before it is brought back up.
    delay_millis(10);
    return true;
}

struct Es8156Data {
    const audio_codec_ctrl_if_t* ctrl_if = nullptr;
    const audio_codec_data_if_t* data_if = nullptr;
    const audio_codec_gpio_if_t* gpio_if = nullptr;
    const audio_codec_if_t* codec_if = nullptr;
    esp_codec_dev_handle_t codec_device = nullptr;
    bool is_open = false;
    AudioCodecDirection open_direction = AUDIO_CODEC_DIR_OUTPUT;
    esp_codec_dev_sample_info_t open_sample_info = {};
};

#define GET_CONFIG(device) (static_cast<const Es8156Config*>((device)->config))
#define GET_DATA(device) (static_cast<Es8156Data*>(device_get_driver_data(device)))

error_t open(Device* device, const struct AudioCodecStreamConfig* config) {
    auto* data = GET_DATA(device);
    if (data->codec_device == nullptr) {
        return ERROR_RESOURCE;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = config->bits_per_sample,
        .channel = config->channels,
        .channel_mask = 0,
        .sample_rate = config->sample_rate,
        .mclk_multiple = 0,
    };

    if (data->is_open) {
        // BOTH already serves INPUT-only or OUTPUT-only requests at the same sample settings;
        // only an exact direction mismatch needs a reopen.
        bool direction_compatible = data->open_direction == config->direction
            || data->open_direction == AUDIO_CODEC_DIR_OUTPUT;
        bool same_config = direction_compatible
            && data->open_sample_info.bits_per_sample == sample_info.bits_per_sample
            && data->open_sample_info.channel == sample_info.channel
            && data->open_sample_info.sample_rate == sample_info.sample_rate;
        return same_config ? ERROR_NONE : ERROR_RESOURCE;
    }

    if (esp_codec_dev_open(data->codec_device, &sample_info) != ESP_CODEC_DEV_OK) {
        LOG_E(TAG, "Failed to open codec device");
        return ERROR_RESOURCE;
    }

    // esp_codec_dev requires an explicit enable call to start the DAC/ADC engines
    // Note: esp_codec_dev_enable has been removed in recent esp_codec_dev versions, open() suffices

    data->is_open = true;
    data->open_direction = config->direction;
    data->open_sample_info = sample_info;
    return ERROR_NONE;
}

error_t close(Device* device) {
    auto* data = GET_DATA(device);
    if (data->codec_device == nullptr) {
        return ERROR_RESOURCE;
    }

    if (data->is_open) {
        esp_codec_dev_close(data->codec_device);
        data->is_open = false;
    }

    return ERROR_NONE;
}

error_t read(Device* device, void* buffer, size_t size, size_t* bytes_read, TickType_t timeout) {
    (void) timeout;
    auto* data = GET_DATA(device);
    if (!data->is_open) {
        return ERROR_RESOURCE;
    }

    // esp_codec_dev_read returns a byte count on success and a negative ESP_CODEC_DEV_* code on
    // failure. It does not return ESP_CODEC_DEV_OK for a successful nonzero-length read.
    int result = esp_codec_dev_read(data->codec_device, buffer, (int) size);
    if (result < 0) {
        return ERROR_RESOURCE;
    }

    *bytes_read = (size_t) result;
    return ERROR_NONE;
}

error_t write(Device* device, const void* buffer, size_t size, size_t* bytes_written, TickType_t timeout) {
    (void) timeout;
    auto* data = GET_DATA(device);
    if (!data->is_open) {
        return ERROR_RESOURCE;
    }

    // esp_codec_dev_write returns a byte count on success and a negative ESP_CODEC_DEV_* code on
    // failure. It does not return ESP_CODEC_DEV_OK for a successful nonzero-length write.
    int result = esp_codec_dev_write(data->codec_device, const_cast<void*>(buffer), (int) size);
    if (result < 0) {
        return ERROR_RESOURCE;
    }

    *bytes_written = (size_t) result;
    return ERROR_NONE;
}

error_t set_volume(Device* device, AudioCodecDirection direction, float volume_percent) {
    auto* data = GET_DATA(device);
    if (data->codec_device == nullptr) {
        return ERROR_RESOURCE;
    }

    if (volume_percent < 0.0f || volume_percent > 100.0f) {
        return ERROR_RESOURCE;
    }

    if (direction == AUDIO_CODEC_DIR_OUTPUT) {
        int volume = (int) std::lround(volume_percent);
        return (esp_codec_dev_set_out_vol(data->codec_device, volume) == ESP_CODEC_DEV_OK) ? ERROR_NONE : ERROR_RESOURCE;
    }

    if (direction == AUDIO_CODEC_DIR_INPUT) {
        // ES8156 ADC gain range is roughly 0..24 dB; map 0..100% linearly onto it.
        float db = (volume_percent / 100.0f) * 24.0f;
        return (esp_codec_dev_set_in_gain(data->codec_device, db) == ESP_CODEC_DEV_OK) ? ERROR_NONE : ERROR_RESOURCE;
    }

    return ERROR_NOT_SUPPORTED;
}

error_t get_volume(Device* device, AudioCodecDirection direction, float* volume_percent) {
    auto* data = GET_DATA(device);
    if (data->codec_device == nullptr) {
        return ERROR_RESOURCE;
    }

    if (direction == AUDIO_CODEC_DIR_OUTPUT) {
        int volume = 0;
        if (esp_codec_dev_get_out_vol(data->codec_device, &volume) != ESP_CODEC_DEV_OK) {
            return ERROR_RESOURCE;
        }
        *volume_percent = (float) volume;
        return ERROR_NONE;
    }

    if (direction == AUDIO_CODEC_DIR_INPUT) {
        float db = 0.0f;
        if (esp_codec_dev_get_in_gain(data->codec_device, &db) != ESP_CODEC_DEV_OK) {
            return ERROR_RESOURCE;
        }
        *volume_percent = (db / 24.0f) * 100.0f;
        return ERROR_NONE;
    }

    return ERROR_NOT_SUPPORTED;
}

error_t set_mute(Device* device, AudioCodecDirection direction, bool muted) {
    auto* data = GET_DATA(device);
    if (data->codec_device == nullptr) {
        return ERROR_RESOURCE;
    }

    if (direction == AUDIO_CODEC_DIR_OUTPUT) {
        return (esp_codec_dev_set_out_mute(data->codec_device, muted) == ESP_CODEC_DEV_OK) ? ERROR_NONE : ERROR_RESOURCE;
    }

    if (direction == AUDIO_CODEC_DIR_INPUT) {
        return (esp_codec_dev_set_in_mute(data->codec_device, muted) == ESP_CODEC_DEV_OK) ? ERROR_NONE : ERROR_RESOURCE;
    }

    return ERROR_NOT_SUPPORTED;
}

error_t get_mute(Device* device, AudioCodecDirection direction, bool* muted) {
    auto* data = GET_DATA(device);
    if (data->codec_device == nullptr) {
        return ERROR_RESOURCE;
    }

    if (direction == AUDIO_CODEC_DIR_OUTPUT) {
        return (esp_codec_dev_get_out_mute(data->codec_device, muted) == ESP_CODEC_DEV_OK) ? ERROR_NONE : ERROR_RESOURCE;
    }

    if (direction == AUDIO_CODEC_DIR_INPUT) {
        return (esp_codec_dev_get_in_mute(data->codec_device, muted) == ESP_CODEC_DEV_OK) ? ERROR_NONE : ERROR_RESOURCE;
    }

    return ERROR_NOT_SUPPORTED;
}

error_t get_native_channels(Device* device, AudioCodecDirection direction, uint8_t* channels) {
    (void) device;
    if (direction != AUDIO_CODEC_DIR_OUTPUT) {
        return ERROR_NOT_SUPPORTED;
    }
    *channels = 2;
    return ERROR_NONE;
}

error_t get_native_sample_rate(Device* device, AudioCodecDirection direction, uint32_t* rate_hz) {
    (void) device;
    (void) direction;
    *rate_hz = NATIVE_SAMPLE_RATE;
    return ERROR_NONE;
}

error_t get_capabilities(Device* device, AudioCodecDirection* supported_directions) {
    (void) device;
    *supported_directions = AUDIO_CODEC_DIR_OUTPUT;
    return ERROR_NONE;
}

static const struct AudioCodecApi API = {
    .open = open,
    .close = close,
    .read = read,
    .write = write,
    .set_volume = set_volume,
    .get_volume = get_volume,
    .set_mute = set_mute,
    .get_mute = get_mute,
    .get_native_sample_rate = get_native_sample_rate,
    .get_native_channels = get_native_channels,
    .get_capabilities = get_capabilities,
    .get_input_gain_multiplier = nullptr,
};

error_t start_device(Device* device) {
    const auto* config = GET_CONFIG(device);

    auto* i2c_controller = device_get_parent(device);
    if (i2c_controller == nullptr || device_get_type(i2c_controller) != &I2C_CONTROLLER_TYPE) {
        LOG_E(TAG, "Parent is not an I2C controller");
        return ERROR_RESOURCE;
    }

    auto* i2s_controller = config->i2s_device;
    if (i2s_controller == nullptr || device_get_type(i2s_controller) != &I2S_CONTROLLER_TYPE) {
        LOG_E(TAG, "I2S controller device is not valid");
        return ERROR_RESOURCE;
    }

    auto* data = new Es8156Data();

    data->ctrl_if = audio_codec_adapter_new_i2c_ctrl(i2c_controller, config->address);
    data->data_if = audio_codec_adapter_new_i2s_data(i2s_controller);
    if (data->ctrl_if == nullptr || data->data_if == nullptr) {
        LOG_E(TAG, "Failed to create adapters");
        goto cleanup;
    }

    if (data->ctrl_if->open(data->ctrl_if, nullptr, 0) != ESP_CODEC_DEV_OK) {
        LOG_E(TAG, "Failed to open control interface");
        goto cleanup;
    }

    if (data->data_if->open(data->data_if, nullptr, 0) != ESP_CODEC_DEV_OK) {
        LOG_E(TAG, "Failed to open data interface");
        goto cleanup;
    }

    // Before es8156_codec_new(), which is what runs the setup sequence this precondition is for.
    if (!reset_codec(data->ctrl_if)) {
        LOG_E(TAG, "Failed to reset the codec");
        goto cleanup;
    }

    {
        es8156_codec_cfg_t codec_config = {};
        codec_config.ctrl_if = data->ctrl_if;
        codec_config.gpio_if = nullptr;
                codec_config.pa_pin = -1;
        codec_config.pa_reverted = false;
                                                        
        data->codec_if = es8156_codec_new(&codec_config);
        if (data->codec_if == nullptr) {
            LOG_E(TAG, "Failed to create ES8156 codec interface");
            goto cleanup;
        }
    }

    {
        esp_codec_dev_cfg_t dev_config = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = data->codec_if,
            .data_if = data->data_if,
        };

        data->codec_device = esp_codec_dev_new(&dev_config);
        if (data->codec_device == nullptr) {
            LOG_E(TAG, "Failed to create codec device");
            goto cleanup;
        }
    }

    device_set_driver_data(device, data);
    return ERROR_NONE;

cleanup:
    // Mirrors stop_device's teardown order -- delete_*_if() routines close their interface
    // first, so we don't need separate ->close() calls here.
    if (data->codec_device != nullptr) {
        esp_codec_dev_delete(data->codec_device);
    }
    if (data->codec_if != nullptr) {
        audio_codec_delete_codec_if(data->codec_if);
    }
    if (data->gpio_if != nullptr) {
        audio_codec_adapter_delete_gpio(data->gpio_if);
    }
    if (data->data_if != nullptr) {
        audio_codec_delete_data_if(data->data_if);
    }
    if (data->ctrl_if != nullptr) {
        audio_codec_delete_ctrl_if(data->ctrl_if);
    }
    delete data;
    return ERROR_RESOURCE;
}

error_t stop_device(Device* device) {
    auto* data = GET_DATA(device);
    if (data == nullptr) {
        return ERROR_NONE;
    }

    if (data->is_open) {
        esp_codec_dev_close(data->codec_device);
    }

    if (data->codec_device != nullptr) {
        esp_codec_dev_delete(data->codec_device);
    }
    if (data->codec_if != nullptr) {
        audio_codec_delete_codec_if(data->codec_if);
    }
    if (data->gpio_if != nullptr) {
        audio_codec_adapter_delete_gpio(data->gpio_if);
    }
    if (data->data_if != nullptr) {
        audio_codec_delete_data_if(data->data_if);
    }
    if (data->ctrl_if != nullptr) {
        audio_codec_delete_ctrl_if(data->ctrl_if);
    }

    device_set_driver_data(device, nullptr);
    delete data;
    return ERROR_NONE;
}

} // namespace

extern "C" {

Driver es8156_driver = {
    .name = "es8156",
    .compatible = (const char*[]) { "everest,es8156", nullptr },
    .start_device = start_device,
    .stop_device = stop_device,
    .api = &API,
    .device_type = &AUDIO_CODEC_TYPE,
    .owner = nullptr,
    .internal = nullptr,
};

}
