#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

#if defined(CONFIG_SOC_WIFI_SUPPORTED) || defined(CONFIG_SLAVE_SOC_WIFI_SUPPORTED)

#include <Tactility/app/pingpong/PingPongPrivate.h>
#include <Tactility/service/espnow/EspNow.h>
#include <Tactility/service/wifi/Wifi.h>
#include <Tactility/service/wifi/WifiApSettings.h>

#include <app/event.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>

#include <lvgl.h>

#include <tactility/check.h>
#include <tactility/delay.h>
#include <tactility/log.h>

#include <esp_mac.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace tt::app::pingpong {

namespace {

constexpr auto* TAG = "PingPong";
constexpr size_t MAX_INBOX = 32;
/** Faster key auto-repeat than LVGL's navigation defaults, so a held key drives the paddle. */
constexpr uint16_t GAME_LONG_PRESS_MS = 90;
constexpr uint16_t GAME_LONG_PRESS_REPEAT_MS = 30;
constexpr uint16_t DEFAULT_LONG_PRESS_MS = 400;
constexpr uint16_t DEFAULT_LONG_PRESS_REPEAT_MS = 100;

bool isFieldPhase(Phase phase) { return phase == Phase::Playing || phase == Phase::GameOver; }

// A game screen rather than a settings screen: black field, one accent colour, and focus shown
// by filling a button instead of the theme's outline, which is hard to see on the badge.
constexpr uint32_t COLOR_BACKGROUND = 0x000000;
constexpr uint32_t COLOR_ACCENT = 0x2CE8A0;
constexpr uint32_t COLOR_DIM = 0x53707E;

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
    lv_style_set_pad_hor(&styleButton, 10);
    lv_style_set_pad_ver(&styleButton, 6);
    lv_style_set_shadow_width(&styleButton, 0);

    lv_style_init(&styleButtonFocused);
    lv_style_set_bg_opa(&styleButtonFocused, LV_OPA_COVER);
    lv_style_set_bg_color(&styleButtonFocused, lv_color_hex(COLOR_ACCENT));
    lv_style_set_text_color(&styleButtonFocused, lv_color_hex(COLOR_BACKGROUND));
    lv_style_set_outline_width(&styleButtonFocused, 0);
}

lv_obj_t* createGameButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, Context* ctx) {
    auto* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &styleButton, LV_STATE_DEFAULT);
    lv_obj_add_style(button, &styleButtonFocused, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ctx);

    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

lv_obj_t* createGameLabel(lv_obj_t* parent, LvglFontSize size, uint32_t color) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(size), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

int16_t clampToField(int32_t value, int16_t margin) {
    if (value < margin) return margin;
    if (value > FIELD_SIZE - margin) return static_cast<int16_t>(FIELD_SIZE - margin);
    return static_cast<int16_t>(value);
}

void sendPacket(Context* ctx, Packet& packet, const MacAddress& destination) {
    packet.from = ctx->myIdentity;
    uint8_t buffer[MAX_PACKET_SIZE];
    size_t size = serialize(packet, buffer, sizeof(buffer));
    if (size == 0) {
        LOG_E(TAG, "Failed to serialize packet type %d", (int)packet.type);
        return;
    }
    service::espnow::send(destination.data(), buffer, size);
}

/** Lobby traffic is broadcast, so every badge in range can see it. */
void broadcastPacket(Context* ctx, Packet& packet) {
    sendPacket(ctx, packet, BROADCAST_ADDRESS);
}

/** Match traffic is unicast to the encrypted peer, so the WiFi MAC encrypts and acknowledges it. */
void sendToOpponent(Context* ctx, Packet& packet) {
    packet.to = ctx->peerIdentity;
    packet.session = ctx->session;
    sendPacket(ctx, packet, ctx->peerRadioAddress);
}

void onReceive(Context* ctx, const esp_now_recv_info_t* receiveInfo, const uint8_t* data, int length) {
    if (length <= 0 || receiveInfo == nullptr || receiveInfo->src_addr == nullptr) return;

    Context::Incoming incoming {};
    if (!deserialize(data, static_cast<size_t>(length), incoming.packet)) {
        return;
    }
    if (incoming.packet.to != BROADCAST_ADDRESS && incoming.packet.to != ctx->myIdentity) {
        return;
    }

    memcpy(incoming.radioAddress.data(), receiveInfo->src_addr, incoming.radioAddress.size());
    incoming.rssi = (receiveInfo->rx_ctrl != nullptr) ? receiveInfo->rx_ctrl->rssi : 0;

    auto lock = ctx->inboxMutex.asScopedLock();
    lock.lock();
    if (ctx->inbox.size() < MAX_INBOX) {
        ctx->inbox.push_back(incoming);
    }
}

/**
 * Leaves any access point for as long as the app runs.
 *
 * Two badges only meet if their radios are on the same channel, and an associated badge follows
 * its access point's channel rather than the one ESP-NOW asks for: two badges on different
 * networks cannot see each other at all, and one sharing the air with a busy network sees its
 * paddle updates queue behind that traffic. Disconnected, both land on the configured channel.
 *
 * @return the SSID to rejoin afterwards, empty when nothing was connected
 */
std::string leaveAccessPoint() {
    const auto state = service::wifi::getRadioState();
    if (state != service::wifi::RadioState::ConnectionActive && state != service::wifi::RadioState::ConnectionPending) {
        return "";
    }

    const std::string ssid = service::wifi::getConnectionTarget();
    // Without pausing the scan, the service's auto-connect would put the badge straight back
    // onto the network it was just taken off.
    service::wifi::setAutoScanPaused(true);
    service::wifi::disconnect();

    // ESP-NOW is only brought up once the radio has actually left, so it settles on the
    // configured channel rather than the access point's.
    for (int i = 0; i < 40; i++) {
        const auto current = service::wifi::getRadioState();
        if (current != service::wifi::RadioState::ConnectionActive && current != service::wifi::RadioState::ConnectionPending) {
            break;
        }
        delay_millis(50);
    }

    LOG_I(TAG, "Left access point '%s' for the duration of the app", ssid.c_str());
    return ssid;
}

void rejoinAccessPoint(const std::string& ssid) {
    service::wifi::setAutoScanPaused(false);
    if (ssid.empty()) {
        return;
    }

    service::wifi::settings::WifiApSettings apSettings;
    if (service::wifi::settings::load(ssid, apSettings)) {
        service::wifi::connect(apSettings, false);
    }
}

/**
 * ESP-NOW is carried by the WiFi radio, so the radio cannot be turned off for a match. What can
 * go is modem sleep: with power saving on, a badge that is associated with an access point parks
 * its receiver between beacons and delays whatever arrives in between, which shows up here as a
 * paddle position that reaches the host late.
 */
void setRadioLowLatency(bool lowLatency) {
    (void)esp_wifi_set_ps(lowLatency ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}

// region Encrypted peer

bool addEncryptedPeer(Context* ctx) {
    esp_now_peer_info_t peer {};
    memcpy(peer.peer_addr, ctx->peerRadioAddress.data(), ctx->peerRadioAddress.size());
    memcpy(peer.lmk, ctx->matchKey.data(), ctx->matchKey.size());
    peer.channel = 0; // Stay on whichever channel ESP-NOW is already using
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;

    if (!service::espnow::addPeer(peer)) {
        LOG_E(TAG, "Failed to add encrypted peer");
        return false;
    }
    ctx->encryptedPeerAdded = true;
    return true;
}

void removeEncryptedPeer(Context* ctx) {
    if (!ctx->encryptedPeerAdded) return;
    service::espnow::removePeer(ctx->peerRadioAddress.data());
    ctx->encryptedPeerAdded = false;
}

// endregion

// region Input

/** Tunes every keypad indev's auto-repeat, so paddle movement follows a held key closely. */
void setKeypadRepeat(bool fast) {
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev != nullptr) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_long_press_time(indev, fast ? GAME_LONG_PRESS_MS : DEFAULT_LONG_PRESS_MS);
            lv_indev_set_long_press_repeat_time(indev, fast ? GAME_LONG_PRESS_REPEAT_MS : DEFAULT_LONG_PRESS_REPEAT_MS);
        }
        indev = lv_indev_get_next(indev);
    }
}

void onFieldKey(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    uint32_t key = lv_event_get_key(event);

    switch (key) {
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            ctx->inputDirection = -1;
            ctx->inputTimeMs = lv_tick_get();
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            ctx->inputDirection = 1;
            ctx->inputTimeMs = lv_tick_get();
            break;
        case LV_KEY_ESC:
            ctx->leaveRequested = true;
            break;
        case LV_KEY_ENTER:
            ctx->confirmRequested = true;
            break;
        default:
            break;
    }
}

void onPeerClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* button = lv_event_get_target_obj(event);
    ctx->challengeRequest = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
}

void onAcceptClicked(lv_event_t* event) {
    static_cast<Context*>(lv_event_get_user_data(event))->confirmRequested = true;
}

void onDeclineClicked(lv_event_t* event) {
    static_cast<Context*>(lv_event_get_user_data(event))->declineRequested = true;
}

void onPlayAgainClicked(lv_event_t* event) {
    static_cast<Context*>(lv_event_get_user_data(event))->rematchRequested = true;
}

void onExitClicked(lv_event_t* event) {
    static_cast<Context*>(lv_event_get_user_data(event))->leaveRequested = true;
}

// endregion

// region UI

void onContentDeleted(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->widgets = Widgets {};
    ctx->renderedStatusText.clear();
    if (ctx->keypadTuned) {
        setKeypadRepeat(false);
        ctx->keypadTuned = false;
    }
}

lv_obj_t* createStatusLabel(lv_obj_t* parent) {
    auto* label = createGameLabel(parent, FONT_SIZE_SMALL, COLOR_DIM);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    // Without this the label keeps LVGL's "Text" placeholder whenever the status is empty,
    // because render() only writes a status that differs from the one it last wrote.
    lv_label_set_text(label, "");
    return label;
}

void buildLobby(Context* ctx) {
    auto* content = ctx->widgets.content;

    // The match screen forces edit mode for arrow keys; the lobby needs focus navigation back.
    lv_group_set_editing(lv_group_get_default(), false);

    auto* title = createGameLabel(content, FONT_SIZE_LARGE, COLOR_ACCENT);
    lv_label_set_text(title, "PING PONG");

    auto* name = createGameLabel(content, FONT_SIZE_SMALL, COLOR_DIM);
    // Labelled rather than bare: an unexplained word under the title reads as a leftover debug
    // string, not as who you are playing as.
    lv_label_set_text_fmt(name, "Player: %s", ctx->myName.c_str());

    auto* list = lv_obj_create(content);
    ctx->widgets.peerList = list;
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &styleScreen, LV_STATE_DEFAULT);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);

    ctx->widgets.statusLabel = createStatusLabel(content);
}

void buildConfirm(Context* ctx) {
    auto* content = ctx->widgets.content;

    lv_group_set_editing(lv_group_get_default(), false);
    // Buttons pushed below the fold would be unreachable, and an unanswered challenge is
    // indistinguishable from a refused one.
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    auto* question = createGameLabel(content, FONT_SIZE_DEFAULT, COLOR_ACCENT);
    lv_obj_set_width(question, LV_PCT(100));
    lv_label_set_long_mode(question, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(question, "%s challenges you", ctx->peerName.c_str());

    auto* code = createGameLabel(content, FONT_SIZE_LARGE, COLOR_ACCENT);
    lv_label_set_text_fmt(code, "CODE %04u", ctx->matchCode);

    auto* hint = createGameLabel(content, FONT_SIZE_SMALL, COLOR_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Same code on both badges?");

    auto* buttons = lv_obj_create(content);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);

    auto* accept = createGameButton(buttons, "Play", onAcceptClicked, ctx);
    createGameButton(buttons, "Decline", onDeclineClicked, ctx);
    lv_group_focus_obj(accept);

    ctx->widgets.statusLabel = createStatusLabel(content);
}

void buildField(Context* ctx) {
    auto* content = ctx->widgets.content;

    // The field runs to the edges of the window, so the match is played full screen.
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 0, 0);

    auto* field = lv_obj_create(content);
    ctx->widgets.field = field;
    lv_obj_remove_style_all(field);
    lv_obj_add_style(field, &styleScreen, LV_STATE_DEFAULT);
    lv_obj_set_width(field, LV_PCT(100));
    lv_obj_set_flex_grow(field, 1);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);

    // The field and its pieces need explicit colours: theme defaults would paint the paddles
    // and the ball in the same surface colour as the field they sit on.
    auto createBlock = [field](int radius) {
        auto* block = lv_obj_create(field);
        lv_obj_set_style_pad_all(block, 0, 0);
        lv_obj_set_style_border_width(block, 0, 0);
        lv_obj_set_style_radius(block, radius, 0);
        lv_obj_set_style_bg_color(block, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
        return block;
    };

    ctx->widgets.leftPaddle = createBlock(0);
    ctx->widgets.rightPaddle = createBlock(0);
    ctx->widgets.ball = createBlock(LV_RADIUS_CIRCLE);

    // The dashed centre line is what makes it read as a court rather than a form.
    for (int i = 0; i < NET_SEGMENTS; i++) {
        auto* segment = lv_obj_create(field);
        lv_obj_remove_style_all(segment);
        lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(segment, lv_color_hex(COLOR_DIM), 0);
        lv_obj_align(segment, LV_ALIGN_TOP_MID, 0, 0);
        ctx->widgets.net[i] = segment;
    }

    auto* score = createGameLabel(field, FONT_SIZE_LARGE, COLOR_ACCENT);
    ctx->widgets.scoreLabel = score;
    lv_obj_align(score, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(score, "0  0");

    // Both badges show the match code, so the challenger can check it against the one its
    // opponent was shown before accepting.
    auto* code = createGameLabel(field, FONT_SIZE_SMALL, COLOR_DIM);
    ctx->widgets.codeLabel = code;
    lv_obj_align(code, LV_ALIGN_TOP_LEFT, 3, 3);
    lv_label_set_text_fmt(code, "%04u", ctx->matchCode);

    ctx->widgets.statusLabel = createStatusLabel(content);

    if (ctx->phase == Phase::GameOver) {
        // The final score stays on the field, so the buttons are the whole result screen.
        lv_group_set_editing(lv_group_get_default(), false);

        auto* buttons = lv_obj_create(content);
        lv_obj_remove_style_all(buttons);
        lv_obj_add_style(buttons, &styleScreen, LV_STATE_DEFAULT);
        lv_obj_set_width(buttons, LV_PCT(100));
        lv_obj_set_height(buttons, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(buttons, 4, 0);
        lv_obj_set_style_pad_column(buttons, 8, 0);
        lv_obj_remove_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

        auto* again = createGameButton(buttons, "Play again", onPlayAgainClicked, ctx);
        createGameButton(buttons, "Exit", onExitClicked, ctx);
        lv_group_focus_obj(again);
        return;
    }

    // The badge keypad only reports arrow keys while its group is in edit mode, so claim
    // focus for the field and force edit mode for as long as the match is being played.
    lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(lv_group_get_default(), field);
    lv_group_focus_obj(field);
    lv_group_set_editing(lv_group_get_default(), true);
    lv_obj_add_event_cb(field, onFieldKey, LV_EVENT_KEY, ctx);

    setKeypadRepeat(true);
    ctx->keypadTuned = true;
}

/** Rebuilds the content area for the current phase. Requires the LVGL lock. */
void buildContent(Context* ctx) {
    if (ctx->widgets.content == nullptr) return;

    if (ctx->keypadTuned) {
        setKeypadRepeat(false);
        ctx->keypadTuned = false;
    }

    lv_obj_clean(ctx->widgets.content);
    auto* content = ctx->widgets.content;
    ctx->widgets = Widgets {};
    ctx->widgets.content = content;
    ctx->renderedStatusText.clear();

    switch (ctx->phase) {
        case Phase::Confirming: buildConfirm(ctx); break;
        case Phase::Playing:
        case Phase::GameOver: buildField(ctx); break;
        default: buildLobby(ctx); break;
    }

    ctx->widgets.built = true;
    ctx->widgets.builtPhase = ctx->phase;
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    initStyles();

    // No toolbar: the match uses the whole window, and the badge's back key reaches the window
    // manager through lvgl_toolbar_trigger_back() whether or not a toolbar object exists.
    lv_obj_add_style(parent, &styleScreen, LV_STATE_DEFAULT);

    auto* content = lv_obj_create(parent);
    ctx->widgets.content = content;
    lv_obj_remove_style_all(content);
    lv_obj_add_style(content, &styleScreen, LV_STATE_DEFAULT);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(content, onContentDeleted, LV_EVENT_DELETE, ctx);

    buildContent(ctx);
}

/** Shows the strongest MAX_LISTED_PEERS badges, keeping focus on the same badge across refreshes. */
void refreshPeerList(Context* ctx) {
    auto* list = ctx->widgets.peerList;
    if (list == nullptr) return;

    // Rebuilding destroys the focused button, and re-sorting moves rows out from under whoever
    // is navigating them. Both are avoided while the same badges are on screen: only the labels
    // are rewritten, in place, and the order is left alone until the set itself changes.
    std::vector<const Peer*> ranked;
    ranked.reserve(ctx->peers.size());
    for (const auto& peer : ctx->peers) {
        ranked.push_back(&peer);
    }
    std::sort(ranked.begin(), ranked.end(), [](const Peer* a, const Peer* b) { return a->rssi > b->rssi; });
    if (ranked.size() > MAX_LISTED_PEERS) {
        ranked.resize(MAX_LISTED_PEERS);
    }

    std::vector<MacAddress> selection;
    selection.reserve(ranked.size());
    for (const Peer* peer : ranked) {
        selection.push_back(peer->identity);
    }

    auto sortedCopy = [](std::vector<MacAddress> addresses) {
        std::sort(addresses.begin(), addresses.end());
        return addresses;
    };

    if (sortedCopy(selection) == sortedCopy(ctx->listedPeers)) {
        for (size_t i = 0; i < ctx->listedPeers.size(); i++) {
            auto* button = lv_obj_get_child(list, static_cast<int32_t>(i));
            if (button == nullptr) break;
            auto it = std::find_if(ctx->peers.begin(), ctx->peers.end(), [&](const Peer& peer) {
                return peer.identity == ctx->listedPeers[i];
            });
            if (it == ctx->peers.end()) continue;
            auto* label = lv_obj_get_child(button, 0);
            if (label != nullptr && lv_obj_check_type(label, &lv_label_class)) {
                lv_label_set_text_fmt(label, "%s   %d dBm", it->name.c_str(), (int)it->rssi);
            }
        }
        return;
    }

    MacAddress focused {};
    bool hadFocus = false;
    size_t focusedIndex = 0;
    lv_obj_t* focusedObject = lv_group_get_focused(lv_group_get_default());
    if (focusedObject != nullptr && lv_obj_get_parent(focusedObject) == list) {
        auto index = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(focusedObject)));
        if (index < ctx->listedPeers.size()) {
            focused = ctx->listedPeers[index];
            focusedIndex = index;
            hadFocus = true;
        }
    }

    lv_obj_clean(list);
    ctx->listedPeers = selection;

    if (ranked.empty()) {
        auto* empty = createGameLabel(list, FONT_SIZE_SMALL, COLOR_DIM);
        lv_label_set_text(empty, "Looking for players...");
        return;
    }

    lv_obj_t* focusTarget = nullptr;
    for (size_t i = 0; i < ranked.size(); i++) {
        const Peer& peer = *ranked[i];
        char text[40];
        snprintf(text, sizeof(text), "%s   %d dBm", peer.name.c_str(), (int)peer.rssi);

        auto* button = createGameButton(list, text, onPeerClicked, ctx);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_style_text_align(button, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(lv_obj_get_child(button, 0), LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        if (hadFocus && peer.identity == focused) {
            focusTarget = button;
        }
    }

    // Losing the focused badge must not drop focus out of the list: without a target here it
    // falls to whatever comes first in the shared group, which is the toolbar.
    if (hadFocus && focusTarget == nullptr) {
        const size_t fallback = std::min(focusedIndex, ranked.size() - 1);
        focusTarget = lv_obj_get_child(list, static_cast<int32_t>(fallback));
    }
    if (focusTarget != nullptr) {
        lv_group_focus_obj(focusTarget);
    }
}

/** Requires the LVGL lock. */
void render(Context* ctx) {
    if (ctx->widgets.statusLabel != nullptr && ctx->renderedStatusText != ctx->statusText) {
        ctx->renderedStatusText = ctx->statusText;
        lv_label_set_text(ctx->widgets.statusLabel, ctx->statusText.c_str());
    }

    if (ctx->widgets.field == nullptr) return;

    const int32_t width = lv_obj_get_content_width(ctx->widgets.field);
    const int32_t height = lv_obj_get_content_height(ctx->widgets.field);
    if (width <= 0 || height <= 0) return;

    auto toX = [width](int32_t unit) { return unit * width / FIELD_SIZE; };
    auto toY = [height](int32_t unit) { return unit * height / FIELD_SIZE; };

    const int32_t paddleWidth = std::max<int32_t>(4, width / 50);
    const int32_t paddleHeight = std::max<int32_t>(8, toY(PADDLE_HALF_HEIGHT * 2));
    const int32_t ballSize = std::max<int32_t>(6, toX(BALL_RADIUS * 2));

    const int32_t netWidth = std::max<int32_t>(2, width / 120);
    const int32_t netHeight = std::max<int32_t>(4, height / (NET_SEGMENTS * 2 + 1));
    for (int i = 0; i < NET_SEGMENTS; i++) {
        if (ctx->widgets.net[i] == nullptr) continue;
        lv_obj_set_size(ctx->widgets.net[i], netWidth, netHeight);
        lv_obj_set_pos(ctx->widgets.net[i], width / 2 - netWidth / 2, netHeight / 2 + i * netHeight * 2);
    }

    lv_obj_set_size(ctx->widgets.leftPaddle, paddleWidth, paddleHeight);
    lv_obj_set_size(ctx->widgets.rightPaddle, paddleWidth, paddleHeight);
    lv_obj_set_size(ctx->widgets.ball, ballSize, ballSize);

    lv_obj_set_pos(ctx->widgets.leftPaddle, toX(PADDLE_X_HOST) - paddleWidth / 2, toY(ctx->game.hostPaddleY) - paddleHeight / 2);
    lv_obj_set_pos(ctx->widgets.rightPaddle, toX(PADDLE_X_GUEST) - paddleWidth / 2, toY(ctx->game.guestPaddleY) - paddleHeight / 2);
    lv_obj_set_pos(ctx->widgets.ball, toX(ctx->game.ballX) - ballSize / 2, toY(ctx->game.ballY) - ballSize / 2);

    if (ctx->widgets.scoreLabel != nullptr) {
        lv_label_set_text_fmt(
            ctx->widgets.scoreLabel,
            "%d  %d",
            ctx->isHost ? ctx->game.hostScore : ctx->game.guestScore,
            ctx->isHost ? ctx->game.guestScore : ctx->game.hostScore
        );
    }
}

// endregion

// region Game

/**
 * Timestamps are always compared with lv_tick_elaps(), never against a tick sampled earlier in
 * the same pass: a phase entered part-way through a tick starts \later than that sample, and the
 * unsigned subtraction would wrap to roughly 4.3 billion and fire every timeout at once.
 *
 * \later Which is what made a challenge report "did not respond" and the badge it invited report
 *     "challenge declined", both instantly, without either dialog ever being shown.
 */
void setPhase(Context* ctx, Phase phase) {
    ctx->phase = phase;
    ctx->phaseStartMs = lv_tick_get();
}

void expirePeers(Context* ctx);

void returnToLobby(Context* ctx, const std::string& reason) {
    setRadioLowLatency(false);
    ctx->rematchPending = false;
    ctx->rematchRequested = false;
    removeEncryptedPeer(ctx);
    expirePeers(ctx);
    ctx->peerIdentity = MacAddress {};
    ctx->peerName.clear();
    ctx->statusText = reason;
    setPhase(ctx, Phase::Lobby);
}

void leaveMatch(Context* ctx, const std::string& reason, bool notifyPeer) {
    if (notifyPeer) {
        Packet bye {};
        bye.type = PacketType::Bye;
        sendToOpponent(ctx, bye);
    }
    returnToLobby(ctx, reason);
}

void serve(Context* ctx, bool towardsHost);

void startMatch(Context* ctx, bool asHost) {
    setRadioLowLatency(true);
    ctx->isHost = asHost;
    ctx->heardOpponent = false;
    ctx->lastHandshakeSendMs = 0;
    ctx->rematchRequested = false;
    ctx->rematchPending = false;
    ctx->game = GameState {};
    if (asHost) {
        // The first ball is served like any other, so it waits at the centre and costs nothing
        // if it is missed.
        serve(ctx, false);
    }
    ctx->localPaddleY = FIELD_SIZE / 2;
    ctx->lastPeerPacketMs = lv_tick_get();
    ctx->statusText = asHost ? "You are on the left" : "You are on the right";
    setPhase(ctx, Phase::Playing);
}

void serve(Context* ctx, bool towardsHost) {
    auto& game = ctx->game;
    game.ballX = FIELD_SIZE / 2;
    game.ballY = FIELD_SIZE / 2;
    // The ball waits at the centre before it sets off, so the player it is served to has time to
    // get behind it. Held still with zero velocity, the guest's dead reckoning waits with it.
    game.ballVx = 0;
    game.ballVy = 0;
    game.serveTowardsHost = towardsHost;
    game.serveDelayTicks = SERVE_DELAY_TICKS;
    game.rallyStarted = false;
}

/**
 * Advances the guest's copy of the ball between the host's updates. Only the position moves:
 * bounces, scoring and serving stay with the host, and its next state corrects any drift.
 */
void extrapolateBall(Context* ctx) {
    auto& game = ctx->game;
    // X is left free to run past a paddle, because that is a point being scored and the field
    // clips it; only the walls are predictable enough to hold the ball back.
    game.ballX = static_cast<int16_t>(std::clamp<int32_t>(game.ballX + game.ballVx, -FIELD_SIZE, FIELD_SIZE * 2));
    game.ballY = clampToField(game.ballY + game.ballVy, BALL_RADIUS);
}

/** Host-side physics for one tick. */
void simulate(Context* ctx) {
    auto& game = ctx->game;

    if (game.serveDelayTicks > 0) {
        game.serveDelayTicks--;
        if (game.serveDelayTicks == 0) {
            game.ballVx = game.serveTowardsHost ? static_cast<int16_t>(-BALL_SPEED_X) : BALL_SPEED_X;
        }
        return;
    }

    const int32_t previousX = game.ballX;
    const int32_t previousY = game.ballY;

    game.ballX = static_cast<int16_t>(game.ballX + game.ballVx);
    game.ballY = static_cast<int16_t>(game.ballY + game.ballVy);

    if (game.ballY < BALL_RADIUS) {
        game.ballY = BALL_RADIUS;
        game.ballVy = static_cast<int16_t>(-game.ballVy);
    } else if (game.ballY > FIELD_SIZE - BALL_RADIUS) {
        game.ballY = FIELD_SIZE - BALL_RADIUS;
        game.ballVy = static_cast<int16_t>(-game.ballVy);
    }

    // Where the ball met the paddle decides the angle it leaves at: off the end of the paddle it
    // kicks away, off the middle it comes straight back.
    auto deflect = [&game](int32_t contactY, int32_t paddleY, int16_t placeAt) {
        game.ballX = placeAt;
        game.ballY = clampToField(contactY, BALL_RADIUS);
        game.ballVx = static_cast<int16_t>(-game.ballVx);
        game.rallyStarted = true;
        int32_t spin = game.ballVy + (contactY - paddleY) / 6;
        game.ballVy = static_cast<int16_t>(std::clamp<int32_t>(spin, -BALL_SPEED_Y_MAX, BALL_SPEED_Y_MAX));
    };

    // The ball travels further in one tick than the paddle is thick, so the test is where its
    // leading edge crossed the paddle's plane during this step, not where it happens to sit
    // afterwards. Testing the landing position lets a fast ball step straight over the paddle.
    auto contactHeight = [&](int32_t plane, int32_t leadingBefore, int32_t leadingAfter) {
        const int32_t travelled = leadingBefore - leadingAfter;
        if (travelled == 0) return static_cast<int32_t>(game.ballY);
        const int32_t crossed = leadingBefore - plane;
        return previousY + (game.ballY - previousY) * crossed / travelled;
    };

    if (game.ballVx < 0) {
        const int32_t before = previousX - BALL_RADIUS;
        const int32_t after = game.ballX - BALL_RADIUS;
        if (before > PADDLE_X_HOST && after <= PADDLE_X_HOST) {
            const int32_t contactY = contactHeight(PADDLE_X_HOST, before, after);
            if (abs(contactY - game.hostPaddleY) <= PADDLE_HALF_HEIGHT + BALL_RADIUS) {
                deflect(contactY, game.hostPaddleY, PADDLE_X_HOST + BALL_RADIUS);
            }
        }
        if (game.ballVx < 0 && game.ballX < -BALL_RADIUS) {
            // Only a ball that was in play costs a point: missing the serve that was sent to you
            // is not a miss anyone gets credit for, so it is simply served again.
            if (game.rallyStarted) {
                game.guestScore++;
            }
            serve(ctx, true);
        }
    } else if (game.ballVx > 0) {
        const int32_t before = -(previousX + BALL_RADIUS);
        const int32_t after = -(game.ballX + BALL_RADIUS);
        if (before > -PADDLE_X_GUEST && after <= -PADDLE_X_GUEST) {
            const int32_t contactY = contactHeight(-PADDLE_X_GUEST, before, after);
            if (abs(contactY - game.guestPaddleY) <= PADDLE_HALF_HEIGHT + BALL_RADIUS) {
                deflect(contactY, game.guestPaddleY, PADDLE_X_GUEST - BALL_RADIUS);
            }
        }
        if (game.ballVx > 0 && game.ballX > FIELD_SIZE + BALL_RADIUS) {
            if (game.rallyStarted) {
                game.hostScore++;
            }
            serve(ctx, false);
        }
    }

    if (game.hostScore >= WIN_SCORE || game.guestScore >= WIN_SCORE) {
        game.over = true;
    }
}

Packet buildStatePacket(Context* ctx) {
    Packet packet {};
    packet.type = PacketType::State;
    packet.ballX = ctx->game.ballX;
    packet.ballY = ctx->game.ballY;
    packet.ballVx = ctx->game.ballVx;
    packet.ballVy = ctx->game.ballVy;
    packet.hostPaddleY = ctx->game.hostPaddleY;
    packet.guestPaddleY = ctx->game.guestPaddleY;
    packet.hostScore = ctx->game.hostScore;
    packet.guestScore = ctx->game.guestScore;
    packet.over = ctx->game.over ? 1 : 0;
    return packet;
}

void endMatch(Context* ctx) {
    // The score on the field already says who won.
    ctx->statusText = "";
    ctx->rematchRequested = false;
    setPhase(ctx, Phase::GameOver);
}

void applyInput(Context* ctx) {
    if (lv_tick_elaps(ctx->inputTimeMs.load()) > INPUT_HOLD_MS) {
        ctx->inputDirection = 0;
        return;
    }
    const int direction = ctx->inputDirection.load();
    if (direction == 0) return;
    ctx->localPaddleY = clampToField(ctx->localPaddleY + direction * PADDLE_SPEED, PADDLE_HALF_HEIGHT);
}

/** Remembers a badge that announced itself, evicting the weakest signal when the table is full. */
void rememberPeer(Context* ctx, const Context::Incoming& incoming, uint32_t now) {
    auto it = std::find_if(ctx->peers.begin(), ctx->peers.end(), [&](const Peer& peer) {
        return peer.identity == incoming.packet.from;
    });

    if (it != ctx->peers.end()) {
        it->radioAddress = incoming.radioAddress;
        it->rssi = incoming.rssi;
        it->lastSeenMs = now;
        return;
    }

    if (ctx->peers.size() >= MAX_PEERS) {
        auto weakest = std::min_element(ctx->peers.begin(), ctx->peers.end(), [](const Peer& a, const Peer& b) {
            return a.rssi < b.rssi;
        });
        if (weakest->rssi >= incoming.rssi) {
            return;
        }
        ctx->peers.erase(weakest);
    }

    ctx->peers.push_back(Peer {
        incoming.packet.from,
        incoming.radioAddress,
        nameOf(incoming.packet.from),
        incoming.rssi,
        now
    });
}

void sendDecline(Context* ctx, const MacAddress& to, uint8_t session);

void handlePacket(Context* ctx, const Context::Incoming& incoming, uint32_t now) {
    const Packet& packet = incoming.packet;
    const bool fromOpponent = packet.from == ctx->peerIdentity && packet.session == ctx->session;

    switch (packet.type) {
        case PacketType::Hello:
            // Kept up to date in every phase, so the lobby is populated the moment it returns.
            rememberPeer(ctx, incoming, now);
            break;

        case PacketType::Invite: {
            if (ctx->phase != Phase::Lobby) break;
            if (ctx->hasDeclined && packet.from == ctx->declinedPeer && packet.session == ctx->declinedSession) {
                sendDecline(ctx, packet.from, packet.session);
                break;
            }
            if (!ctx->keys.generate() || !ctx->keys.derive(packet.publicKey, ctx->matchKey, ctx->matchCode)) {
                LOG_E(TAG, "Key agreement failed, ignoring invite");
                break;
            }
            ctx->peerIdentity = packet.from;
            ctx->peerRadioAddress = incoming.radioAddress;
            ctx->peerName = nameOf(packet.from);
            ctx->peerRssi = incoming.rssi;
            ctx->session = packet.session;
            ctx->statusText = "Compare the code before you play";
            setPhase(ctx, Phase::Confirming);
            break;
        }

        case PacketType::Accept:
            if (ctx->phase == Phase::Inviting && fromOpponent) {
                if (!ctx->keys.derive(packet.publicKey, ctx->matchKey, ctx->matchCode)) {
                    returnToLobby(ctx, "Key agreement failed");
                    break;
                }
                ctx->peerRadioAddress = incoming.radioAddress;
                ctx->peerRssi = incoming.rssi;
                if (!addEncryptedPeer(ctx)) {
                    returnToLobby(ctx, "Could not secure the match");
                    break;
                }
                startMatch(ctx, true);
            }
            break;

        case PacketType::Decline:
            if (ctx->phase == Phase::Inviting && fromOpponent) {
                returnToLobby(ctx, ctx->peerName + " declined");
            }
            break;

        case PacketType::Input:
            if (ctx->isHost && fromOpponent && ctx->phase == Phase::Playing) {
                ctx->game.guestPaddleY = clampToField(packet.paddleY, PADDLE_HALF_HEIGHT);
                ctx->lastPeerPacketMs = now;
                ctx->heardOpponent = true;
            }
            break;

        case PacketType::State:
            if (!ctx->isHost && fromOpponent && ctx->phase == Phase::GameOver && packet.over == 0) {
                // The host restarted, which is the only signal a rematch needs.
                startMatch(ctx, false);
            }
            if (!ctx->isHost && fromOpponent && ctx->phase == Phase::Playing) {
                ctx->heardOpponent = true;
                ctx->game.ballX = packet.ballX;
                ctx->game.ballY = packet.ballY;
                ctx->game.ballVx = packet.ballVx;
                ctx->game.ballVy = packet.ballVy;
                ctx->game.hostPaddleY = packet.hostPaddleY;
                ctx->game.hostScore = packet.hostScore;
                ctx->game.guestScore = packet.guestScore;
                ctx->game.over = packet.over != 0;
                ctx->lastPeerPacketMs = now;
            }
            break;

        case PacketType::Rematch:
            if (ctx->isHost && fromOpponent && ctx->phase == Phase::GameOver) {
                startMatch(ctx, true);
            }
            break;

        case PacketType::Bye:
            if (fromOpponent && ctx->phase == Phase::Confirming) {
                returnToLobby(ctx, ctx->peerName + " withdrew the challenge");
            } else if (fromOpponent && ctx->phase != Phase::Lobby) {
                returnToLobby(ctx, "Opponent left the match");
            }
            break;
    }
}

void challenge(Context* ctx, int index) {
    if (index < 0 || index >= (int)ctx->listedPeers.size()) return;

    const MacAddress identity = ctx->listedPeers[index];
    auto it = std::find_if(ctx->peers.begin(), ctx->peers.end(), [&](const Peer& peer) { return peer.identity == identity; });
    if (it == ctx->peers.end()) return;

    if (!ctx->keys.generate()) {
        ctx->statusText = "Key generation failed";
        return;
    }

    ctx->hasDeclined = false;
    ctx->lastInviteSendMs = 0;
    ctx->peerIdentity = it->identity;
    ctx->peerRadioAddress = it->radioAddress;
    ctx->peerName = it->name;
    ctx->peerRssi = it->rssi;
    ctx->session = static_cast<uint8_t>(esp_random());
    ctx->isHost = true;
    ctx->statusText = "Waiting for " + ctx->peerName + " to accept...";
    setPhase(ctx, Phase::Inviting);

    Packet invite {};
    invite.type = PacketType::Invite;
    invite.to = ctx->peerIdentity;
    invite.session = ctx->session;
    invite.publicKey = ctx->keys.getPublicKey();
    broadcastPacket(ctx, invite);
}

void acceptChallenge(Context* ctx) {
    Packet accept {};
    accept.type = PacketType::Accept;
    accept.to = ctx->peerIdentity;
    accept.session = ctx->session;
    accept.publicKey = ctx->keys.getPublicKey();
    broadcastPacket(ctx, accept);

    if (!addEncryptedPeer(ctx)) {
        returnToLobby(ctx, "Could not secure the match");
        return;
    }
    startMatch(ctx, false);
}

void sendDecline(Context* ctx, const MacAddress& to, uint8_t session) {
    Packet decline {};
    decline.type = PacketType::Decline;
    decline.to = to;
    decline.session = session;
    broadcastPacket(ctx, decline);
}

void declineChallenge(Context* ctx) {
    sendDecline(ctx, ctx->peerIdentity, ctx->session);
    // The decline is a single unacknowledged broadcast. Remembering it lets the repeated invite
    // that follows a lost one be answered, instead of the challenger waiting out its timeout.
    ctx->hasDeclined = true;
    ctx->declinedPeer = ctx->peerIdentity;
    ctx->declinedSession = ctx->session;
    returnToLobby(ctx, "Challenge declined");
}

void expirePeers(Context* ctx) {
    std::erase_if(ctx->peers, [](const Peer& peer) { return lv_tick_elaps(peer.lastSeenMs) > PEER_EXPIRY_MS; });
}

void tick(Context* ctx, uint32_t& nextHelloMs, uint32_t& networkTick) {
    const uint32_t now = lv_tick_get();

    std::vector<Context::Incoming> packets;
    {
        auto lock = ctx->inboxMutex.asScopedLock();
        lock.lock();
        packets.swap(ctx->inbox);
    }
    for (const auto& incoming : packets) {
        handlePacket(ctx, incoming, now);
    }

    const bool confirmed = ctx->confirmRequested.exchange(false);
    const bool declined = ctx->declineRequested.exchange(false);
    const bool leaving = ctx->leaveRequested.exchange(false);

    if (ctx->phase == Phase::Confirming) {
        if (confirmed) {
            acceptChallenge(ctx);
        } else if (declined || leaving) {
            declineChallenge(ctx);
        }
    } else if (leaving) {
        if (ctx->phase != Phase::Lobby) {
            leaveMatch(ctx, "Looking for players...", ctx->phase == Phase::Playing || ctx->phase == Phase::GameOver);
        }
    }

    const int requested = ctx->challengeRequest.exchange(-1);
    if (requested >= 0 && ctx->phase == Phase::Lobby) {
        challenge(ctx, requested);
    }

    switch (ctx->phase) {
        case Phase::Lobby:
            expirePeers(ctx);
            if (now >= nextHelloMs) {
                // Jitter keeps a room full of badges from lining their announcements up.
                nextHelloMs = now + HELLO_INTERVAL_MS + (esp_random() % HELLO_JITTER_MS);
                Packet hello {};
                hello.type = PacketType::Hello;
                broadcastPacket(ctx, hello);
            }
            break;

        case Phase::Inviting:
            // Invite, Accept and Decline are all unacknowledged broadcasts, so the challenge is
            // repeated until it is answered: a single lost frame otherwise costs both badges
            // their full timeout, one reporting silence and the other a refusal.
            if (lv_tick_elaps(ctx->lastInviteSendMs) >= INVITE_RETRY_MS) {
                ctx->lastInviteSendMs = now;
                Packet invite {};
                invite.type = PacketType::Invite;
                invite.to = ctx->peerIdentity;
                invite.session = ctx->session;
                invite.publicKey = ctx->keys.getPublicKey();
                broadcastPacket(ctx, invite);
            }
            if (lv_tick_elaps(ctx->phaseStartMs) > INVITE_TIMEOUT_MS) {
                // Withdraw, so the other badge closes its dialog instead of sitting on a
                // challenge nobody is waiting for any more.
                Packet bye {};
                bye.type = PacketType::Bye;
                bye.to = ctx->peerIdentity;
                bye.session = ctx->session;
                broadcastPacket(ctx, bye);
                returnToLobby(ctx, ctx->peerName + " did not respond");
            }
            break;

        case Phase::Confirming:
            if (lv_tick_elaps(ctx->phaseStartMs) > CONFIRM_TIMEOUT_MS) {
                declineChallenge(ctx);
            }
            break;

        case Phase::Playing: {
            applyInput(ctx);
            if (ctx->isHost) {
                ctx->game.hostPaddleY = ctx->localPaddleY;
                simulate(ctx);
            } else {
                ctx->game.guestPaddleY = ctx->localPaddleY;
                // State arrives at 20Hz and is not retransmitted when lost, so the guest carries
                // the ball on with the velocity it was last told. Without this the ball sits
                // still between packets and stops dead on the first one that goes missing.
                extrapolateBall(ctx);

                // The Accept is an unacknowledged broadcast: repeat it until the host's first
                // state proves it heard, otherwise this side plays a match nobody is running.
                if (!ctx->heardOpponent && lv_tick_elaps(ctx->lastHandshakeSendMs) >= HANDSHAKE_RETRY_MS) {
                    ctx->lastHandshakeSendMs = lv_tick_get();
                    Packet accept {};
                    accept.type = PacketType::Accept;
                    accept.to = ctx->peerIdentity;
                    accept.session = ctx->session;
                    accept.publicKey = ctx->keys.getPublicKey();
                    broadcastPacket(ctx, accept);
                }
            }

            // Half of the ticks carry network traffic: 20Hz is enough for a paddle game
            // and keeps the ESP-NOW send out of every frame. The tick that ends the match
            // always sends, so the guest learns the result from the state it is waiting for.
            if ((networkTick++ % 2) == 0 || (ctx->isHost && ctx->game.over)) {
                if (ctx->isHost) {
                    Packet packet = buildStatePacket(ctx);
                    sendToOpponent(ctx, packet);
                } else {
                    Packet packet {};
                    packet.type = PacketType::Input;
                    packet.paddleY = ctx->localPaddleY;
                    sendToOpponent(ctx, packet);
                }
            }

            if (ctx->game.over) {
                endMatch(ctx);
            } else if (lv_tick_elaps(ctx->lastPeerPacketMs) > MATCH_TIMEOUT_MS) {
                leaveMatch(ctx, "Lost contact with " + ctx->peerName, false);
            }
            break;
        }

        case Phase::GameOver:
            if (ctx->rematchRequested.exchange(false)) {
                if (ctx->isHost) {
                    // The host restarting is the signal: its next state carries over == 0.
                    startMatch(ctx, true);
                } else {
                    ctx->statusText = "Waiting for " + ctx->peerName + "...";
                    ctx->rematchPending = true;
                    ctx->rematchRequestMs = lv_tick_get();
                    ctx->lastResultSendMs = 0;
                }
                break;
            }

            // The request is one more unacknowledged frame, so repeat it rather than wait on a
            // host that never heard it, and give up if the answer never comes.
            if (ctx->rematchPending) {
                if (lv_tick_elaps(ctx->rematchRequestMs) > REMATCH_TIMEOUT_MS) {
                    leaveMatch(ctx, ctx->peerName + " did not restart", false);
                    break;
                }
                if (lv_tick_elaps(ctx->lastResultSendMs) >= RESULT_RESEND_MS) {
                    ctx->lastResultSendMs = lv_tick_get();
                    Packet rematch {};
                    rematch.type = PacketType::Rematch;
                    sendToOpponent(ctx, rematch);
                }
                break;
            }
            // ESP-NOW unicast is acknowledged, but the acknowledgement is not visible here, so
            // keep repeating the final state until the player leaves the result screen.
            if (ctx->isHost && lv_tick_elaps(ctx->lastResultSendMs) >= RESULT_RESEND_MS) {
                ctx->lastResultSendMs = now;
                Packet packet = buildStatePacket(ctx);
                sendToOpponent(ctx, packet);
            }
            break;
    }
}

// endregion

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();

    Context ctx; // Aggregate init is unavailable: RecursiveMutex has an explicit default constructor
    ctx.appInstanceId = appInstanceId;
    esp_read_mac(ctx.myIdentity.data(), ESP_MAC_WIFI_STA);
    ctx.myName = nameOf(ctx.myIdentity);
    ctx.statusText = "Looking for players...";

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    // The lobby goes up first: leaving the access point takes up to two seconds, and there is
    // no reason for the player to watch a blank screen through it.
    WindowId window = window_manager_create(appInstanceId, createWidgets, &ctx);

    ctx.restoreSsid = leaveAccessPoint();

    static uint8_t defaultKey[ESP_NOW_KEY_LEN] = {};
    service::espnow::enable(service::espnow::EspNowConfig(defaultKey, service::espnow::Mode::Station, 1, false, false));

    auto subscription = service::espnow::subscribeReceiver(
        [&ctx](const esp_now_recv_info_t* receiveInfo, const uint8_t* data, int length) {
            onReceive(&ctx, receiveInfo, data, length);
        }
    );

    uint32_t nextHelloMs = 0;
    uint32_t networkTick = 0;
    uint32_t lastPeerListMs = 0;

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&event_group, nullptr, pdMS_TO_TICKS(TICK_MS));

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

        tick(&ctx, nextHelloMs, networkTick);

        const uint32_t now = lv_tick_get();
        lvgl_lock();
        if (ctx.widgets.built && ctx.widgets.builtPhase != ctx.phase) {
            buildContent(&ctx);
            lastPeerListMs = 0;
        }
        if (ctx.phase == Phase::Lobby && (lastPeerListMs == 0 || lv_tick_elaps(lastPeerListMs) >= PEER_LIST_REFRESH_MS)) {
            lastPeerListMs = now;
            refreshPeerList(&ctx);
        } else if (ctx.phase == Phase::Playing && ctx.widgets.field != nullptr) {
            // The keypad drops out of edit mode on its own (e.g. on a long press), which
            // would turn the arrow keys back into focus navigation mid-match.
            lv_group_set_editing(lv_group_get_default(), true);
        }
        render(&ctx);
        lvgl_unlock();
    }

    if (ctx.phase == Phase::Confirming) {
        // Leaving via the toolbar's back key is an answer too, so the challenger stops waiting.
        declineChallenge(&ctx);
    } else if (ctx.phase == Phase::Playing || ctx.phase == Phase::GameOver) {
        Packet bye {};
        bye.type = PacketType::Bye;
        sendToOpponent(&ctx, bye);
    }
    removeEncryptedPeer(&ctx);
    setRadioLowLatency(false);

    service::espnow::unsubscribeReceiver(subscription);
    if (service::espnow::isEnabled()) {
        service::espnow::disable();
    }
    rejoinAccessPoint(ctx.restoreSsid);

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.pingpong",
    .name = "Ping Pong",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) }
};

} // namespace tt::app::pingpong

#endif // CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SLAVE_SOC_WIFI_SUPPORTED
