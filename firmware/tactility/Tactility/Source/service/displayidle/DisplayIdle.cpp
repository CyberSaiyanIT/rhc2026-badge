#ifdef ESP_PLATFORM

#include <Tactility/service/displayidle/DisplayIdleService.h>
#include <Tactility/service/ServiceManifest.h>
#include <Tactility/service/neopixel/NeoPixel.h>
#include <Tactility/service/ServiceRegistration.h>
#include <Tactility/service/music/Music.h>
#include <Tactility/network/Http.h>

#include <app/manager.h>

#include <cstring>

#include "BouncingBallsScreensaver.h"
#include "MatrixRainScreensaver.h"
#include "MystifyScreensaver.h"
#include "NowPlayingScreensaver.h"
#include "RomHackLogoScreensaver.h"
#include "Screensaver.h"
#include "StackChanScreensaver.h"

#include <cstdlib>
#include <ctime>
#include <vector>

#include <tactility/delay.h>
#include <tactility/log.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/backlight.h>

#include <lvgl/lvgl.h>

namespace tt::service::displayidle {

constexpr auto* TAG = "DisplayIdle";


void DisplayIdleService::animationTimerCb(lv_timer_t* timer) {
    // Already on the LVGL task with its lock held, so the screensaver may touch LVGL freely.
    static_cast<DisplayIdleService*>(lv_timer_get_user_data(timer))->updateScreensaver();
}

void DisplayIdleService::stopAnimationTimer() {
    if (animationTimer != nullptr) {
        lv_timer_delete(animationTimer);
        animationTimer = nullptr;
    }
}

namespace {

/**
 * Work that holds the screen without touching the keypad, which the idle timer cannot tell from an
 * abandoned badge. Checked by id, so an app that crashes leaves no latched inhibit behind.
 */
bool isScreensaverInhibited() {
    if (network::http::isDownloading()) {
        return true;
    }

    char app_id[64];
    if (app_manager_get_topmost_app_id(app_id, sizeof(app_id)) != ERROR_NONE) {
        return false;
    }
    return std::strcmp(app_id, "tactility.pingpong") == 0;
}

/** Paused counts: the user is still on a track, just not hearing it right now. */
bool isMusicActive() {
    if (!service::music::isAvailable()) {
        return false;
    }
    using enum service::music::State;
    const auto state = service::music::getTelemetry().state;
    return state == Playing || state == Buffering || state == Paused;
}

/**
 * The children hidden by setActiveScreenHidden(), so the restore only unhides what it hid and an
 * app's own hidden widgets stay hidden.
 */
std::vector<lv_obj_t*> hiddenChildren;

/**
 * The overlay does not stop what is under it from invalidating, and LVGL drops invalidations from
 * a hidden subtree, so hiding the screen's children removes that work.
 *
 * The children rather than the screen itself: lv_obj_remove_flag() marks the parent's layout
 * dirty unguarded (lv_obj.c:288) and a screen has no parent, so clearing the flag there crashes.
 */
void setActiveScreenHidden(bool hidden) {
    if (hidden) {
        auto* screen = lv_screen_active();
        if (screen == nullptr) {
            return;
        }
        hiddenChildren.clear();
        const uint32_t count = lv_obj_get_child_count(screen);
        for (uint32_t i = 0; i < count; i++) {
            auto* child = lv_obj_get_child(screen, i);
            if (child == nullptr || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            hiddenChildren.push_back(child);
        }
        return;
    }

    // The owning app may have deleted any of these in the meantime, hence the validity check.
    for (lv_obj_t* child : hiddenChildren) {
        if (lv_obj_is_valid(child)) {
            lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
    hiddenChildren.clear();
}

} // namespace

void DisplayIdleService::stopScreensaverCb(lv_event_t* e) {
    auto* self = static_cast<DisplayIdleService*>(lv_event_get_user_data(e));
    lv_event_stop_bubbling(e);
    self->stopScreensaverRequested.store(true, std::memory_order_release);
    lv_display_trigger_activity(nullptr);
}

static void setBacklightBrightness(uint8_t brightness) {
    ::Device* display;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display) == ERROR_NONE) {
        ::Device* backlight;
        if (display_get_backlight(display, &backlight) == ERROR_NONE) {
            backlight_set_brightness(backlight, brightness);
            device_put(backlight);
        }
        device_put(display);
    }
}

static bool hasDisplayWithBacklight() {
    ::Device* display;
    bool result = false;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display) == ERROR_NONE) {
        ::Device* backlight;
        if (display_get_backlight(display, &backlight) == ERROR_NONE) {
            result = true;
            device_put(backlight);
        }
        device_put(display);
    }
    return result;
}

void DisplayIdleService::stopScreensaver() {
    if (!lvgl_try_lock(100)) {
        // Lock failed - keep flag set to retry on next tick
        return;
    }

    const auto restoreDuty = cachedDisplaySettings.backlightDuty;
    const bool wasDimmed = displayDimmed;

    stopAnimationTimer();
    if (screensaverOverlay) {
        if (screensaver) {
            screensaver->stop();
            screensaver.reset();
        }
        lv_obj_delete(screensaverOverlay);
        screensaverOverlay = nullptr;
    }
    setActiveScreenHidden(false);
    lvgl_unlock();
    stopScreensaverRequested.store(false, std::memory_order_relaxed);

    // The strip steps down on the same timeout as the screen, so there is no second one to keep in
    // step with this. It ignores a repeat of what it is already doing.
    neopixel::setIdle(false);

    // Reset auto-off state
    screensaverActiveCounter = 0;
    backlightOff = false;

    // Restore backlight if display was dimmed
    if (wasDimmed) {
        setBacklightBrightness(restoreDuty);
    }

    displayDimmed = wasDimmed ? false : displayDimmed;
}

void DisplayIdleService::activateScreensaver() {
    lv_obj_t* top = lv_layer_top();

    if (screensaverOverlay != nullptr) return;

    neopixel::setIdle(true);

    // Reset auto-off counter when starting screensaver
    screensaverActiveCounter = 0;
    backlightOff = false;

    lv_coord_t screenW = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenH = lv_display_get_vertical_resolution(nullptr);

    // Black background overlay
    screensaverOverlay = lv_obj_create(top);
    lv_obj_remove_style_all(screensaverOverlay);
    lv_obj_set_size(screensaverOverlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(screensaverOverlay, 0, 0);
    lv_obj_set_style_bg_color(screensaverOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screensaverOverlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(screensaverOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screensaverOverlay, stopScreensaverCb, LV_EVENT_CLICKED, this);

    setActiveScreenHidden(true);

    // Animating while audio runs costs the pipeline CPU and SPI bandwidth it cannot spare, so
    // playback overrides the configured screensaver rather than being a setting of its own.
    if (isMusicActive()) {
        screensaver = std::make_unique<NowPlayingScreensaver>();
        screensaver->start(screensaverOverlay, screenW, screenH);
        return;
    }

    // Create and start the screensaver based on settings
    switch (cachedDisplaySettings.screensaverType) {
        case settings::display::ScreensaverType::Mystify:
            screensaver = std::make_unique<MystifyScreensaver>();
            break;
        case settings::display::ScreensaverType::BouncingBalls:
            screensaver = std::make_unique<BouncingBallsScreensaver>();
            break;
        case settings::display::ScreensaverType::MatrixRain:
            screensaver = std::make_unique<MatrixRainScreensaver>();
            break;
        case settings::display::ScreensaverType::StackChan:
            screensaver = std::make_unique<StackChanScreensaver>();
            break;
        case settings::display::ScreensaverType::RomHackLogo:
            screensaver = std::make_unique<RomHackLogoScreensaver>();
            break;
        case settings::display::ScreensaverType::None:
        default:
            // Just black screen, no animated screensaver
            screensaver = nullptr;
            break;
    }

    if (screensaver) {
        screensaver->start(screensaverOverlay, screenW, screenH);
        animationTimer = lv_timer_create(animationTimerCb, ANIMATION_INTERVAL_MS, this);
    }
}

void DisplayIdleService::updateScreensaver() {
    if (screensaver) {
        lv_coord_t screenW = lv_display_get_horizontal_resolution(nullptr);
        lv_coord_t screenH = lv_display_get_vertical_resolution(nullptr);
        screensaver->update(screenW, screenH);
    }
}

void DisplayIdleService::tick() {
    if (!lvgl_try_lock(100)) {
        return;
    }
    if (lv_display_get_default() == nullptr) {
        lvgl_unlock();
        return;
    }

    // Check for settings reload request (thread-safe)
    if (settingsReloadRequested.exchange(false, std::memory_order_acquire)) {
        cachedDisplaySettings = settings::display::loadOrGetDefault();
    }

    const uint32_t inactive_ms = lv_display_get_inactive_time(nullptr);
    // LVGL's inactivity only grows until an input resets it, so a drop since the last tick is
    // activity. A fixed window would lose the wake whenever this low-priority tick runs late.
    const uint32_t previous_inactive_ms = lastInactiveMs;
    lastInactiveMs = inactive_ms;

    // Advancing the screensaver is the animation timer's job. All this has to decide is when the
    // backlight has been lit for long enough to be worth giving up on.
    if (displayDimmed && screensaverOverlay && !stopScreensaverRequested.load(std::memory_order_acquire) &&
        !backlightOff) {
        screensaverActiveCounter++;
        if (screensaverActiveCounter >= SCREENSAVER_AUTO_OFF_TICKS) {
            stopAnimationTimer();
            if (screensaver) {
                screensaver->stop();
                screensaver.reset();
            }
            setBacklightBrightness(0);
            backlightOff = true;
        }
    }

    lvgl_unlock();

    // Check stop request early for faster response
    if (stopScreensaverRequested.load(std::memory_order_acquire)) {
        stopScreensaver();
        return;
    }

    if (hasDisplayWithBacklight()) {
        if (isScreensaverInhibited() || !cachedDisplaySettings.backlightTimeoutEnabled || cachedDisplaySettings.backlightTimeoutMs == 0) {
            if (displayDimmed) {
                // The whole screensaver comes down, not just the backlight: restoring brightness
                // alone leaves the overlay covering a lit screen that nothing updates.
                stopScreensaver();
            }
        } else {
            if (!displayDimmed && inactive_ms >= cachedDisplaySettings.backlightTimeoutMs) {
                if (!lvgl_try_lock(100)) {
                    return; // Retry on next tick
                }
                activateScreensaver();
                lvgl_unlock();
                // Turn off backlight for "None" screensaver (just black screen)
                if (cachedDisplaySettings.screensaverType == settings::display::ScreensaverType::None) {
                    setBacklightBrightness(0);
                }
                displayDimmed = true;
            } else if (displayDimmed && inactive_ms < previous_inactive_ms) {
                stopScreensaver();
            }
        }
    }
}

bool DisplayIdleService::onStart(ServiceContext& service) {
    // Seed random number generator for varied screensaver patterns
    srand(static_cast<unsigned int>(time(nullptr)));

    cachedDisplaySettings = settings::display::loadOrGetDefault();

    timer = std::make_unique<Timer>(Timer::Type::Periodic, millis_to_ticks(TICK_INTERVAL_MS), [this]{ this->tick(); });
    timer->start();
    return true;
}

void DisplayIdleService::onStop(ServiceContext& service) {
    if (timer) {
        timer->stop();
        timer = nullptr;
    }
    if (screensaverOverlay) {
        // Retry screensaver cleanup during shutdown
        constexpr int maxRetries = 5;
        for (int i = 0; i < maxRetries && screensaverOverlay; ++i) {
            stopScreensaver();
            if (screensaverOverlay && i < maxRetries - 1) {
                delay_millis(50); // Brief delay before retry
            }
        }
        if (screensaverOverlay) {
            LOG_W(TAG, "Failed to stop screensaver during shutdown - potential resource leak");
        }
    }
    screensaver.reset();
}

void DisplayIdleService::startScreensaver() {
    if (!lvgl_try_lock(100)) {
        return;
    }

    // Reload settings to get current screensaver type
    // Note: This is safe because we hold the LVGL lock which serializes with tick()
    cachedDisplaySettings = settings::display::loadOrGetDefault();

    activateScreensaver();
    lvgl_unlock();

    // Turn off backlight for "None" screensaver
    if (hasDisplayWithBacklight() && cachedDisplaySettings.screensaverType == settings::display::ScreensaverType::None) {
        setBacklightBrightness(0);
    }
    displayDimmed = true;
}

bool DisplayIdleService::isScreensaverActive() const {
    return screensaverOverlay != nullptr;
}

void DisplayIdleService::reloadSettings() {
    // Set flag for thread-safe reload - actual reload happens in tick()
    settingsReloadRequested.store(true, std::memory_order_release);
}

std::shared_ptr<DisplayIdleService> findService() {
    return std::static_pointer_cast<DisplayIdleService>(
        findServiceById("tactility.displayidle")
    );
}

extern const ServiceManifest manifest = {
    .id = "tactility.displayidle",
    .createService = create<DisplayIdleService>
};

}

#endif // ESP_PLATFORM
