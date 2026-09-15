#include <Tactility/file/File.h>
#include <Tactility/network/Http.h>
#include <Tactility/service/wifi/Wifi.h>
#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl.h>
#include <tactility/log.h>

#include <cJSON.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace tt::app::announcements {

constexpr auto* TAG = "Announcements";

namespace {

constexpr auto* ANNOUNCEMENTS_URL = "https://romhack.io/announcements.php";
/** Kept so the last fetch is still readable with the radio off, which is most of the time. */
constexpr auto* ANNOUNCEMENTS_PATH = "/data/announcements.json";

/** The auto-connect timer retries about every two seconds, so this allows several attempts. */
constexpr int WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr int POLL_INTERVAL_MS = 100;

enum class FetchState {
    Idle,
    WaitingForWifi,
    Busy,
    Succeeded,
    Failed
};

struct Announcement {
    std::string title;
    std::string timestamp;
    std::string content;
};

/**
 * Outlives the app's task: the download callbacks run on the main dispatcher and can still fire
 * after the user closed the app, so both they and the app task hold a share of it.
 */
struct Context {
    uint32_t appInstanceId = 0;
    std::vector<Announcement> announcements;
    std::string statusText;
    lv_obj_t* list = nullptr;

    /** -1 while the list is showing. Committed before the detail window is pushed, never by it. */
    int selected = -1;
    WindowId listWindow = 0;
    WindowId detailWindow = 0;

    int wifiWaitedMs = 0;
    std::atomic<bool> refreshRequested { false };
    std::atomic<int> pendingSelection { -1 };
    std::atomic<bool> pendingBack { false };
    std::atomic<FetchState> fetchState { FetchState::Idle };
    std::shared_ptr<std::vector<uint8_t>> downloaded;
};

/** "2026-09-13T15:40:12+02:00" reads better as "2026-09-13 15:40" on a badge-sized screen. */
std::string formatTimestamp(const std::string& iso) {
    if (iso.size() < 16 || iso[10] != 'T') {
        return iso;
    }
    return iso.substr(0, 10) + " " + iso.substr(11, 5);
}

bool parseAnnouncements(const char* json, std::vector<Announcement>& out, std::string& outStatus) {
    out.clear();

    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        outStatus = "Could not read announcements.";
        return false;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        outStatus = "Unexpected announcements format.";
        return false;
    }

    const int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (item == nullptr) continue;
        const auto text = [item](const char* key) -> std::string {
            cJSON* value = cJSON_GetObjectItem(item, key);
            return (value != nullptr && value->valuestring != nullptr) ? value->valuestring : "";
        };
        out.push_back(Announcement { text("title"), text("timestamp"), text("content") });
    }

    cJSON_Delete(root);
    outStatus = out.empty() ? "No announcements yet." : "";
    return true;
}

bool loadStored(std::vector<Announcement>& out, std::string& outStatus) {
    auto json = file::readString(ANNOUNCEMENTS_PATH);
    if (!json) {
        out.clear();
        outStatus = "Press Play to check for announcements.";
        return false;
    }
    return parseAnnouncements(reinterpret_cast<const char*>(json.get()), out, outStatus);
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void onRefreshPressed(lv_event_t* event) {
    static_cast<Context*>(lv_event_get_user_data(event))->refreshRequested = true;
}

/** Records which one was picked; main() decides what that means. */
void onAnnouncementPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const auto index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    ctx->pendingSelection = static_cast<int>(index);
}

void onListDeleted(lv_event_t* event) {
    static_cast<Context*>(lv_event_get_user_data(event))->list = nullptr;
}

void renderList(Context* ctx) {
    if (ctx->list == nullptr) {
        return;
    }
    lv_obj_clean(ctx->list);

    if (!ctx->statusText.empty()) {
        lv_obj_t* status = lv_label_create(ctx->list);
        lv_obj_set_style_text_font(status, lvgl_get_text_font(FONT_SIZE_DEFAULT), LV_PART_MAIN);
        lv_label_set_text(status, ctx->statusText.c_str());
        lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(status, LV_PCT(100));
        lv_obj_set_style_pad_all(status, 8, LV_PART_MAIN);
        lv_obj_set_style_text_color(status, lv_color_hex(0x888888), LV_PART_MAIN);
    }

    for (size_t i = 0; i < ctx->announcements.size(); i++) {
        const auto& announcement = ctx->announcements[i];

        lv_obj_t* item = lv_button_create(ctx->list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(item, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_gap(item, 2, LV_PART_MAIN);
        lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(item, onAnnouncementPressed, LV_EVENT_SHORT_CLICKED, ctx);

        lv_obj_t* title = lv_label_create(item);
        lv_obj_set_style_text_font(title, lvgl_get_text_font(FONT_SIZE_DEFAULT), LV_PART_MAIN);
        lv_label_set_text(title, announcement.title.c_str());
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, LV_PCT(100));

        lv_obj_t* when = lv_label_create(item);
        lv_obj_set_style_text_font(when, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
        lv_label_set_text(when, formatTimestamp(announcement.timestamp).c_str());
        lv_obj_set_style_text_opa(when, LV_OPA_70, LV_PART_MAIN);
    }
}

void createListWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Announcements");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);
    lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_REFRESH, onRefreshPressed, ctx);

    ctx->list = lv_obj_create(parent);
    lv_obj_set_width(ctx->list, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_set_flex_flow(ctx->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(ctx->list, onListDeleted, LV_EVENT_DELETE, ctx);

    renderList(ctx);
}

void onDetailBackPressed(lv_event_t* event) {
    static_cast<Context*>(lv_event_get_user_data(event))->pendingBack = true;
}

/**
 * lv_obj does not scroll itself on key presses and, unlike lv_button, never joins the focus group,
 * so an announcement longer than the screen would be unreadable on a keypad without this.
 */
void onDetailKey(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    lv_obj_t* body = lv_event_get_target_obj(event);
    const uint32_t key = lv_event_get_key(event);

    if (key == LV_KEY_ESC) {
        ctx->pendingBack = true;
        return;
    }

    int32_t step = 0;
    if (key == LV_KEY_UP) step = -40;
    else if (key == LV_KEY_DOWN) step = 40;
    else return;

    int32_t scroll_y = lv_obj_get_scroll_y(body) + step;
    const int32_t max_y = std::max<int32_t>(0, lv_obj_get_scroll_bottom(body) + lv_obj_get_scroll_y(body));
    scroll_y = std::clamp<int32_t>(scroll_y, 0, max_y);
    lv_obj_scroll_to_y(body, scroll_y, LV_ANIM_ON);
}

/**
 * Renders whichever announcement main() already committed to Context::selected. This can run on
 * another app's task when the window resurfaces, so it must not choose anything itself.
 */
void createDetailWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (ctx->selected < 0 || static_cast<size_t>(ctx->selected) >= ctx->announcements.size()) {
        return;
    }
    const auto& announcement = ctx->announcements[ctx->selected];

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, announcement.title.c_str());
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_LEFT, onDetailBackPressed, ctx);

    lv_obj_t* body = lv_obj_create(parent);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 8, LV_PART_MAIN);

    lv_obj_t* when = lv_label_create(body);
    lv_obj_set_style_text_font(when, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
    lv_label_set_text(when, formatTimestamp(announcement.timestamp).c_str());
    lv_obj_set_style_text_opa(when, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(when, 6, LV_PART_MAIN);

    lv_obj_t* content = lv_label_create(body);
    lv_obj_set_style_text_font(content, lvgl_get_text_font(FONT_SIZE_DEFAULT), LV_PART_MAIN);
    lv_label_set_text(content, announcement.content.c_str());
    lv_label_set_long_mode(content, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(content, LV_PCT(100));

    // Focusable so the key handler above can reach it; the toolbar's back button stays in the
    // group too, so NEXT still cycles to it.
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(lv_group_get_default(), body);
    lv_group_focus_obj(body);
    lv_obj_add_event_cb(body, onDetailKey, LV_EVENT_KEY, ctx);
}

void setStatus(Context* ctx, const char* text) {
    lvgl_lock();
    ctx->statusText = text;
    renderList(ctx);
    lvgl_unlock();
}

void startFetch(const std::shared_ptr<Context>& ctx) {
    ctx->fetchState = FetchState::Busy;
    setStatus(ctx.get(), "Checking for updates...");

    // To memory: the payload is small and the copy on flash is only written once it parses.
    network::http::downloadToMemory(
        ANNOUNCEMENTS_URL,
        "",
        [ctx](std::shared_ptr<std::vector<uint8_t>> data) {
            ctx->downloaded = std::move(data);
            ctx->fetchState = FetchState::Succeeded;
        },
        [ctx](const char* errorMessage) {
            LOG_E(TAG, "Download error: %s", errorMessage);
            ctx->fetchState = FetchState::Failed;
        }
    );
}

/**
 * The radio is usually off to save power, so a refresh turns it on and lets the Wi-Fi service's
 * own auto-connect reach a saved network.
 */
void startWifi(const std::shared_ptr<Context>& ctx) {
    ctx->fetchState = FetchState::WaitingForWifi;
    ctx->wifiWaitedMs = 0;
    const auto radio = service::wifi::getRadioState();
    if (radio == service::wifi::RadioState::Off || radio == service::wifi::RadioState::OffPending) {
        service::wifi::setEnabled(true);
    }
    setStatus(ctx.get(), "Connecting to Wi-Fi...");
}

void pollWifi(const std::shared_ptr<Context>& ctx) {
    if (service::wifi::getRadioState() == service::wifi::RadioState::ConnectionActive) {
        startFetch(ctx);
        return;
    }
    ctx->wifiWaitedMs += POLL_INTERVAL_MS;
    if (ctx->wifiWaitedMs >= WIFI_CONNECT_TIMEOUT_MS) {
        ctx->fetchState = FetchState::Idle;
        setStatus(ctx.get(), "Could not connect to Wi-Fi");
    }
}

/** Parses what was fetched and keeps it, so the list is readable again with the radio off. */
void applyFetch(Context* ctx) {
    std::vector<Announcement> announcements;
    std::string status;

    auto data = std::move(ctx->downloaded);
    if (data == nullptr) {
        status = "Download produced nothing.";
    } else {
        data->push_back(0); // cJSON needs a terminator; the body is not one
        const auto* json = reinterpret_cast<const char*>(data->data());
        if (parseAnnouncements(json, announcements, status)) {
            if (!file::writeString(ANNOUNCEMENTS_PATH, json)) {
                LOG_E(TAG, "Failed to store %s", ANNOUNCEMENTS_PATH);
            }
        } else {
            // Nothing was stored, so the installed copy is still the last good one.
            std::string ignored;
            loadStored(announcements, ignored);
        }
    }

    lvgl_lock();
    ctx->announcements = std::move(announcements);
    ctx->statusText = std::move(status);
    renderList(ctx);
    lvgl_unlock();
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    auto ctx = std::make_shared<Context>();
    ctx->appInstanceId = appInstanceId;

    loadStored(ctx->announcements, ctx->statusText);

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    app_event_subscribe(&sub, &event_group);

    ctx->listWindow = window_manager_create(appInstanceId, createListWidgets, ctx.get());

    bool shouldClose = false;
    while (!shouldClose) {
        // No dedicated wake-up bit exists for the atomics below (set from LVGL's event handlers and
        // the download's dispatcher-thread callbacks), so they are polled.
        task_event_group_wait_any(&event_group, nullptr, pdMS_TO_TICKS(POLL_INTERVAL_MS));

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
        if (shouldClose) {
            continue;
        }

        // Window transitions happen here rather than in the LVGL callbacks, so create_widgets only
        // ever renders state that is already committed.
        const int selection = ctx->pendingSelection.exchange(-1);
        if (selection >= 0 && ctx->detailWindow == 0 &&
            static_cast<size_t>(selection) < ctx->announcements.size()) {
            ctx->selected = selection;
            ctx->detailWindow = window_manager_create(appInstanceId, createDetailWidgets, ctx.get());
        }

        if (ctx->pendingBack.exchange(false) && ctx->detailWindow != 0) {
            window_manager_remove(ctx->detailWindow);
            ctx->detailWindow = 0;
            ctx->selected = -1;
        }

        if (ctx->fetchState == FetchState::WaitingForWifi) {
            pollWifi(ctx);
        } else if (ctx->refreshRequested.exchange(false) && ctx->fetchState == FetchState::Idle) {
            startWifi(ctx);
        } else if (ctx->fetchState == FetchState::Succeeded) {
            ctx->fetchState = FetchState::Idle;
            applyFetch(ctx.get());
        } else if (ctx->fetchState == FetchState::Failed) {
            ctx->fetchState = FetchState::Idle;
            ctx->downloaded.reset();
            setStatus(ctx.get(), "Could not fetch announcements");
        }
    }

    if (ctx->detailWindow != 0) {
        window_manager_remove(ctx->detailWindow);
    }
    window_manager_remove(ctx->listWindow);
    app_event_unsubscribe(&sub);
    task_event_group_destruct(&event_group);
    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "Announcements",
    .name = "Announcements",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
    .flags = 0,
};

} // namespace tt::app::announcements
