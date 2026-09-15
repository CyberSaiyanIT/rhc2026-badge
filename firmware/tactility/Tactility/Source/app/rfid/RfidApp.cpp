#include <Tactility/RecursiveMutex.h>
#include <Tactility/Timer.h>
#include <Tactility/LogMessages.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/log.h>
#include <drivers/mfrc522.h>

#include <cassert>
#include <format>
#include <string>
#include <vector>

#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>

namespace tt::app::rfid {

extern const ::AppManifest manifest;

namespace {

constexpr auto* TAG = "RfidApp";

struct Context {
    uint32_t appInstanceId;

    RecursiveMutex mutex;
    std::unique_ptr<Timer> scanTimer = nullptr;
    
    struct Device* rfidDevice = nullptr;
    bool scanning = false;
    std::string lastUid = "";

    lv_obj_t* statusLabelWidget = nullptr;
    lv_obj_t* uidLabelWidget = nullptr;
    lv_obj_t* scanButtonLabelWidget = nullptr;
};

void updateViews(Context* ctx) {
    if (ctx->mutex.lock(100 / portTICK_PERIOD_MS)) {
        if (ctx->scanning) {
            lv_label_set_text(ctx->scanButtonLabelWidget, "Stop Scan");
            lv_label_set_text(ctx->statusLabelWidget, "Scanning for cards...");
        } else {
            lv_label_set_text(ctx->scanButtonLabelWidget, "Start Scan");
            lv_label_set_text(ctx->statusLabelWidget, "Idle");
        }

        if (!ctx->lastUid.empty()) {
            lv_label_set_text(ctx->uidLabelWidget, ctx->lastUid.c_str());
        } else {
            lv_label_set_text(ctx->uidLabelWidget, "");
        }

        ctx->mutex.unlock();
    }
}

void updateViewsSafely(Context* ctx) {
    lvgl_lock();
    updateViews(ctx);
    lvgl_unlock();
}

void onScanTimer(Context* ctx) {
    struct Device* safe_port = nullptr;
    if (ctx->mutex.lock(100 / portTICK_PERIOD_MS)) {
        safe_port = ctx->rfidDevice;
        ctx->mutex.unlock();
    }
    
    if (!safe_port) return;

    // device_get_first_by_compatible hands back a referenced device, and this one is built in,
    // so it outlives the app.
    
    uint8_t uid[10];
    size_t uid_len = 0;
    
    if (mfrc522_read_uid(safe_port, uid, &uid_len) && uid_len > 0) {
        char buf[32] = {0};
        int pos = 0;
        for (size_t i = 0; i < uid_len; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", uid[i]);
        }
        LOG_I(TAG, "UID READ: %s", buf);
        
        if (ctx->mutex.lock(100 / portTICK_PERIOD_MS)) {
            ctx->lastUid = buf;
            ctx->mutex.unlock();
            updateViewsSafely(ctx);
        }
    }
}

/** Stops a scan and drops the reader's power with it. */
void stopScanningIfRunning(Context* ctx);

void stopScanning(Context* ctx) {
    if (ctx->mutex.lock(100 / portTICK_PERIOD_MS)) {
        if (ctx->scanTimer) {
            LOG_I(TAG, "Stopping scan");
            ctx->scanTimer->stop();
            ctx->scanTimer.reset();
        }
        ctx->scanning = false;
        ctx->mutex.unlock();
        updateViewsSafely(ctx);
    }
}

void startScanning(Context* ctx) {
    if (ctx->mutex.lock(100 / portTICK_PERIOD_MS)) {
        if (!ctx->rfidDevice) {
            error_t err = device_get_first_by_compatible("nxp,mfrc522", &ctx->rfidDevice);
            if (err != ERROR_NONE) {
                LOG_E(TAG, "MFRC522 device not found!");
                ctx->mutex.unlock();
                return;
            }
        }

        // The reader is left stopped at boot, so starting it here is what powers its rail. The
        // power manager can refuse on a flat battery, so a failure must leave the app usable.
        if (!device_is_ready(ctx->rfidDevice)) {
            if (device_start(ctx->rfidDevice) != ERROR_NONE) {
                LOG_E(TAG, "Failed to power on the reader");
                device_put(ctx->rfidDevice);
                ctx->rfidDevice = nullptr;
                ctx->mutex.unlock();
                updateViewsSafely(ctx);
                return;
            }
        }
        
        LOG_I(TAG, "Starting scan");
        ctx->lastUid = "";
        ctx->scanning = true;
        ctx->scanTimer = std::make_unique<Timer>(Timer::Type::Periodic, 200, [ctx]{
            onScanTimer(ctx);
        });
        ctx->scanTimer->start();
        ctx->mutex.unlock();
        
        updateViewsSafely(ctx);
    }
}

void onPressScan(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    bool is_scanning = false;
    if (ctx->mutex.lock(100 / portTICK_PERIOD_MS)) {
        is_scanning = ctx->scanning;
        ctx->mutex.unlock();
    }
    
    if (is_scanning) {
        // Not stopScanning(): ending a scan releases the reader too, so the RF field is only up
        // while a scan is actually running.
        stopScanningIfRunning(ctx);
    } else {
        startScanning(ctx);
    }
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    auto* toolbar = lvgl_toolbar_create(parent, "RFID Reader");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);

    auto* main_wrapper = lv_obj_create(parent);
    lv_obj_set_flex_flow(main_wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(main_wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(main_wrapper, 1);

    auto* scan_button = lv_button_create(main_wrapper);
    lv_obj_set_width(scan_button, LV_PCT(100));
    lv_obj_add_event_cb(scan_button, onPressScan, LV_EVENT_SHORT_CLICKED, ctx);
    
    auto* scan_button_label = lv_label_create(scan_button);
    lv_obj_center(scan_button_label);
    lv_label_set_text(scan_button_label, "Start Scan");
    ctx->scanButtonLabelWidget = scan_button_label;

    ctx->statusLabelWidget = lv_label_create(main_wrapper);
    lv_label_set_text(ctx->statusLabelWidget, "Idle");
    lv_obj_set_style_margin_top(ctx->statusLabelWidget, 10, 0);
    
    ctx->uidLabelWidget = lv_label_create(main_wrapper);
    lv_label_set_text(ctx->uidLabelWidget, "");
    lv_obj_set_style_text_font(ctx->uidLabelWidget, &lv_font_montserrat_18, 0);
    lv_obj_set_style_margin_top(ctx->uidLabelWidget, 10, 0);
}

void stopScanningIfRunning(Context* ctx) {
    // The two locks are taken in sequence, never nested: stopScanning() releases before returning.
    stopScanning(ctx);
    
    if (ctx->mutex.lock(250 / portTICK_PERIOD_MS)) {
        if (ctx->rfidDevice) {
            // Drops the RF field again: nothing else on the badge uses the reader.
            if (device_is_ready(ctx->rfidDevice)) {
                device_stop(ctx->rfidDevice);
            }
            device_put(ctx->rfidDevice);
            ctx->rfidDevice = nullptr;
        }
        ctx->mutex.unlock();
    }
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    Context ctx;
    ctx.appInstanceId = appInstanceId;

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(appInstanceId, createWidgets, &ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            switch (event.type) {
                case APP_EVENT_CLOSE:
                    stopScanningIfRunning(&ctx);
                    shouldClose = true;
                    break;
                default:
                    break;
            }
            if (shouldClose) break;
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "RfidApp",
    .name = "RFID Reader",
    .category = APP_CATEGORY_SYSTEM,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) }
};

} // namespace tt::app::rfid
