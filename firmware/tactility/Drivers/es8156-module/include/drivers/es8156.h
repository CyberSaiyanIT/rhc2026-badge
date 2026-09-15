// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <tactility/error.h>
#include <tactility/drivers/audio_codec.h>

struct Device;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ES8156 codec device configuration. Control is I2C, the bus being the device's parent,
 * while the I2S controller carrying audio is referenced by phandle.
 */
struct Es8156Config {
    /** I2C address on the bus (typically 0x18) */
    uint8_t address;
    /** I2S controller device that carries audio data */
    struct Device* i2s_device;
};

#ifdef __cplusplus
}
#endif
