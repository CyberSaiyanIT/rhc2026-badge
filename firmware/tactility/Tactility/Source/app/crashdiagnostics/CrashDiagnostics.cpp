#ifdef ESP_PLATFORM

#include "Tactility/PanicHandler.h"

#include <Tactility/app/launcher/Launcher.h>
#include <Tactility/file/File.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <lvgl/fonts.h>
#include <lvgl.h>
#include <tactility/check.h>
#include <tactility/drivers/pointer.h>
#include <tactility/log.h>
#include <tactility/paths.h>

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include <esp_cpu_utils.h>
#else
#include <esp_cpu.h>
#endif

#include <sdkconfig.h>

#include <iomanip>
#include <sstream>

namespace tt::app::crashdiagnostics {

constexpr auto* TAG = "CrashDiagnostics";

extern const ::AppManifest manifest;

namespace {

struct Context {
    uint32_t appInstanceId;
    // Set by onContinuePressed() right before it emits APP_EVENT_CLOSE - read by appMain()
    // after its own thread finishes cleanup, to decide whether to start the launcher
    // afterwards (matches the old model's onContinuePressed(): stop() then launcher::start()).
    bool continuePressed = false;
};


const char* crashCauseToString(CrashCause cause) {
    switch (cause) {
        case CrashCause::Debug: return "Debug";
        case CrashCause::WatchdogInterrupt: return "Watchdog (interrupt)";
        case CrashCause::WatchdogTask: return "Watchdog (task)";
        case CrashCause::Abort: return "Abort";
        case CrashCause::Fault: return "Fault";
        case CrashCause::Unknown:
        default: return "Unknown";
    }
}

uint32_t callstackPc(const CallstackFrame& frame) {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    return esp_cpu_process_stack_pc(frame.pc);
#else
    return frame.pc; // No processing needed on RISC-V
#endif
}

std::string formatCrashData(const CrashData& crashData) {
    std::stringstream stream;

    stream << "Cause: " << crashCauseToString(crashData.cause) << "\n";

    stream << "Reason: ";
    if (crashData.reason[0] != '\0') {
         stream << crashData.reason;
    } else {
        stream << "unknown";
    }
    stream << "\n";

    stream << "Fault address: " << std::hex << std::setw(8) << std::setfill('0') << crashData.faultAddress << std::dec << "\n";

    if (crashData.faultDataAddressValid) {
        stream << "Accessed address: " << std::hex << std::setw(8) << std::setfill('0') << crashData.faultDataAddress << std::dec << "\n";
    }

    stream << "Callstack" << (crashData.callstackCorrupted ? " (corrupted)" : "") << ":";
    if (crashData.callstackLength > 0) {
        stream << "\n";
        for (uint8_t i = 0; i < crashData.callstackLength; i++) {
            stream << std::hex << std::setw(8) << std::setfill('0') << callstackPc(crashData.callstack[i]) << std::dec << " ";
        }
    } else {
        stream << " empty" << "\n";
    }

    return stream.str();
}

// Best-effort: crash.txt is a convenience for offline inspection, not required for the app to work.
void writeCrashLogFile(const CrashData& crashData) {
    char root[128];
    if (paths_get_data_path(root, sizeof(root)) != ERROR_NONE) {
        LOG_E(TAG, "Failed to resolve data path for crash.txt");
        return;
    }

    std::string path = std::string(root) + "/crash.txt";
    if (!file::writeString(path, formatCrashData(crashData))) {
        LOG_E(TAG, "Failed to write %s", path.c_str());
    }
}

void onContinuePressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->continuePressed = true;
    app_event_emit_close(ctx->appInstanceId);
}

// The USB serial console flushes its 64-byte FIFO only on a newline and drops the rest, so this
// writes one short line per chunk. Addresses carry "0x" because idf.py monitor decodes that shape.
void logCrashData(const CrashData& crashData) {
    LOG_I(TAG, "Cause: %s", crashCauseToString(crashData.cause));
    LOG_I(TAG, "Reason: %s", crashData.reason[0] != '\0' ? crashData.reason : "unknown");
    LOG_I(TAG, "Fault address: 0x%08x", (unsigned)crashData.faultAddress);
    if (crashData.faultDataAddressValid) {
        LOG_I(TAG, "Accessed address: 0x%08x", (unsigned)crashData.faultDataAddress);
    }

    if (crashData.callstackLength == 0) {
        LOG_I(TAG, "Backtrace%s: empty", crashData.callstackCorrupted ? " (corrupted)" : "");
        return;
    }

    LOG_I(TAG, "Backtrace%s: %d frames", crashData.callstackCorrupted ? " (corrupted)" : "", (int)crashData.callstackLength);
    constexpr uint8_t frames_per_line = 4;
    for (uint8_t i = 0; i < crashData.callstackLength; i += frames_per_line) {
        std::stringstream stream;
        for (uint8_t j = i; j < crashData.callstackLength && j < i + frames_per_line; j++) {
            stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << callstackPc(crashData.callstack[j]) << std::dec << " ";
        }
        LOG_I(TAG, "  %s", stream.str().c_str());
    }
}

void onDumpKeyPressed(lv_event_t* event) {
    if (lv_event_get_key(event) != LV_KEY_ENTER) {
        return;
    }
    logCrashData(getRtcCrashData());
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_add_event_cb(parent, onContinuePressed, LV_EVENT_SHORT_CLICKED, ctx);
    auto* top_label = lv_label_create(parent);
    lv_label_set_text(top_label, "Oops! We've crashed ..."); // TODO: Funny messages
    lv_obj_align(top_label, LV_ALIGN_TOP_MID, 0, 2);

    auto* bottom_label = lv_label_create(parent);
    if (device_has_active_by_type(&POINTER_TYPE)) {
        lv_label_set_text(bottom_label, "Tap screen to continue");
    } else {
        lv_label_set_text(bottom_label, "Reboot device to continue");
    }
    lv_obj_align(bottom_label, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Nothing else here is focusable, so the keypad has no target. A zero-sized object takes focus
    // without being tappable, leaving the parent's tap-to-continue handler the only touch target.
    if (lv_group_get_default() != nullptr) {
        auto* key_receiver = lv_obj_create(parent);
        lv_obj_remove_style_all(key_receiver);
        lv_obj_set_size(key_receiver, 0, 0);
        lv_obj_add_event_cb(key_receiver, onDumpKeyPressed, LV_EVENT_KEY, nullptr);
        lv_group_add_obj(lv_group_get_default(), key_receiver);
        lv_group_focus_obj(key_receiver);
    }

    auto* sad_face = lv_label_create(parent);
    lv_label_set_text(sad_face, ":-(");
    lv_obj_set_style_text_font(sad_face, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_align(sad_face, LV_ALIGN_CENTER, 0, 0);
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    Context ctx {};
    ctx.appInstanceId = appInstanceId;

    writeCrashLogFile(getRtcCrashData());

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

    bool continuePressed = ctx.continuePressed;

    if (continuePressed) {
        launcher::start();
    }

    return 0;
}

} // namespace

void start() {
    uint32_t instanceId = 0;
    app_manager_start(manifest.id, &instanceId);
}

extern const ::AppManifest manifest = {
    .id = "tactility.crashdiagnostics",
    .name = "Crash Diagnostics",
    .category = APP_CATEGORY_SYSTEM,
    .location = { .type = APP_LOCATION_MEMORY, .location = reinterpret_cast<void*>(appMain) },
    .flags = APP_MANIFEST_FLAG_HIDDEN,
};

} // namespace

#endif
