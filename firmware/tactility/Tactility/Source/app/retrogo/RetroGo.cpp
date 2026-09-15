#include "Tactility/Tactility.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <lvgl.h>
#include <lvgl/fonts.h>
#include <tactility/check.h>
#include <tactility/log.h>

#ifdef ESP_PLATFORM
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#endif

namespace tt::app::retrogo {

extern const ::AppManifest manifest;

namespace {

constexpr const char* TAG = "RetroGo";

// Retro-Go's launcher, by the partition label it is flashed to. Must stay in step with the
// -retrogo partition tables and with RG_APP_LAUNCHER in Retro-Go's target config.
constexpr const char* LAUNCHER_PARTITION = "launcher";

// Long enough for the "Starting Retro-Go" label to reach the panel before the chip resets.
constexpr uint32_t RESTART_DELAY_MS = 500;

struct Context {
    uint32_t appInstanceId;
    lv_obj_t* status;
};

#ifdef ESP_PLATFORM

/**
 * Points the bootloader at Retro-Go's launcher for the next boot. esp_ota_set_boot_partition()
 * validates the image first, so an empty slot is rejected here rather than bricking that boot.
 */
const char* selectRetroGo() {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, LAUNCHER_PARTITION
    );
    if (partition == nullptr) {
        LOG_E(TAG, "no partition labelled '%s'", LAUNCHER_PARTITION);
        return "Retro-Go is not in the partition table.";
    }

    esp_err_t error = esp_ota_set_boot_partition(partition);
    if (error == ESP_ERR_OTA_VALIDATE_FAILED) {
        LOG_E(TAG, "'%s' holds no valid image", LAUNCHER_PARTITION);
        return "Retro-Go is not flashed yet.";
    }
    if (error != ESP_OK) {
        LOG_E(TAG, "esp_ota_set_boot_partition failed: 0x%x", error);
        return "Could not select Retro-Go.";
    }
    return nullptr;
}

#endif

void onStartPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));

#ifdef ESP_PLATFORM
    const char* failure = selectRetroGo();
    if (failure != nullptr) {
        lv_label_set_text(ctx->status, failure);
        return;
    }

    lv_label_set_text(ctx->status, "Starting Retro-Go...");
    getMainDispatcher().dispatch([] {
        vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
        esp_restart();
    });
#else
    lv_label_set_text(ctx->status, "Retro-Go only runs on the device.");
#endif
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* title = lv_label_create(parent);
    lv_label_set_text(title, "Retro-Go");
    lv_obj_set_style_text_font(title, lvgl_get_text_font(FONT_SIZE_LARGE), 0);

    ctx->status = lv_label_create(parent);
    lv_label_set_text(ctx->status, "Reboots the badge into Retro-Go.");

    auto* start_button = lv_button_create(parent);
    auto* start_label = lv_label_create(start_button);
    lv_label_set_text(start_label, "Start");
    lv_obj_add_event_cb(start_button, onStartPressed, LV_EVENT_SHORT_CLICKED, ctx);
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    Context ctx {};
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
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.retrogo",
    .name = "Retro-Go",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
};

} // namespace
