#pragma once

#include <tactility/drivers/gpio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RomhackPowerRailConfig {
    /** Output pin that switches this rail on when driven to its active level */
    struct GpioPinSpec pin;
    /** Rail that feeds this one, or NULL */
    struct Device* supply;
    /** Time to wait after switching on before the rail is considered usable */
    uint32_t startup_delay_us;
    /** Shortest the rail must stay off before it may be switched on again */
    uint32_t min_off_time_us;
};

extern struct Driver romhack_power_rail_driver;

#ifdef __cplusplus
}
#endif
