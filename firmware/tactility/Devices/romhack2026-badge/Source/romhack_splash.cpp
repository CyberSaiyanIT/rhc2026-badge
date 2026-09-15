// SPDX-License-Identifier: Apache-2.0
#include "romhack_splash_data.h"

#include <tactility/device.h>
#include <tactility/drivers/backlight.h>
#include <tactility/drivers/display.h>
#include <tactility/log.h>

#include <cstdlib>
#include <cstring>

#define TAG "RomhackSplash"

namespace {

// Drawn in bands so the background clear needs one small buffer instead of a full 150KB frame.
constexpr int CLEAR_BAND_HEIGHT = 40;

// Matches the LVGL splash in Boot.cpp, so handing over to it doesn't visibly shift anything.
constexpr int ROMHACK_Y = 70;
constexpr int CYBERSAIYAN_Y = 123;
constexpr int TRABODA_Y = 206;

void drawCentered(Device* display, const uint8_t* data, int width, int height, int y) {
    const int x = (ROMHACK_SPLASH_SCREEN_W - width) / 2;
    // DisplayApi wants an exclusive end coordinate (see lvgl-module's display.cpp flush).
    display_draw_bitmap(display, x, y, x + width, y + height, data);
}

void clearToBlack(Device* display) {
    const size_t band_bytes = (size_t) ROMHACK_SPLASH_SCREEN_W * CLEAR_BAND_HEIGHT * 2;
    auto* band = static_cast<uint8_t*>(malloc(band_bytes));
    if (band == nullptr) {
        return;
    }
    memset(band, 0, band_bytes);
    for (int y = 0; y < ROMHACK_SPLASH_SCREEN_H; y += CLEAR_BAND_HEIGHT) {
        int height = ROMHACK_SPLASH_SCREEN_H - y;
        if (height > CLEAR_BAND_HEIGHT) {
            height = CLEAR_BAND_HEIGHT;
        }
        display_draw_bitmap(display, 0, y, ROMHACK_SPLASH_SCREEN_W, y + height, band);
    }
    free(band);
}

} // namespace

/**
 * Paints the splash to the panel's GRAM before LVGL exists, then lights the backlight. Nothing is
 * mounted yet, so the artwork is linked in as RGB565 and the backlight uses the driver's default.
 */
extern "C" void romhack_draw_early_splash(void) {
    Device* display = nullptr;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display) != ERROR_NONE) {
        LOG_W(TAG, "No active display for early splash");
        return;
    }

    // The panel still holds whatever survived the last power cycle, so clear before lighting it.
    clearToBlack(display);
    drawCentered(display, ROMHACK_SPLASH_DATA, ROMHACK_SPLASH_W, ROMHACK_SPLASH_H, ROMHACK_Y);
    drawCentered(display, CYBERSAIYAN_SPLASH_DATA, CYBERSAIYAN_SPLASH_W, CYBERSAIYAN_SPLASH_H, CYBERSAIYAN_Y);
    drawCentered(display, TRABODA_SPLASH_DATA, TRABODA_SPLASH_W, TRABODA_SPLASH_H, TRABODA_Y);

    Device* backlight = nullptr;
    if (display_get_backlight(display, &backlight) == ERROR_NONE) {
        if (!device_is_ready(backlight) && device_start(backlight) != ERROR_NONE) {
            LOG_E(TAG, "Failed to start %s", backlight->name);
        }
        device_put(backlight);
    }

    device_put(display);
    LOG_I(TAG, "Early splash drawn");
}
