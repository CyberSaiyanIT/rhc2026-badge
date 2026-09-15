#include <tactility/module.h>
#include <tactility/device.h>
#include <tactility/driver.h>

extern Driver romhack_keypad_driver;
extern Driver romhack_power_rail_driver;

extern "C" {

static error_t start() {
    return ERROR_NONE;
}

static error_t stop() {
    return ERROR_NONE;
}

static Driver* const romhack_drivers[] = {
    &romhack_keypad_driver,
    &romhack_power_rail_driver,
    nullptr
};

Module romhack2026_badge_module = {
    .name = "romhack",
    .start = start,
    .stop = stop,
    .drivers = romhack_drivers,
    .symbols = nullptr,
    .internal = nullptr
};

}
