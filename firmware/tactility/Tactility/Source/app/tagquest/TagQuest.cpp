#include "TagQuestSettings.h"

#include <Tactility/network/Http.h>
#include <Tactility/service/wifi/Wifi.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <lvgl/devices/keyboard.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/log.h>

#include <cJSON.h>

#include <cctype>

#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>

#include <atomic>
#include <memory>
#include <format>
#include <string>
#include <vector>

#ifdef ESP_PLATFORM
#include <drivers/mfrc522.h>
#endif

namespace tt::app::tagquest {

extern const ::AppManifest manifest;

namespace {

constexpr auto* TAG = "TagQuest";

constexpr auto* API_URL = "https://romhack.io/badge-quiz.php";

constexpr int POLL_INTERVAL_MS = 100;
constexpr int SCAN_INTERVAL_MS = 200;
constexpr int WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr int ANSWER_TIME_MS = 30000;
constexpr int SUBMIT_GRACE_MS = 10000;
constexpr int RESULT_DISPLAY_MS = 4000;
/** How long "out of time" stays up, counting down, before the badge goes back to scanning. */
constexpr int TIMEOUT_CLOSING_MS = 10000;

constexpr size_t ANSWER_COUNT = 4;

constexpr uint32_t COLOR_BACKGROUND = 0x000000;
constexpr uint32_t COLOR_ACCENT = 0xE80B60;
constexpr uint32_t COLOR_DIM = 0x53707E;
/** For the closing seconds and a wrong answer - the only two things worth pulling the eye. */
constexpr uint32_t COLOR_ALERT = 0xE8542C;

constexpr auto* QUEST_LOGO_ASSET = "A:/system/flame_question_icon.png";
constexpr lv_coord_t QUEST_LOGO_WIDTH = 96;
constexpr lv_coord_t QUEST_LOGO_HEIGHT = 96;

/** Seconds left at which the countdown starts reading as a warning. */
constexpr int COUNTDOWN_ALERT_MS = 10000;

lv_style_t styleScreen;
lv_style_t styleButton;
lv_style_t styleButtonFocused;
bool stylesReady = false;

void initStyles() {
    if (stylesReady) return;
    stylesReady = true;

    lv_style_init(&styleScreen);
    lv_style_set_bg_color(&styleScreen, lv_color_hex(COLOR_BACKGROUND));
    lv_style_set_bg_opa(&styleScreen, LV_OPA_COVER);
    lv_style_set_border_width(&styleScreen, 0);
    lv_style_set_radius(&styleScreen, 0);
    lv_style_set_text_color(&styleScreen, lv_color_hex(COLOR_ACCENT));

    lv_style_init(&styleButton);
    lv_style_set_bg_opa(&styleButton, LV_OPA_TRANSP);
    lv_style_set_border_width(&styleButton, 2);
    lv_style_set_border_color(&styleButton, lv_color_hex(COLOR_ACCENT));
    lv_style_set_radius(&styleButton, 2);
    lv_style_set_text_color(&styleButton, lv_color_hex(COLOR_ACCENT));
    lv_style_set_pad_hor(&styleButton, 8);
    lv_style_set_pad_ver(&styleButton, 6);
    lv_style_set_shadow_width(&styleButton, 0);

    lv_style_init(&styleButtonFocused);
    lv_style_set_bg_opa(&styleButtonFocused, LV_OPA_COVER);
    lv_style_set_bg_color(&styleButtonFocused, lv_color_hex(COLOR_ACCENT));
    lv_style_set_text_color(&styleButtonFocused, lv_color_hex(COLOR_BACKGROUND));
    lv_style_set_outline_width(&styleButtonFocused, 0);
}

enum class State {
    NeedsUsername,
    Scanning,
    WaitingForWifi,
    Fetching,
    Question,
    Submitting,
    TimedOut,
    Result
};

enum class RequestState {
    Idle,
    Busy,
    Succeeded,
    Failed
};

struct Question {
    std::string text;
    std::vector<std::string> answers;
};

struct Context {
    uint32_t appInstanceId = 0;
    Settings settings;

    State state = State::Scanning;
    /** Owned by main(), which is the only thing allowed to change state. */
    int stateElapsedMs = 0;
    int scanElapsedMs = 0;

    struct Device* reader = nullptr;
    std::string tagId;
    Question question;
    std::string resultText;
    uint32_t resultColor = COLOR_ACCENT;
    std::string statusText;

    std::string pendingUsername;
    std::atomic<bool> usernameSubmitted { false };

    std::atomic<RequestState> requestState { RequestState::Idle };
    std::atomic<int> pendingAnswer { -1 };
    std::atomic<bool> pendingClose { false };
    /** Written by the HTTP callback, read by main() only once requestState leaves Busy. */
    network::http::Response response;
    std::string requestError;

    WindowId window = 0;
    lv_obj_t* body = nullptr;
    lv_obj_t* countdownLabel = nullptr;
    lv_obj_t* usernameInput = nullptr;
};

// region Reader

/**
 * Powers the reader up.
 * The rail is left off at boot and dropped again on every tag, so this runs on each return to
 * scanning and has to tolerate the power manager refusing it on a flat battery.
 */
bool startReader(Context* ctx) {
#ifdef ESP_PLATFORM
    if (ctx->reader == nullptr) {
        if (device_get_first_by_compatible("nxp,mfrc522", &ctx->reader) != ERROR_NONE) {
            LOG_E(TAG, "No RFID reader present");
            ctx->reader = nullptr;
            return false;
        }
    }

    if (!device_is_ready(ctx->reader)) {
        if (device_start(ctx->reader) != ERROR_NONE) {
            LOG_E(TAG, "Failed to power on the reader");
            device_put(ctx->reader);
            ctx->reader = nullptr;
            return false;
        }
    }
    return true;
#else
    (void) ctx;
    return false;
#endif
}

/** Drops the RF field entirely, which is what the badge wants between questions. */
void stopReader(Context* ctx) {
#ifdef ESP_PLATFORM
    if (ctx->reader == nullptr) {
        return;
    }
    if (device_is_ready(ctx->reader)) {
        device_stop(ctx->reader);
    }
    device_put(ctx->reader);
    ctx->reader = nullptr;
#else
    (void) ctx;
#endif
}

bool readTag(Context* ctx, std::string& outTagId, bool& outTagPresent) {
    outTagPresent = false;
#ifdef ESP_PLATFORM
    if (ctx->reader == nullptr) {
        return false;
    }
    char text[128];
    if (!mfrc522_read_ndef_text(ctx->reader, text, sizeof(text), &outTagPresent)) {
        return false;
    }
    outTagId = text;
    const auto first = outTagId.find_first_not_of(" \t\r\n");
    const auto last = outTagId.find_last_not_of(" \t\r\n");
    outTagId = (first == std::string::npos) ? "" : outTagId.substr(first, last - first + 1);
    return !outTagId.empty();
#else
    (void) ctx;
    (void) outTagId;
    return false;
#endif
}

// endregion

// region Parsing

/**
 * Whether the text decoded from a tag is a UUID4, which is what the API accepts as a tag id.
 * Mirrors the server's own validation so a mis-written tag is reported on the badge rather than
 * after a round trip.
 */
bool isValidTagId(const std::string& tagId) {
    // 8-4-4-4-12 hex digits, with the version nibble fixed at 4 and the variant one of 8/9/a/b.
    if (tagId.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < tagId.size(); i++) {
        const char c = tagId[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    if (tagId[14] != '4') {
        return false;
    }
    const char variant = static_cast<char>(std::tolower(static_cast<unsigned char>(tagId[19])));
    return variant == '8' || variant == '9' || variant == 'a' || variant == 'b';
}

bool parseQuestion(const std::vector<uint8_t>& body, Question& out, std::string& outError) {
    std::string json(body.begin(), body.end());
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        outError = "Bad response from server";
        return false;
    }

    bool ok = false;
    const cJSON* text = cJSON_GetObjectItem(root, "questionText");
    const cJSON* answers = cJSON_GetObjectItem(root, "possibleAnswers");

    if (cJSON_IsString(text) && text->valuestring != nullptr &&
        cJSON_IsArray(answers) && cJSON_GetArraySize(answers) == (int) ANSWER_COUNT) {
        out.text = text->valuestring;
        out.answers.clear();
        ok = true;
        for (size_t i = 0; i < ANSWER_COUNT; i++) {
            const cJSON* answer = cJSON_GetArrayItem(answers, (int) i);
            if (!cJSON_IsString(answer) || answer->valuestring == nullptr) {
                ok = false;
                break;
            }
            out.answers.emplace_back(answer->valuestring);
        }
    }

    if (!ok) {
        // An error field is the server's own explanation and beats a generic message.
        const cJSON* error = cJSON_GetObjectItem(root, "error");
        outError = (cJSON_IsString(error) && error->valuestring != nullptr)
            ? error->valuestring
            : "Unexpected question format";
    }

    cJSON_Delete(root);
    return ok;
}

std::string parseAnswerResult(const std::vector<uint8_t>& body, uint32_t& outColor) {
    outColor = COLOR_ALERT;
    std::string json(body.begin(), body.end());
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return "Bad response from server";
    }

    std::string text = "Unexpected answer response";
    outColor = COLOR_DIM;
    const cJSON* result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsString(result) && result->valuestring != nullptr) {
        const std::string value = result->valuestring;
        if (value == "correct") {
            text = "<CORRECT>";
            outColor = COLOR_ACCENT;
        } else if (value == "incorrect") {
            text = "<INCORRECT>";
            outColor = COLOR_ALERT;
        } else if (value == "already_answered") {
            text = "<ANSWER ALREADY GIVEN>";
            outColor = COLOR_DIM;
        }

        // The score is what the leaderboard actually moves on, so it is worth showing when the
        // answer earned anything.
        const cJSON* points = cJSON_GetObjectItem(root, "pointsAwarded");
        if (cJSON_IsNumber(points) && points->valueint > 0) {
            text += "\n+" + std::to_string(points->valueint);
        }
    } else {
        const cJSON* error = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsString(error) && error->valuestring != nullptr) {
            text = error->valuestring;
        }
    }

    cJSON_Delete(root);
    return text;
}

// endregion

// region Requests

/**
 * Nothing here is escaped because the values cannot need it: the tag id is a UUID read from an
 * NDEF record and the username is validated as alphanumeric before it is ever stored.
 */
std::string questionUrl(const std::string& tagId) {
    return std::string(API_URL) + "?question=" + tagId;
}

std::string answerUrl(const Context* ctx, int answerIndex) {
    // The API numbers answers from 1, while the buttons are indexed from 0.
    return questionUrl(ctx->tagId) +
        "&badge-id=" + ctx->settings.badgeId +
        "&nickname=" + ctx->settings.username +
        "&answer-index=" + std::to_string(answerIndex + 1);
}

/**
 * The callbacks run on the dispatcher rather than this app's task, so they only hand the response
 * over; main() is what acts on it.
 */
void sendRequest(const std::shared_ptr<Context>& ctx, const std::string& url, network::http::Method method) {
    ctx->requestState = RequestState::Busy;
    network::http::request(
        url,
        method,
        "",
        "",
        "",
        [ctx](const network::http::Response& response) {
            ctx->response = response;
            ctx->requestState = RequestState::Succeeded;
        },
        [ctx](const char* errorMessage) {
            LOG_E(TAG, "Request failed: %s", errorMessage);
            ctx->requestError = errorMessage;
            ctx->requestState = RequestState::Failed;
        }
    );
}

/** The radio is normally off to save power, so the service's auto-connect is nudged awake. */
void startWifi(Context* ctx) {
    const auto radio = service::wifi::getRadioState();
    if (radio == service::wifi::RadioState::Off || radio == service::wifi::RadioState::OffPending) {
        service::wifi::setEnabled(true);
    }
}

// endregion

// region Views

void renderBody(Context* ctx);

void onAnswerPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    const auto index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    // Only recorded here; main() decides whether the app is still in a state that accepts it.
    ctx->pendingAnswer.store(index);
}

void onBodyDeleted(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->body = nullptr;
    ctx->countdownLabel = nullptr;
    ctx->usernameInput = nullptr;
}

void detachUsernameKeyboard(lv_obj_t* textarea) {
    auto* keyboard = lvgl_software_keyboard_get_last();
    if (keyboard == nullptr || keyboard->object == nullptr ||
        lv_keyboard_get_textarea(keyboard->object) != textarea) {
        return;
    }

    if (auto* group = lv_obj_get_group(textarea); group != nullptr) {
        lv_group_set_editing(group, false);
    }
    lvgl_software_keyboard_hide(keyboard);
    lv_group_remove_obj(keyboard->object);
    lv_keyboard_set_textarea(keyboard->object, nullptr);
}

void onUsernameInputClosed(lv_event_t* event) {
    detachUsernameKeyboard(lv_event_get_current_target_obj(event));
}

void onUsernameAccepted(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (ctx->usernameInput == nullptr) {
        return;
    }
    const char* text = lv_textarea_get_text(ctx->usernameInput);
    if (text != nullptr) {
        ctx->pendingUsername = text;
        ctx->usernameSubmitted.store(true);
    }
}

lv_obj_t* createMessageLabel(lv_obj_t* parent, const std::string& text, enum LvglFontSize size, uint32_t color = COLOR_ACCENT) {
    auto* label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, lvgl_get_text_font(size), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text.c_str());
    return label;
}

void renderScanning(Context* ctx) {
    auto* mission = createMessageLabel(ctx->body, "YOUR NEXT MISSION", FONT_SIZE_LARGE);
    lv_obj_set_style_text_align(mission, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(mission, 4, 0);

    auto* logoWrapper = lv_obj_create(ctx->body);
    lv_obj_remove_style_all(logoWrapper);
    lv_obj_set_size(logoWrapper, LV_PCT(100), QUEST_LOGO_HEIGHT);
    lv_obj_set_flex_flow(logoWrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(logoWrapper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_ver(logoWrapper, 6, 0);
    lv_obj_remove_flag(logoWrapper, LV_OBJ_FLAG_SCROLLABLE);

    auto* logo = lv_image_create(logoWrapper);
    lv_image_set_src(logo, QUEST_LOGO_ASSET);
    lv_obj_set_size(logo, QUEST_LOGO_WIDTH, QUEST_LOGO_HEIGHT);
    lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_STRETCH);

    auto* headline = createMessageLabel(ctx->body, "FIND THIS LOGO\nAROUND THE CAMP", FONT_SIZE_DEFAULT);
    lv_obj_set_style_text_align(headline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(headline, 4, 0);

    auto* status = createMessageLabel(ctx->body, ctx->statusText, FONT_SIZE_SMALL, COLOR_DIM);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);

    auto* player = createMessageLabel(ctx->body, "PLAYER // " + ctx->settings.username, FONT_SIZE_SMALL, COLOR_ACCENT);
    lv_obj_set_style_text_align(player, LV_TEXT_ALIGN_CENTER, 0);
}

/**
 * Kept to the top of the screen on purpose: the software keyboard covers the lower half of the
 * display, and a field placed any further down is hidden the moment it is focused.
 */
void renderUsernameEntry(Context* ctx) {
    createMessageLabel(ctx->body, "Cyber Trivia", FONT_SIZE_LARGE);
    createMessageLabel(ctx->body, "What's your name?", FONT_SIZE_SMALL, COLOR_DIM);

    ctx->usernameInput = lv_textarea_create(ctx->body);
    lv_textarea_set_one_line(ctx->usernameInput, true);
    lv_textarea_set_max_length(ctx->usernameInput, USERNAME_MAX_LENGTH);
    lv_textarea_set_placeholder_text(ctx->usernameInput, "Letters and digits");
    lv_obj_set_width(ctx->usernameInput, LV_PCT(100));
    if (!ctx->settings.username.empty()) {
        lv_textarea_set_text(ctx->usernameInput, ctx->settings.username.c_str());
    }
    // lv_keyboard forwards its confirm key to the bound textarea as LV_EVENT_READY, so the
    // keyboard's own tick accepts the name as well as the OK button below.
    lv_obj_add_event_cb(ctx->usernameInput, onUsernameInputClosed, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(ctx->usernameInput, onUsernameInputClosed, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(ctx->usernameInput, onUsernameAccepted, LV_EVENT_READY, ctx);
    // Focused on arrival so the first key press opens the keyboard rather than walking the ring.
    lv_group_focus_obj(ctx->usernameInput);

    auto* button = lv_button_create(ctx->body);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_style_margin_top(button, 4, 0);
    lv_obj_add_event_cb(button, onUsernameInputClosed, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(button, onUsernameAccepted, LV_EVENT_SHORT_CLICKED, ctx);

    auto* label = lv_label_create(button);
    lv_label_set_text(label, "OK");
    lv_obj_center(label);

    createMessageLabel(ctx->body, ctx->statusText, FONT_SIZE_SMALL);
}

void renderQuestion(Context* ctx) {
    auto* question = createMessageLabel(ctx->body, ctx->question.text, FONT_SIZE_DEFAULT);

    // Reachable by the focus ring, or PREV stops at the first answer and the question above it
    // can never be brought on screen.
    lv_obj_add_flag(question, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(lv_group_get_default(), question);
    // Only lv_button sets this for itself, so without it focus lands on the question but LVGL
    // never scrolls it into view - which is why scrolling up appeared to stop at the first answer.
    lv_obj_add_flag(question, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    // A label carries no focus styling of its own, so without this the focus ring appears to
    // vanish when it reaches the question.
    lv_obj_set_style_outline_width(question, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(question, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(question, lv_color_hex(COLOR_DIM), LV_STATE_FOCUSED);
    lv_group_focus_obj(question);

    for (size_t i = 0; i < ctx->question.answers.size(); i++) {
        // lv_button joins the default focus group by itself, which is what makes these reachable
        // with the badge's D-pad; a plain lv_obj would not be.
        auto* button = lv_button_create(ctx->body);
        lv_obj_remove_style_all(button);
        lv_obj_add_style(button, &styleButton, LV_STATE_DEFAULT);
        lv_obj_add_style(button, &styleButtonFocused, LV_STATE_FOCUSED);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(button, onAnswerPressed, LV_EVENT_SHORT_CLICKED, ctx);

        auto* label = lv_label_create(button);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, LV_PCT(100));
        // Numbered to match the answer-index the API expects, so a player reading out "three"
        // and the submitted value cannot disagree.
        lv_label_set_text(label, std::format("{}  {}", i + 1, ctx->question.answers[i]).c_str());
    }

    // Focusing reveals the label's nearest edge only, which on a question taller than the viewport
    // opens part-way down its own text. Pin it to the first line instead.
    lv_obj_scroll_to_y(ctx->body, 0, LV_ANIM_OFF);
}

/** Renders whatever state main() has already committed. It never decides anything itself. */
void renderBody(Context* ctx) {
    if (ctx->body == nullptr) {
        return;
    }
    lv_obj_clean(ctx->body);

    // The countdown is a sibling of the body rather than one of its children, so lv_obj_clean()
    // above does not touch it and it stays on screen whatever the body is scrolled to.
    if (ctx->countdownLabel != nullptr) {
        if (ctx->state == State::Question || ctx->state == State::TimedOut) {
            lv_obj_remove_flag(ctx->countdownLabel, LV_OBJ_FLAG_HIDDEN);
            // A rebuild during the grace period recreates this label empty, and nothing else
            // rewrites it until the state changes.
            if (ctx->state == State::TimedOut) {
                lv_label_set_text(ctx->countdownLabel, "Timing out soon");
                lv_obj_set_style_text_color(ctx->countdownLabel, lv_color_hex(COLOR_ALERT), 0);
            }
        } else {
            lv_obj_add_flag(ctx->countdownLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    switch (ctx->state) {
        case State::NeedsUsername:
            renderUsernameEntry(ctx);
            break;
        case State::Question:
        case State::TimedOut:
            renderQuestion(ctx);
            break;
        case State::Result:
            createMessageLabel(ctx->body, ctx->resultText, FONT_SIZE_LARGE, ctx->resultColor);
            break;
        case State::Fetching:
        case State::WaitingForWifi:
            createMessageLabel(ctx->body, ctx->statusText, FONT_SIZE_LARGE);
            break;
        case State::Scanning:
            renderScanning(ctx);
            break;
        default:
            createMessageLabel(ctx->body, ctx->statusText, FONT_SIZE_DEFAULT);
            break;
    }
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    initStyles();

    lv_obj_add_style(parent, &styleScreen, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    ctx->body = lv_obj_create(parent);
    // remove_style_all drops the theme's card background and border, which would otherwise sit as
    // a grey panel on the black field. It also takes the theme's scrollbar with it, which this
    // screen cannot do without: there is no toolbar and the question can run past the fold, so
    // the bar is the only cue that there is more to see.
    lv_obj_remove_style_all(ctx->body);
    lv_obj_add_style(ctx->body, &styleScreen, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ctx->body, lv_color_hex(COLOR_DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ctx->body, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(ctx->body, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(ctx->body, 2, LV_PART_SCROLLBAR);
    lv_obj_set_width(ctx->body, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->body, 1);
    lv_obj_set_flex_flow(ctx->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->body, 6, 0);
    lv_obj_set_style_pad_row(ctx->body, 4, 0);
    lv_obj_add_event_cb(ctx->body, onBodyDeleted, LV_EVENT_DELETE, ctx);

    // Outside the body and after it in the column, so the remaining time stays pinned to the
    // bottom of the screen while the question and its answers scroll independently.
    ctx->countdownLabel = lv_label_create(parent);
    lv_obj_set_style_text_font(ctx->countdownLabel, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_set_style_text_color(ctx->countdownLabel, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_align(ctx->countdownLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ctx->countdownLabel, LV_PCT(100));
    lv_obj_set_style_margin_all(ctx->countdownLabel, 4, 0);
    lv_label_set_text(ctx->countdownLabel, "");

    renderBody(ctx);
}

/** Widget work from the app's task has to hold the LVGL lock; createWidgets already holds it. */
void refresh(Context* ctx) {
    lvgl_lock();
    renderBody(ctx);
    lvgl_unlock();
}

/**
 * Opens the software keyboard on the name field and hands it keypad focus.
 *
 * Sent as a press rather than calling lvgl_software_keyboard_show() directly: only the module's
 * own LV_EVENT_PRESSED handler also moves the group into the keyboard and turns on editing, and
 * editing is what makes the badge's D-pad emit directions instead of walking the focus ring.
 * Without it the keyboard is either closed or on screen but unreachable.
 */
void openUsernameKeyboard(Context* ctx) {
    lvgl_lock();
    if (ctx->usernameInput != nullptr) {
        lv_obj_send_event(ctx->usernameInput, LV_EVENT_PRESSED, nullptr);
    }
    lvgl_unlock();
}

void setStatus(Context* ctx, const std::string& text) {
    ctx->statusText = text;
    refresh(ctx);
}

void updateCountdown(Context* ctx, int remainingMs) {
    lvgl_lock();
    if (ctx->countdownLabel != nullptr) {
        const int seconds = (remainingMs + 999) / 1000;
        lv_label_set_text_fmt(ctx->countdownLabel, "%d", seconds);
        const uint32_t color = (remainingMs <= COUNTDOWN_ALERT_MS) ? COLOR_ALERT : COLOR_ACCENT;
        lv_obj_set_style_text_color(ctx->countdownLabel, lv_color_hex(color), 0);
    }
    lvgl_unlock();
}

void updateTimeoutCountdown(Context* ctx, int remainingMs) {
    lvgl_lock();
    if (ctx->countdownLabel != nullptr) {
        const int seconds = (remainingMs + 999) / 1000;
        lv_label_set_text_fmt(ctx->countdownLabel, "LAST CHANCE %d", seconds);
        lv_obj_set_style_text_color(ctx->countdownLabel, lv_color_hex(COLOR_ALERT), 0);
    }
    lvgl_unlock();
}

void setCountdownText(Context* ctx, const char* text, uint32_t color) {
    lvgl_lock();
    if (ctx->countdownLabel != nullptr) {
        lv_label_set_text(ctx->countdownLabel, text);
        lv_obj_set_style_text_color(ctx->countdownLabel, lv_color_hex(color), 0);
    }
    lvgl_unlock();
}

// endregion

// region State machine

void enterState(Context* ctx, State state) {
    ctx->state = state;
    ctx->stateElapsedMs = 0;
}

void enterScanning(Context* ctx) {
    ctx->tagId.clear();
    ctx->pendingAnswer.store(-1);
    ctx->requestState = RequestState::Idle;
    enterState(ctx, State::Scanning);

    if (startReader(ctx)) {
        setStatus(ctx, "Scan the hidden tag when you find it");
    } else {
        setStatus(ctx, "Reader unavailable. Check the battery.");
    }
}

void enterResult(Context* ctx, const std::string& text, uint32_t color = COLOR_ALERT) {
    ctx->resultText = text;
    ctx->resultColor = color;
    enterState(ctx, State::Result);
    refresh(ctx);
}

/** A tag was read: the reader is dropped before anything slow happens, per the badge's design. */
void onTagFound(const std::shared_ptr<Context>& ctx, const std::string& tagId) {
    LOG_I(TAG, "Tag: %s", tagId.c_str());

    if (!isValidTagId(tagId)) {
        LOG_W(TAG, "Tag does not carry a UUID4");
        // The reader is deliberately left powered here, unlike the valid path: the driver halts a
        // tag after reading it, so it stays silent while the field is up. Cycling the rail would
        // reset it to answering again and the same bad tag would be read on a loop.
        enterResult(ctx.get(), "Invalid tag", COLOR_ALERT);
        return;
    }

    ctx->tagId = tagId;
    stopReader(ctx.get());

    if (service::wifi::getRadioState() == service::wifi::RadioState::ConnectionActive) {
        enterState(ctx.get(), State::Fetching);
        setStatus(ctx.get(), "Tag found!");
        sendRequest(ctx, questionUrl(tagId), network::http::Method::Get);
    } else {
        startWifi(ctx.get());
        enterState(ctx.get(), State::WaitingForWifi);
        setStatus(ctx.get(), "Tag found!\nConnecting...");
    }
}

void pollScanning(const std::shared_ptr<Context>& ctx) {
    ctx->scanElapsedMs += POLL_INTERVAL_MS;
    if (ctx->scanElapsedMs < SCAN_INTERVAL_MS) {
        return;
    }
    ctx->scanElapsedMs = 0;

    if (ctx->reader == nullptr) {
        // A refused power-on is retried, so a badge that was plugged in recovers on its own.
        if (!startReader(ctx.get())) {
            return;
        }
        setStatus(ctx.get(), "Scan the hidden tag when you find it");
    }

    std::string tagId;
    bool tagPresent = false;
    if (readTag(ctx.get(), tagId, tagPresent)) {
        onTagFound(ctx, tagId);
    } else if (tagPresent) {
        // A tag answered but carries no NDEF text - an unprogrammed sticker, or one written in a
        // format this app does not read. Worth naming, or it looks like the reader is dead.
        LOG_W(TAG, "Tag present but no NDEF text record");
        enterResult(ctx.get(), "Invalid tag", COLOR_ALERT);
    }
}

void pollWifi(const std::shared_ptr<Context>& ctx) {
    if (service::wifi::getRadioState() == service::wifi::RadioState::ConnectionActive) {
        enterState(ctx.get(), State::Fetching);
        setStatus(ctx.get(), "Tag found!");
        sendRequest(ctx, questionUrl(ctx->tagId), network::http::Method::Get);
        return;
    }
    if (ctx->stateElapsedMs >= WIFI_CONNECT_TIMEOUT_MS) {
        enterResult(ctx.get(), "No Wi-Fi connection");
    }
}

void pollFetching(const std::shared_ptr<Context>& ctx) {
    const auto state = ctx->requestState.load();
    if (state == RequestState::Busy) {
        return;
    }
    if (state == RequestState::Failed) {
        enterResult(ctx.get(), ctx->requestError.empty() ? "Network error" : ctx->requestError);
        return;
    }

    std::string error;
    if (parseQuestion(ctx->response.body, ctx->question, error)) {
        enterState(ctx.get(), State::Question);
        refresh(ctx.get());
        updateCountdown(ctx.get(), ANSWER_TIME_MS);
    } else {
        LOG_E(TAG, "Question rejected (HTTP %d): %s", ctx->response.statusCode, error.c_str());
        // The status is shown too: a redirect or a server error otherwise reaches the user as an
        // unexplained parse failure, and nobody at the venue can read the log.
        if (ctx->response.statusCode < 200 || ctx->response.statusCode >= 300) {
            error += " (" + std::to_string(ctx->response.statusCode) + ")";
        }
        enterResult(ctx.get(), error);
    }
    ctx->requestState = RequestState::Idle;
}

void pollQuestion(const std::shared_ptr<Context>& ctx) {
    const int answer = ctx->pendingAnswer.exchange(-1);
    if (answer >= 0) {
        // The countdown governs only the time to choose, so it stops here and the submission gets
        // its own grace period.
        enterState(ctx.get(), State::Submitting);
        setStatus(ctx.get(), "Submitting...");
        sendRequest(ctx, answerUrl(ctx.get(), answer), network::http::Method::Post);
        return;
    }

    const int remaining = ANSWER_TIME_MS - ctx->stateElapsedMs;
    if (remaining <= 0) {
        // Deliberately no refresh(): rebuilding the widgets here would drop the answer buttons the
        // user can still press, and reset their scroll position and focus.
        enterState(ctx.get(), State::TimedOut);
        setCountdownText(ctx.get(), "Timing out soon", COLOR_ALERT);
        return;
    }
    updateCountdown(ctx.get(), remaining);
}

/** The answering window has closed but a late answer still counts, so the buttons stay live. */
void pollTimedOut(const std::shared_ptr<Context>& ctx) {
    const int answer = ctx->pendingAnswer.exchange(-1);
    if (answer >= 0) {
        enterState(ctx.get(), State::Submitting);
        setStatus(ctx.get(), "Submitting...");
        sendRequest(ctx, answerUrl(ctx.get(), answer), network::http::Method::Post);
        return;
    }

    if (ctx->stateElapsedMs >= TIMEOUT_CLOSING_MS) {
        enterResult(ctx.get(), "Out of time");
        return;
    }

    updateTimeoutCountdown(ctx.get(), TIMEOUT_CLOSING_MS - ctx->stateElapsedMs);
}

void pollSubmitting(const std::shared_ptr<Context>& ctx) {
    const auto state = ctx->requestState.load();
    if (state == RequestState::Succeeded) {
        uint32_t color = COLOR_ACCENT;
        const auto text = parseAnswerResult(ctx->response.body, color);
        enterResult(ctx.get(), text, color);
        ctx->requestState = RequestState::Idle;
        return;
    }
    if (state == RequestState::Failed) {
        enterResult(ctx.get(), ctx->requestError.empty() ? "Network error" : ctx->requestError);
        ctx->requestState = RequestState::Idle;
        return;
    }
    if (ctx->stateElapsedMs >= SUBMIT_GRACE_MS) {
        // The server may still have recorded it, so this says nothing about correctness.
        enterResult(ctx.get(), "No answer from server");
    }
}

// endregion

int32_t appMain(int argc, char* argv[]) {
    const uint32_t appInstanceId = app_scheduler_current_app_id();
    auto ctx = std::make_shared<Context>();
    ctx->appInstanceId = appInstanceId;
    ctx->settings = loadOrCreate();

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    const bool needsUsername = ctx->settings.username.empty();
    ctx->state = needsUsername ? State::NeedsUsername : State::Scanning;
    ctx->statusText = needsUsername ? "" : "Scan the hidden tag when you find it";

    ctx->window = window_manager_create(appInstanceId, createWidgets, ctx.get());

    if (needsUsername) {
        openUsernameKeyboard(ctx.get());
    } else {
        startReader(ctx.get());
    }

    bool shouldClose = false;
    while (!shouldClose) {
        // The atomics set by the LVGL callbacks and the dispatcher's request callbacks have no
        // wake-up bit of their own, so the loop ticks rather than blocking indefinitely. This is
        // also what drives the countdown, which keeps it off a timer that would outlive the
        // widgets it writes to.
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

        ctx->stateElapsedMs += POLL_INTERVAL_MS;

        switch (ctx->state) {
            case State::NeedsUsername:
                if (ctx->usernameSubmitted.exchange(false)) {
                    if (isValidUsername(ctx->pendingUsername)) {
                        ctx->settings.username = ctx->pendingUsername;
                        save(ctx->settings);
                        enterScanning(ctx.get());
                    } else {
                        setStatus(ctx.get(), "Use 1-10 letters or digits");
                        openUsernameKeyboard(ctx.get());
                    }
                }
                break;
            case State::Scanning:
                pollScanning(ctx);
                break;
            case State::WaitingForWifi:
                pollWifi(ctx);
                break;
            case State::Fetching:
                pollFetching(ctx);
                break;
            case State::Question:
                pollQuestion(ctx);
                break;
            case State::Submitting:
                pollSubmitting(ctx);
                break;
            case State::TimedOut:
                pollTimedOut(ctx);
                break;
            case State::Result:
                if (ctx->stateElapsedMs >= RESULT_DISPLAY_MS) {
                    enterScanning(ctx.get());
                }
                break;
        }
    }

    stopReader(ctx.get());

    window_manager_remove(ctx->window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "TagQuest",
    .name = "Cyber Trivia",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
};

} // namespace tt::app::tagquest
