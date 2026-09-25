#include <Tactility/service/music/MediaKeys.h>
#include <Tactility/service/music/Music.h>

#include <lvgl/devices/keyboard.h>
#include <lvgl_window_manager/window_manager.h>

#include <atomic>

namespace tt::service::music {

namespace {

// The badge's dedicated media keys, as emitted by romhack_keypad.cpp. Not LV_KEY_* values: they
// sit outside Unicode on purpose so they cannot collide with a character a keyboard produces.
constexpr uint32_t KEY_PLAY_PAUSE = 0x20000;
constexpr uint32_t KEY_PREVIOUS = 0x20001;
constexpr uint32_t KEY_NEXT = 0x20002;

/**
 * The app instance that wants the media keys, or 0 for none. An instance rather than an app id and
 * checked against the front window, so a killed app cannot leave the keys claimed.
 */
std::atomic<uint32_t> claimedBy { 0 };
std::atomic<MediaKeyHandlerFn> handlerFunction { nullptr };
std::atomic<void*> handlerContext { nullptr };
std::atomic<bool> enabled { false };
std::atomic<uint8_t> pending { (uint8_t) MediaCommand::None };

MediaCommand commandFor(uint32_t key) {
    switch (key) {
        case KEY_PLAY_PAUSE: return MediaCommand::PlayPause;
        case KEY_NEXT: return MediaCommand::Next;
        case KEY_PREVIOUS: return MediaCommand::Previous;
        default: return MediaCommand::None;
    }
}

bool onKey(uint32_t key, bool pressed, void* /*context*/) {
    const auto command = commandFor(key);
    if (command == MediaCommand::None) {
        return false;
    }

    // Left for the app: it has its own use for this key, or a richer one than transport control.
    const uint32_t claim = claimedBy.load();
    if (claim != 0 && claim == window_manager_get_topmost_app()) {
        const auto handler = handlerFunction.load();
        return handler != nullptr && handler(key, pressed, handlerContext.load());
    }

    if (!enabled.load()) {
        // Swallowed even so. A key that does nothing here must not fall through to whatever
        // widget happens to be focused, which would read as a random button press.
        return true;
    }

    // Only the press acts; the release is swallowed so the two never count as two presses.
    if (pressed) {
        pending = (uint8_t) command;
    }
    return true;
}

} // namespace

void installMediaKeys() {
    lvgl_keyboard_set_filter(onKey, nullptr);
}

void removeMediaKeys() {
    lvgl_keyboard_set_filter(nullptr, nullptr);
}

void setMediaKeysEnabled(bool value) {
    enabled = value;
}

MediaCommand takeMediaCommand() {
    return (MediaCommand) pending.exchange((uint8_t) MediaCommand::None);
}

void claimMediaKeys(uint32_t appInstanceId) {
    claimedBy = appInstanceId;
}

void setMediaKeyHandler(uint32_t appInstanceId, MediaKeyHandlerFn handler, void* context) {
    if (claimedBy.load() != appInstanceId) {
        return;
    }
    // Context first, so the LVGL task can never read the new handler against the old context.
    handlerContext = context;
    handlerFunction = handler;
}

void releaseMediaKeys(uint32_t appInstanceId) {
    // Compared before clearing, so an app closing after another has already claimed the keys
    // does not take them away from it.
    uint32_t expected = appInstanceId;
    if (claimedBy.compare_exchange_strong(expected, 0)) {
        handlerFunction = nullptr;
        handlerContext = nullptr;
    }
}

} // namespace tt::service::music
