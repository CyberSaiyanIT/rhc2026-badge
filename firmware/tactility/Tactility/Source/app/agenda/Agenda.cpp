#include <Tactility/file/File.h>
#include <Tactility/network/Http.h>
#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl.h>
#include <tactility/log.h>

#include <cJSON.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace tt::app::agenda {

constexpr auto* TAG = "Agenda";

namespace {

constexpr auto* SCHEDULE_URL = "https://cfp.romhack.io/romhack-camp-2026/schedule/export/schedule.json";
constexpr auto* SCHEDULE_PATH = "/data/schedule.json";
constexpr auto* SCHEDULE_DOWNLOAD_PATH = "/data/schedule.json.tmp";

/** Download runs on the main dispatcher; the parse and all LVGL work happen on the app's own task. */
enum class DownloadState {
    Idle,
    Busy,
    Succeeded,
    Failed
};

struct Event {
    std::string title;
    std::string start;
    std::string room;
    std::string track;
};

struct Day {
    std::string date;
    std::vector<Event> events;
};

/**
 * Outlives the app's task: the download callbacks run on the main dispatcher and can still fire
 * after the user closed the app, so both they and the app task hold a share of it.
 */
struct Context {
    uint32_t appInstanceId = 0;
    std::vector<Day> days;
    std::string statusText;
    lv_obj_t* scroll = nullptr;
    lv_timer_t* titleScrollTimer = nullptr;
    lv_obj_t* list = nullptr;
    std::atomic<bool> syncRequested { false };
    std::atomic<DownloadState> downloadState { DownloadState::Idle };
    /** Handed over by the download callback; parsed and written on the app's own task. */
    std::shared_ptr<std::vector<uint8_t>> downloaded;
};

/** Colour-codes the schedule by track, so a day's blocks are tellable apart at a glance. */
lv_color_t trackColor(const std::string& track) {
    if (track == "Entertainment") {
        return lv_color_hex(0x8A2BE2);
    } else if (track == "Journalism, society, hacker culture") {
        return lv_color_hex(0x90EE90);
    } else if (track == "Cybersecurity and Hacking") {
        return lv_color_hex(0x87CEEB);
    }
    return lv_color_hex(0x222222);
}

/**
 * Substitutes the few characters Montserrat has no glyph for and would draw as a box. Measured
 * against the generated font: these are the only gaps in the range it covers.
 */
std::string normalizeText(const char* text) {
    static constexpr struct { const char* from; char to; } SUBSTITUTIONS[] = {
        { "\u2011", '-' },  // non-breaking hyphen
        { "\u201b", '\'' }, // single high-reversed-9 quote
        { "\u201f", '"' },  // double high-reversed-9 quote
    };
    std::string out = text;
    for (const auto& substitution : SUBSTITUTIONS) {
        for (size_t at = out.find(substitution.from); at != std::string::npos;
             at = out.find(substitution.from, at + 1)) {
            out.replace(at, strlen(substitution.from), 1, substitution.to);
        }
    }
    return out;
}

const char* jsonString(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItem(object, key);
    return (item != nullptr && item->valuestring != nullptr) ? item->valuestring : "";
}

/**
 * Parses into caller-owned locals rather than into the Context: createWidgets can run on another
 * app's task while this app's own task is still parsing, and it reads Context::days.
 */
bool parseScheduleJson(const char* json, std::vector<Day>& outDays, std::string& outStatus) {
    outDays.clear();

    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        outStatus = "Error parsing schedule.";
        return false;
    }

    cJSON* days = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "schedule"), "conference"), "days");
    const int day_count = cJSON_GetArraySize(days);
    for (int i = 0; i < day_count; i++) {
        cJSON* day = cJSON_GetArrayItem(days, i);
        if (day == nullptr) continue;

        Day parsed_day;
        parsed_day.date = normalizeText(jsonString(day, "date"));
        if (parsed_day.date.empty()) {
            parsed_day.date = "Day";
        }

        cJSON* rooms = cJSON_GetObjectItem(day, "rooms");
        for (cJSON* room = (rooms != nullptr) ? rooms->child : nullptr; room != nullptr; room = room->next) {
            const int event_count = cJSON_GetArraySize(room);
            for (int j = 0; j < event_count; j++) {
                cJSON* event = cJSON_GetArrayItem(room, j);
                if (event == nullptr) continue;
                parsed_day.events.push_back(Event {
                    .title = normalizeText(jsonString(event, "title")),
                    .start = normalizeText(jsonString(event, "start")),
                    .room = normalizeText((room->string != nullptr) ? room->string : ""),
                    .track = jsonString(event, "track")
                });
            }
        }

        std::ranges::sort(parsed_day.events, [](const Event& a, const Event& b) {
            return a.start < b.start;
        });
        outDays.push_back(std::move(parsed_day));
    }

    cJSON_Delete(root);

    outStatus = outDays.empty() ? "Schedule is empty." : "";
    return true;
}

bool parseSchedule(const char* path, std::vector<Day>& outDays, std::string& outStatus) {
    auto json = file::readString(path);
    if (!json) {
        outDays.clear();
        outStatus = "No schedule downloaded. Press Sync.";
        return false;
    }
    return parseScheduleJson(reinterpret_cast<const char*>(json.get()), outDays, outStatus);
}

void appendJsonString(std::string& out, const std::string& value) {
    out += '"';
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escape[7];
                    snprintf(escape, sizeof(escape), "\\u%04x", c);
                    out += escape;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

/**
 * Re-emits only the fields this app reads, in the shape parseScheduleJson() understands, so the
 * stored copy needs no second parser and the rest costs no writes to the wear-levelled partition.
 */
std::string serializeSchedule(const std::vector<Day>& days) {
    std::string out = R"({"schedule":{"conference":{"days":[)";
    for (size_t d = 0; d < days.size(); d++) {
        if (d > 0) out += ',';
        out += R"({"date":)";
        appendJsonString(out, days[d].date);
        out += R"(,"rooms":{)";

        // Events are grouped back under their room, which is where the room name lives in this shape.
        std::vector<std::string> rooms;
        for (const auto& event : days[d].events) {
            if (std::ranges::find(rooms, event.room) == rooms.end()) {
                rooms.push_back(event.room);
            }
        }
        for (size_t r = 0; r < rooms.size(); r++) {
            if (r > 0) out += ',';
            appendJsonString(out, rooms[r]);
            out += ":[";
            bool first = true;
            for (const auto& event : days[d].events) {
                if (event.room != rooms[r]) continue;
                if (!first) out += ',';
                first = false;
                out += R"({"title":)";
                appendJsonString(out, event.title);
                out += R"(,"start":)";
                appendJsonString(out, event.start);
                out += R"(,"track":)";
                appendJsonString(out, event.track);
                out += '}';
            }
            out += ']';
        }
        out += "}}";
    }
    out += "]}}}";
    return out;
}

void addListText(lv_obj_t* parent, const char* text, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_DEFAULT), LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_all(label, 8, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

void addEvent(lv_obj_t* parent, const Event& event) {
    lv_obj_t* item = lv_button_create(parent);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(item, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(item, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(item, trackColor(event.track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, LV_OPA_40, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(item);
    lv_obj_set_style_text_font(title, lvgl_get_text_font(FONT_SIZE_DEFAULT), LV_PART_MAIN);
    lv_label_set_text(title, event.title.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title, LV_PCT(100));

    lv_obj_t* subtitle = lv_label_create(item);
    lv_obj_set_style_text_font(subtitle, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
    lv_label_set_text(subtitle, (event.start + " | " + event.room).c_str());
    lv_obj_set_style_text_opa(subtitle, LV_OPA_70, LV_PART_MAIN);
}

/** Rebuilds the list from what's already parsed. Caller holds the LVGL lock. */
void renderSchedule(Context* ctx) {
    if (ctx->list == nullptr) {
        return;
    }
    lv_obj_clean(ctx->list);

    if (!ctx->statusText.empty()) {
        addListText(ctx->list, ctx->statusText.c_str(), lv_color_hex(0x888888));
    }

    for (const auto& day : ctx->days) {
        addListText(ctx->list, day.date.c_str(), lv_color_hex(0x888888));
        for (const auto& event : day.events) {
            addEvent(ctx->list, event);
        }
    }
}

void onSyncPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->syncRequested = true;
}

/** The window manager deletes the widget tree whenever this window is buried, while main() keeps running. */
void cancelTitleScrollTimer(Context* ctx);

void onWidgetsDeleted(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    cancelTitleScrollTimer(ctx);
    ctx->list = nullptr;
    ctx->scroll = nullptr;
}

/**
 * Titles too long to fit scroll themselves, which fights the list scrolling. Clipping while the
 * list moves parks them at their start; circular scrolling resumes once it settles.
 */
void setTitlesScrolling(Context* ctx, bool scrolling) {
    if (ctx->list == nullptr) {
        return;
    }
    const auto mode = scrolling ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
    for (uint32_t i = 0; i < lv_obj_get_child_count(ctx->list); i++) {
        lv_obj_t* item = lv_obj_get_child(ctx->list, i);
        // Day headings are plain labels; only the event buttons hold a title label to scroll.
        if (lv_obj_get_child_count(item) == 0) {
            continue;
        }
        lv_obj_t* title = lv_obj_get_child(item, 0);
        if (title != nullptr && lv_obj_check_type(title, &lv_label_class)) {
            lv_label_set_long_mode(title, mode);
        }
    }
}

/** Long enough that a flick-and-read does not start two animations at once. */
constexpr uint32_t TITLE_SCROLL_RESUME_MS = 2000;

void cancelTitleScrollTimer(Context* ctx) {
    if (ctx->titleScrollTimer != nullptr) {
        lv_timer_delete(ctx->titleScrollTimer);
        ctx->titleScrollTimer = nullptr;
    }
}

void onTitleScrollTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    ctx->titleScrollTimer = nullptr; // one-shot: LVGL deletes it once this returns
    setTitlesScrolling(ctx, true);
}

void onScrollBegin(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    cancelTitleScrollTimer(ctx);
    setTitlesScrolling(ctx, false);
}

/** Each burst of scrolling restarts the wait, so continuous scrolling never resumes mid-flick. */
void onScrollEnd(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    cancelTitleScrollTimer(ctx);
    ctx->titleScrollTimer = lv_timer_create(onTitleScrollTimer, TITLE_SCROLL_RESUME_MS, ctx);
    lv_timer_set_repeat_count(ctx->titleScrollTimer, 1);
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, LV_STATE_DEFAULT);

    // The toolbar scrolls with the content rather than staying pinned, so a small screen gives the
    // schedule its full height once the user has started reading.
    ctx->scroll = lv_obj_create(parent);
    lv_obj_set_size(ctx->scroll, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ctx->scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ctx->scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->scroll, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(ctx->scroll, onScrollBegin, LV_EVENT_SCROLL_BEGIN, ctx);
    lv_obj_add_event_cb(ctx->scroll, onScrollEnd, LV_EVENT_SCROLL_END, ctx);

    lv_obj_t* toolbar = lvgl_toolbar_create(ctx->scroll, "Schedule");
    lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_REFRESH, onSyncPressed, ctx);

    ctx->list = lv_obj_create(ctx->scroll);
    lv_obj_set_width(ctx->list, LV_PCT(100));
    lv_obj_set_height(ctx->list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ctx->list, onWidgetsDeleted, LV_EVENT_DELETE, ctx);

    renderSchedule(ctx);

    // Titles are built clipped; the same delay that follows a scroll also covers the first draw.
    cancelTitleScrollTimer(ctx);
    ctx->titleScrollTimer = lv_timer_create(onTitleScrollTimer, TITLE_SCROLL_RESUME_MS, ctx);
    lv_timer_set_repeat_count(ctx->titleScrollTimer, 1);
}

void setStatus(Context* ctx, const char* text) {
    lvgl_lock();
    ctx->statusText = text;
    renderSchedule(ctx);
    lvgl_unlock();
}

void startDownload(const std::shared_ptr<Context>& ctx) {
    ctx->downloadState = DownloadState::Busy;
    setStatus(ctx.get(), "Downloading...");

    // To memory, not to a file: the published schedule is several times the size of the part this
    // app keeps, and the wear-levelled data partition is slow enough that the difference dominates.
    network::http::downloadToMemory(
        SCHEDULE_URL,
        "",
        [ctx](std::shared_ptr<std::vector<uint8_t>> data) {
            ctx->downloaded = std::move(data);
            ctx->downloadState = DownloadState::Succeeded;
        },
        [ctx](const char* errorMessage) {
            LOG_E(TAG, "Download error: %s", errorMessage);
            ctx->downloadState = DownloadState::Failed;
        }
    );
}

/** Writes in one call: FATFS erases before every write, so many small ones cost far more. */
bool writeSchedule(const std::string& json) {
    auto* file = fopen(SCHEDULE_DOWNLOAD_PATH, "wb");
    if (file == nullptr) {
        return false;
    }
    const bool written = fwrite(json.data(), 1, json.size(), file) == json.size();
    fclose(file);
    if (!written) {
        remove(SCHEDULE_DOWNLOAD_PATH);
        return false;
    }
    // FatFs rename() fails with EEXIST rather than replacing.
    remove(SCHEDULE_PATH);
    if (rename(SCHEDULE_DOWNLOAD_PATH, SCHEDULE_PATH) != 0) {
        LOG_E(TAG, "Failed to install %s", SCHEDULE_PATH);
        return false;
    }
    return true;
}

/** Parses what was downloaded and stores only the fields this app reads. */
void applyDownload(Context* ctx) {
    std::vector<Day> days;
    std::string status;

    auto data = std::move(ctx->downloaded);
    if (data == nullptr) {
        status = "Download produced nothing.";
    } else {
        data->push_back(0); // cJSON needs a terminator; the body is not one
        if (parseScheduleJson(reinterpret_cast<const char*>(data->data()), days, status)) {
            const std::string stored = serializeSchedule(days);
            LOG_I(TAG, "Storing %u of %u bytes", (unsigned) stored.size(), (unsigned) data->size() - 1);
            data.reset(); // freed before the write, so both copies never sit in PSRAM at once
            if (!writeSchedule(stored)) {
                status = "Failed to store schedule.";
            } else if (status.empty()) {
                status = LV_SYMBOL_OK " Sync successful!";
            }
        } else {
            // Nothing was stored, so the installed copy is still the last good one.
            std::string ignored;
            parseSchedule(SCHEDULE_PATH, days, ignored);
        }
    }

    lvgl_lock();
    ctx->days = std::move(days);
    ctx->statusText = std::move(status);
    renderSchedule(ctx);
    lvgl_unlock();
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    auto ctx = std::make_shared<Context>();
    ctx->appInstanceId = appInstanceId;

    // Safe unlocked: no window exists yet, so createWidgets cannot run on another task.
    parseSchedule(SCHEDULE_PATH, ctx->days, ctx->statusText);

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    app_event_subscribe(&sub, &event_group);

    WindowId window = window_manager_create(appInstanceId, createWidgets, ctx.get());

    bool shouldClose = false;
    while (!shouldClose) {
        // No dedicated wake-up bit exists for syncRequested/downloadState (set from LVGL's
        // button handler and the download's dispatcher-thread callbacks), so poll for those.
        task_event_group_wait_any(&event_group, nullptr, pdMS_TO_TICKS(100));

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

        if (ctx->syncRequested.exchange(false) && ctx->downloadState == DownloadState::Idle) {
            startDownload(ctx);
        } else if (ctx->downloadState == DownloadState::Succeeded) {
            ctx->downloadState = DownloadState::Idle;
            applyDownload(ctx.get());
        } else if (ctx->downloadState == DownloadState::Failed) {
            ctx->downloadState = DownloadState::Idle;
            ctx->downloaded.reset();
            setStatus(ctx.get(), "Download failed");
        }
    }

    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    task_event_group_destruct(&event_group);
    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "Agenda",
    .name = "Schedule",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
    .flags = 0,
};

} // namespace tt::app::agenda
