#include "tactility/memory.h"
#include "tactility/system_event.h"

#include <tactility/check.h>
#include <tactility/delay.h>
#include <tactility/drivers/backlight.h>
#include <tactility/drivers/display.h>
#include <tactility/log.h>
#include <tactility/time.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

#include <Tactility/DeprecatedPaths.h>
#include <Tactility/MountPoints.h>
#include <Tactility/PowerManager.h>
#include <Tactility/TactilityPrivate.h>
#include <Tactility/hal/usb/Usb.h>
#include <Tactility/lvgl/Lvgl.h>
#include <Tactility/lvgl/Style.h>
#include <Tactility/settings/BootSettings.h>
#include <Tactility/settings/DisplaySettings.h>

#include <lvgl.h>

#include <atomic>
#include <format>

#ifdef ESP_PLATFORM
#include <Tactility/app/crashdiagnostics/CrashDiagnostics.h>
#include <Tactility/PanicHandler.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <sdkconfig.h>
#else
#define CONFIG_TT_SPLASH_DURATION 0
#endif

namespace tt::app::boot {

constexpr auto* TAG = "Boot";

extern const ::AppManifest manifest;

namespace {

// Snapshot of hal::usb::isUsbBootMode(), taken before boot work starts and potentially clears
// the underlying flag via setupUsbBootMode()/resetUsbBootMode(). createSplashWidgets() reads
// this instead of the live flag to avoid a race between the two.
std::atomic<bool> isUsbBootSplash = false;

// Set when CONFIG_TT_USER_DATA_LOCATION_SD is defined but no SD card is mounted. Switches the
// window to an error screen and halts before starting the next app.
std::atomic<bool> sdCardMissing = false;

// Battery below tt::power::CRITICAL_BATTERY_MV at boot. Replaces the launcher with a warning that
// counts down to sleep, which the user can override.
std::atomic<bool> batteryCritical = false;
int batteryCriticalMillivolts = 0;

// Set by the critical-battery screen's "Continue" button, handled on this app's own task.
std::atomic<bool> batteryContinueRequested = false;

uint32_t bootAppInstanceId = 0;
WindowId bootWindowId = 0;

// Set once LVGL has rendered and flushed a frame, so the backlight only comes up on a panel that
// already shows the splash instead of on whatever survived in the framebuffer from the last run.
std::atomic<bool> splashDrawn = false;

void onDisplayRefreshReady(lv_event_t*) {
    splashDrawn = true;
}

// Never blocks the boot indefinitely: a device whose display never reports a refresh still gets
// its backlight, just on the timeout path.
void waitForSplashDrawn() {
    constexpr uint32_t poll_interval_ms = 10;
    constexpr uint32_t timeout_ms = 1000;
    for (uint32_t waited = 0; !splashDrawn && waited < timeout_ms; waited += poll_interval_ms) {
        delay_millis(poll_interval_ms);
    }
    if (!splashDrawn) {
        LOG_W(TAG, "Splash not confirmed drawn, enabling backlight anyway");
    }
}

#ifdef ESP_PLATFORM
constexpr auto PARTITION_PREFIX = std::string("/");
#else
constexpr auto PARTITION_PREFIX = std::string("");
#endif

// Equivalent of AppPaths::getAssetsPath() for the internal "Boot" app id, without needing a
// live AppContext (which this app no longer has under the new app-module model).
std::string getBootAssetsPath(const std::string& childPath) {
    return std::format("{}{}/app/Boot/assets/{}", PARTITION_PREFIX, file::SYSTEM_PARTITION_NAME, childPath);
}

void setupDisplay() {
    // TODO: Support for multiple displays

    Device* display = nullptr;
    if (device_get_first_by_type(&DISPLAY_TYPE, &display) != ERROR_NONE) {
        LOG_I(TAG, "No kernel display");
        return;
    }

    // Set backlight brightness
    Device* backlight;
    if (display_get_backlight(display, &backlight) == ERROR_NONE) {
        if (!device_is_ready(backlight)) {
            // Nothing has lit the panel yet, so hold off until LVGL has actually put the splash
            // on it. An early-splash device has already done both and skips straight through.
            waitForSplashDrawn();
            if (device_start(backlight) != ERROR_NONE) {
                LOG_E(TAG, "Failed to start %s", backlight->name);
            }
        }

        settings::display::DisplaySettings settings;
        if (settings::display::load(settings)) {
        } else {
            settings = settings::display::getDefault();
        }

        if (backlight_set_brightness(backlight, settings.backlightDuty) == ERROR_NONE) {
            LOG_I(TAG, "Backlight for %s set to %d", display->name, settings.backlightDuty);
        } else {
            LOG_E(TAG, "Failed to set brightness of %s", backlight->name);
        }
        device_put(backlight);
    } else {
        LOG_I(TAG, "No backlight for %s", display->name);
    }

    device_put(display);
}

bool setupUsbBootMode() {
    if (!hal::usb::isUsbBootMode()) {
        return false;
    }

    LOG_I(TAG, "Rebooting into mass storage device mode");
    auto mode = hal::usb::getUsbBootMode();  // Get mode before reset
    hal::usb::resetUsbBootMode();
    if (mode == hal::usb::BootMode::Flash) {
        if (!hal::usb::startMassStorageWithFlash(true)) {
            LOG_E(TAG, "Unable to start flash mass storage");
            return false;
        }
    } else if (mode == hal::usb::BootMode::Sdmmc) {
        if (!hal::usb::startMassStorageWithSdmmc(true)) {
            LOG_E(TAG, "Unable to start SD mass storage");
            return false;
        }
    }

    return true;
}

void waitForMinimalSplashDuration(TickType_t startTime) {
    const auto end_time = get_ticks();
    const auto ticks_passed = end_time - startTime;
    constexpr auto minimum_ticks = (CONFIG_TT_SPLASH_DURATION / portTICK_PERIOD_MS);
    if (minimum_ticks > ticks_passed) {
        delay_ticks(minimum_ticks - ticks_passed);
    }
}

std::string getLauncherAppId() {
    settings::BootSettings boot_properties;
    // When boot.properties hasn't been overridden, return default
    if (!settings::loadBootSettings(boot_properties)) {
        return CONFIG_TT_LAUNCHER_APP_ID;
    }

    // When boot properties didn't specify an override, return default
    if (boot_properties.launcherAppId.empty()) {
        LOG_E(TAG, "Failed to load launcher configuration, or launcher not configured");
        return CONFIG_TT_LAUNCHER_APP_ID;
    }

    // If the app in the boot.properties does not exist, return default
    AppManifest manifest;
    if (app_manager_find_manifest(boot_properties.launcherAppId.c_str(), &manifest) != ERROR_NONE) {
        LOG_E(TAG, "Launcher app %s not found", boot_properties.launcherAppId.c_str());
        return CONFIG_TT_LAUNCHER_APP_ID;
    }

    // The boot.properties launcher app id is valid
    return boot_properties.launcherAppId;
}

int getSmallestDimension() {
    auto* display = lv_display_get_default();
    int width = lv_display_get_horizontal_resolution(display);
    int height = lv_display_get_vertical_resolution(display);
    return std::min(width, height);
}

lv_obj_t* createLogo(lv_obj_t* parent, const char* asset) {
    auto* image = lv_image_create(parent);
    lv_obj_set_size(image, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    const auto path = lvgl::PATH_PREFIX + getBootAssetsPath(asset);
    lv_image_set_src(image, path.c_str());
    return image;
}

void createSplashWidgets(lv_obj_t* root, void*) {
    lvgl::obj_set_style_bg_blacken(root);
    lv_obj_set_style_border_width(root, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(root, 0, LV_STATE_DEFAULT);

    // TODO: Replace with automatic asset buckets like on Android
    if (isUsbBootSplash) {
        lv_obj_align(createLogo(root, "logo_usb.png"), LV_ALIGN_CENTER, 0, 0);
    } else if (getSmallestDimension() < 150) { // e.g. Cardputer
        lv_obj_align(createLogo(root, "logo_small.png"), LV_ALIGN_CENTER, 0, 0);
    } else {
        // The event and community logos stack as one centred block, so the pair stays centred
        // together regardless of the individual asset heights.
        auto* column = lv_obj_create(root);
        lv_obj_remove_style_all(column);
        lv_obj_set_size(column, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_align(column, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(column, 8, LV_STATE_DEFAULT);
        createLogo(column, "logo_romhack.png");
        createLogo(column, "logo_cybersaiyan.png");

        lv_obj_align(createLogo(root, "logo_traboda.png"), LV_ALIGN_BOTTOM_MID, 0, -12);
    }

#ifdef ESP_PLATFORM
    if (isUsbBootSplash) {
        auto* button = lv_button_create(root);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -16);
        auto* label = lv_label_create(button);
        lv_label_set_text(label, "Return to OS");
        lv_obj_add_event_cb(button, [](lv_event_t*) {
            hal::usb::stop();
            esp_restart();
        }, LV_EVENT_SHORT_CLICKED, nullptr);
    }
#endif
}

/** Seconds the warning stays up before it puts the badge to sleep on its own. */
constexpr int BATTERY_CRITICAL_COUNTDOWN_S = 30;

lv_timer_t* batteryCountdownTimer = nullptr;
lv_obj_t* batteryCountdownLabel = nullptr;
int batteryCountdownSeconds = BATTERY_CRITICAL_COUNTDOWN_S;

void stopBatteryCountdown() {
    if (batteryCountdownTimer != nullptr) {
        lv_timer_delete(batteryCountdownTimer);
        batteryCountdownTimer = nullptr;
    }
}

/**
 * The badge has no power switch, so deep sleep is the only way to stop draining a flat cell. MENU
 * is on GPIO0, the one RTC-capable key and so the only one that can wake it.
 */
void enterSleep() {
    stopBatteryCountdown();
#ifdef ESP_PLATFORM
    Device* display = nullptr;
    if (device_get_first_by_type(&DISPLAY_TYPE, &display) == ERROR_NONE) {
        Device* backlight = nullptr;
        if (display_get_backlight(display, &backlight) == ERROR_NONE) {
            backlight_set_brightness(backlight, 0);
            device_put(backlight);
        }
        device_put(display);
    }
    // The keypad's pull-up is dropped when deep sleep starts, so the pad is pulled up through the
    // RTC path instead; otherwise the wake pin floats and wakes the badge straight back up.
    rtc_gpio_pullup_en(GPIO_NUM_0);
    rtc_gpio_pulldown_dis(GPIO_NUM_0);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
#else
    LOG_I(TAG, "Sleep requested");
#endif
}

void onBatteryCountdownTick(lv_timer_t*) {
    batteryCountdownSeconds--;
    if (batteryCountdownSeconds <= 0) {
        enterSleep();
        return;
    }
    if (batteryCountdownLabel != nullptr) {
        lv_label_set_text_fmt(batteryCountdownLabel, "Sleeping in %d s", batteryCountdownSeconds);
    }
}

void onBatteryContinuePressed(lv_event_t*);

void createBatteryCriticalWidgets(lv_obj_t* root, void*) {
    lvgl::obj_set_style_bg_blacken(root);
    lv_obj_set_style_border_width(root, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(root, 0, LV_STATE_DEFAULT);

    auto* title = lv_label_create(root);
    lv_label_set_text(title, "Battery critically low");
    lv_obj_set_style_text_font(title, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEF4444), LV_STATE_DEFAULT);
    lv_obj_set_width(title, LV_PCT(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);

    auto* description = lv_label_create(root);
    lv_label_set_text_fmt(description, "%d mV left. Charge the badge.", batteryCriticalMillivolts);
    lv_obj_set_width(description, LV_PCT(90));
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(description, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(description, LV_ALIGN_CENTER, 0, 0);

    const int title_margin = lvgl_get_text_font_height(FONT_SIZE_LARGE);
    lv_obj_align_to(title, description, LV_ALIGN_OUT_TOP_MID, 0, -title_margin);

    batteryCountdownLabel = lv_label_create(root);
    lv_label_set_text_fmt(batteryCountdownLabel, "Sleeping in %d s", batteryCountdownSeconds);
    lv_obj_set_style_text_color(batteryCountdownLabel, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align_to(batteryCountdownLabel, description, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    auto* sleep_button = lv_button_create(root);
    auto* sleep_label = lv_label_create(sleep_button);
    lv_label_set_text(sleep_label, "Sleep");
    lv_obj_center(sleep_label);
    lv_obj_align(sleep_button, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_add_event_cb(sleep_button, [](lv_event_t*) { enterSleep(); }, LV_EVENT_SHORT_CLICKED, nullptr);

    auto* continue_button = lv_button_create(root);
    auto* continue_label = lv_label_create(continue_button);
    lv_label_set_text(continue_label, "Continue");
    lv_obj_center(continue_label);
    lv_obj_align(continue_button, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_add_event_cb(continue_button, onBatteryContinuePressed, LV_EVENT_SHORT_CLICKED, nullptr);

    lv_group_focus_obj(continue_button);

    stopBatteryCountdown();
    batteryCountdownTimer = lv_timer_create(onBatteryCountdownTick, 1000, nullptr);
}

void createSdCardMissingWidgets(lv_obj_t* root, void*) {
    lvgl::obj_set_style_bg_blacken(root);
    lv_obj_set_style_border_width(root, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(root, 0, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* label = lv_label_create(root);
    lv_label_set_text(label, "SD card not found.\nPlease insert one and reboot.");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);

    auto* button = lv_button_create(root);
    lv_obj_set_style_margin_top(button, 16, LV_STATE_DEFAULT);
    auto* button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Reboot");
    lv_obj_add_event_cb(button, [](lv_event_t*) {
#ifdef ESP_PLATFORM
        esp_restart();
#endif
    }, LV_EVENT_SHORT_CLICKED, nullptr);
}

// Replaces the splash with a self-contained error screen (no dependency on the old alertdialog
// app - this app has no parent in the old App stack to deliver a result back to).
void showSdCardMissingScreen() {
    if (bootWindowId != 0) {
        window_manager_remove(bootWindowId);
    }
    bootWindowId = window_manager_create(bootAppInstanceId, createSdCardMissingWidgets, nullptr);
}

void startLauncher() {
    auto launcher_app_id = getLauncherAppId();
    uint32_t launcher_instance_id = 0;
    app_manager_start(launcher_app_id.c_str(), &launcher_instance_id);
}

/**
 * Only records the choice and wakes this app's task. Starting the launcher here would run it on
 * the LVGL task with the lock held, and boot-completed subscribers block and touch LVGL.
 */
void onBatteryContinuePressed(lv_event_t*) {
    stopBatteryCountdown();
    batteryContinueRequested = true;
    app_event_emit_close(bootAppInstanceId);
}

void showBatteryCriticalScreen() {
    if (bootWindowId != 0) {
        window_manager_remove(bootWindowId);
    }
    bootWindowId = window_manager_create(bootAppInstanceId, createBatteryCriticalWidgets, nullptr);
}

void startNextApp() {
    if (sdCardMissing) {
        showSdCardMissingScreen();
        return;
    }

    if (batteryCritical) {
        showBatteryCriticalScreen();
        return;
    }

#ifdef ESP_PLATFORM
    if (esp_reset_reason() == ESP_RST_PANIC) {
        crashdiagnostics::start(); // fire-and-forget; no result expected back
        return;
    }
#endif

    startLauncher();
}

void runBootSequence(TickType_t startTime) {
    LOG_I(TAG, "Starting boot sequence");

    // Give the UI some time to redraw
    // If we don't do this, various init calls will read files and block SPI IO for the display
    // This would result in a blank/black screen being shown during this phase of the boot process
    // This works with 5 ms on a T-Lora Pager, so we give it 10 ms to be safe
    delay_millis(10);

    LOG_I(TAG, "Setup display");
    setupDisplay();
    LOG_I(TAG, "Prepare file systems");
    prepareFileSystems();

#ifdef CONFIG_TT_USER_DATA_LOCATION_SD
    std::string sd_path;
    if (!findFirstMountedSdCardPath(sd_path)) {
        LOG_E(TAG, "SD card not found");
        sdCardMissing = true;
    }
#endif

    if (int millivolts = 0; tt::power::readBatteryMillivolts(millivolts)) {
        batteryCriticalMillivolts = millivolts;
        batteryCritical = millivolts < tt::power::CRITICAL_BATTERY_MV;
        LOG_I(TAG, "Battery: %d mV%s", millivolts, batteryCritical ? " (critically low)" : "");
    } else {
        // No sensor, or nothing it returned looked like a cell. Booting normally is the only safe
        // reading of that: a broken sensor must not be able to hold the badge off.
        LOG_W(TAG, "Battery voltage unknown, assuming healthy");
    }

    if (setupUsbBootMode()) {
        // Stay open: the splash's "Return to OS" button is this app's only way to leave mass
        // storage mode, so it must not self-close here like the normal boot path does below.
        return;
    }

    registerApps();
    waitForMinimalSplashDuration(startTime);
    startNextApp();

    if (sdCardMissing || batteryCritical) {
        // Stay open: the error screen's own buttons are this app's only way to leave here.
        return;
    }

    // This event will likely block as other systems are initialized
    // e.g. Wi-Fi reads AP configs from SD card
    LOG_I(TAG, "Publish event");
    system_event_emit(KERNEL_EVENT_BOOT_COMPLETED, nullptr, 0);

    app_event_emit_close(app_scheduler_current_app_id());
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    bootAppInstanceId = appInstanceId;
    const auto start_time = get_ticks();

    // Snapshot before runBootSequence() potentially clears the flag via setupUsbBootMode()
    isUsbBootSplash = hal::usb::isUsbBootMode();
    sdCardMissing = false;
    batteryCritical = false;
    batteryContinueRequested = false;
    batteryCountdownSeconds = BATTERY_CRITICAL_COUNTDOWN_S;

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    splashDrawn = false;
    lvgl_lock();
    if (auto* display = lv_display_get_default(); display != nullptr) {
        lv_display_add_event_cb(display, onDisplayRefreshReady, LV_EVENT_REFR_READY, nullptr);
    }
    lvgl_unlock();

#ifdef CONFIG_TT_EARLY_SPLASH
    // The panel has shown the splash since before LVGL existed, and the window manager's root
    // clears the screen first, so rebuilding it reads as the splash drawn twice. USB boot mode
    // still needs a real window for its "Return to OS" button.
    const bool needsSplashWindow = isUsbBootSplash;
#else
    constexpr bool needsSplashWindow = true;
#endif
    if (needsSplashWindow) {
        bootWindowId = window_manager_create(appInstanceId, createSplashWidgets, nullptr);
    }

    runBootSequence(start_time);

    // Waits until app_manager_start(launcher) (or a permanent stop) tells us to give up -
    // startNextApp() above is what triggers that, via app-module's "save the previously active
    // app" policy, unless sdCardMissing halted before it.
    while (true) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        bool shouldClose = false;
        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
        if (shouldClose) {
            // The rest of the ordinary boot path, which the critical-battery screen held back.
            if (batteryContinueRequested.exchange(false)) {
                batteryCritical = false;
                startLauncher();
                LOG_I(TAG, "Publish event");
                system_event_emit(KERNEL_EVENT_BOOT_COMPLETED, nullptr, 0);
            }
            break;
        }
    }

    lvgl_lock();
    if (auto* display = lv_display_get_default(); display != nullptr) {
        lv_display_remove_event_cb_with_user_data(display, onDisplayRefreshReady, nullptr);
    }
    lvgl_unlock();

    stopBatteryCountdown();
    if (bootWindowId != 0) {
        window_manager_remove(bootWindowId);
    }
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.boot",
    .name = "Boot",
    .category = APP_CATEGORY_SYSTEM,
    .location = { .type = APP_LOCATION_MEMORY, .location = reinterpret_cast<void*>(appMain) },
    .flags = APP_MANIFEST_FLAG_HIDDEN,
    .stack = { .depth = 4096, .desired_memory_capability = MEMORY_CAPABILITY_INTERNAL }
};

} // namespace
