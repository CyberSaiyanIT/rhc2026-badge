#ifdef ESP_PLATFORM

#include "RomHackLogoScreensaver.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace tt::service::displayidle {

namespace {

constexpr int32_t SUBPIXEL = 1000;

/** @return how far the logo may travel on an axis before its far edge leaves the screen */
lv_coord_t travel(lv_coord_t screen, lv_coord_t logo) {
    return screen > logo ? screen - logo : 0;
}

/**
 * Bounces @a value back inside 0 to @a max, turning @a direction round. Reflects rather than
 * clamps, or the bounce loses distance every time a frame runs late and the step overshoots.
 */
int32_t reflect(int32_t value, int32_t max, int32_t* direction) {
    if (max <= 0) {
        return 0;
    }
    if (value < 0) {
        *direction = -*direction;
        return std::min<int32_t>(-value, max);
    }
    if (value > max) {
        *direction = -*direction;
        return std::max<int32_t>(2 * max - value, 0);
    }
    return value;
}

} // namespace

void RomHackLogoScreensaver::start(lv_obj_t* overlay, lv_coord_t screenW, lv_coord_t screenH) {
    logo_ = lv_image_create(overlay);
    if (logo_ == nullptr) {
        return;
    }
    lv_image_set_src(logo_, LOGO_ASSET);
    lv_obj_set_size(logo_, LOGO_WIDTH, LOGO_HEIGHT);
    // Stretch rather than a scale: it pins the pivot top-left so what is drawn is exactly the
    // object's box. An image otherwise centres its source first and lands half its width off.
    lv_image_set_inner_align(logo_, LV_IMAGE_ALIGN_STRETCH);

    const lv_coord_t maxX = travel(screenW, LOGO_WIDTH);
    const lv_coord_t maxY = travel(screenH, LOGO_HEIGHT);
    x_ = (maxX > 0 ? rand() % maxX : 0) * SUBPIXEL;
    y_ = (maxY > 0 ? rand() % maxY : 0) * SUBPIXEL;

    // Equal speed on both axes, so the logo travels at 45 degrees and reflects predictably off
    // the edges the way the original DVD bounce does.
    dx_ = (rand() % 2) ? LOGO_SPEED : -LOGO_SPEED;
    dy_ = (rand() % 2) ? LOGO_SPEED : -LOGO_SPEED;

    lastUpdate_ = lv_tick_get();
    lv_obj_set_pos(logo_, x_ / SUBPIXEL, y_ / SUBPIXEL);

    // Held only for the resolution. Not hooked to LV_EVENT_REFR_START: this animation is the only
    // thing invalidating the screen, so the first cycle that moves nothing would end the bounce.
    display_ = lv_display_get_default();
}

void RomHackLogoScreensaver::stop() {
    display_ = nullptr;
    logo_ = nullptr; // Deleted by the parent overlay
}

void RomHackLogoScreensaver::update(lv_coord_t, lv_coord_t) {
    advance();
}

void RomHackLogoScreensaver::advance() {
    if (logo_ == nullptr) {
        return;
    }

    const lv_coord_t screenW = lv_display_get_horizontal_resolution(display_);
    const lv_coord_t screenH = lv_display_get_vertical_resolution(display_);

    const uint32_t now = lv_tick_get();
    // Never capped. The driving tick is a low-priority callback on the shared timer daemon and
    // runs late whenever the system is busy; a cap would turn every late frame into a short one.
    const uint32_t elapsed = lv_tick_elaps(lastUpdate_);
    lastUpdate_ = now;
    if (elapsed == 0) {
        return;
    }

    const int32_t maxX = travel(screenW, LOGO_WIDTH) * SUBPIXEL;
    const int32_t maxY = travel(screenH, LOGO_HEIGHT) * SUBPIXEL;

    x_ = reflect(x_ + dx_ * (int32_t) elapsed, maxX, &dx_);
    y_ = reflect(y_ + dy_ * (int32_t) elapsed, maxY, &dy_);

    lv_obj_set_pos(logo_, x_ / SUBPIXEL, y_ / SUBPIXEL);

}

} // namespace tt::service::displayidle

#endif // ESP_PLATFORM
