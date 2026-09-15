#pragma once
#ifdef ESP_PLATFORM

#include "Screensaver.h"

namespace tt::service::displayidle {

/** DVD-player style bounce of the RomHack logo. */
class RomHackLogoScreensaver final : public Screensaver {
public:
    RomHackLogoScreensaver() = default;
    ~RomHackLogoScreensaver() override = default;
    RomHackLogoScreensaver(const RomHackLogoScreensaver&) = delete;
    RomHackLogoScreensaver& operator=(const RomHackLogoScreensaver&) = delete;
    RomHackLogoScreensaver(RomHackLogoScreensaver&&) = delete;
    RomHackLogoScreensaver& operator=(RomHackLogoScreensaver&&) = delete;

    void start(lv_obj_t* overlay, lv_coord_t screenW, lv_coord_t screenH) override;
    void stop() override;
    void update(lv_coord_t screenW, lv_coord_t screenH) override;

private:
    static constexpr auto* LOGO_ASSET = "A:/system/romhack_logo.png";
    /**
     * On-screen size, kept at the asset's own pixels: upscaling softens the 1px outlines and the
     * small "2026" digits, and LVGL skips its transform path entirely at LV_SCALE_NONE.
     */
    static constexpr lv_coord_t LOGO_WIDTH = 210;
    static constexpr lv_coord_t LOGO_HEIGHT = 39;

    /**
     * Pixels per second, not per update: the driving tick skips a round whenever it cannot take
     * the LVGL lock, so a per-update step would crawl exactly when the system is busy.
     */
    static constexpr int32_t LOGO_SPEED = 45;


    void advance();

    lv_obj_t* logo_ = nullptr;
    /** Held for the resolution, so advance() needs no arguments of its own. */
    lv_display_t* display_ = nullptr;
    /** Position in thousandths of a pixel, so a fractional step per frame is not lost to rounding. */
    int32_t x_ = 0;
    int32_t y_ = 0;
    int32_t dx_ = LOGO_SPEED;
    int32_t dy_ = LOGO_SPEED;
    uint32_t lastUpdate_ = 0;
};

} // namespace tt::service::displayidle

#endif // ESP_PLATFORM
