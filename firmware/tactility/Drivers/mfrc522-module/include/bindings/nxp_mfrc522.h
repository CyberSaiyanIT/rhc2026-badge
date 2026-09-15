#pragma once
#include <tactility/bindings/bindings.h>

#include <tactility/drivers/gpio.h>

struct NxpMfrc522Config {
    struct GpioPinSpec pin_reset;
    /** Power rail that releases the reader from reset, or NULL when pin_reset is used instead */
    struct Device* supply;
};

DEFINE_DEVICETREE(mfrc522, struct NxpMfrc522Config)
