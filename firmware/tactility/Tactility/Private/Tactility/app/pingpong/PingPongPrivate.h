#pragma once

#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

#if defined(CONFIG_SOC_WIFI_SUPPORTED) || defined(CONFIG_SLAVE_SOC_WIFI_SUPPORTED)

#include "PingPongProtocol.h"

#include <Tactility/RecursiveMutex.h>

#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace tt::app::pingpong {

// The field is a resolution-independent 1000x1000 unit square that is mapped onto the
// widget area at render time, so both players agree on physics regardless of display size.
constexpr int16_t FIELD_SIZE = 1000;
constexpr int16_t PADDLE_HALF_HEIGHT = 110;
constexpr int16_t PADDLE_X_HOST = 40;
constexpr int16_t PADDLE_X_GUEST = FIELD_SIZE - PADDLE_X_HOST;
constexpr int16_t BALL_RADIUS = 18;
constexpr int16_t BALL_SPEED_X = 16;
constexpr int16_t BALL_SPEED_Y_MAX = 26;
constexpr int16_t PADDLE_SPEED = 26;
constexpr uint8_t WIN_SCORE = 5;
/** The ball waits at the centre this long before a serve sets off. */
constexpr uint8_t SERVE_DELAY_TICKS = 32;

constexpr uint32_t TICK_MS = 25;
// With hundreds of badges in one room, presence is the dominant source of channel traffic:
// a ~1ms broadcast per badge per interval. Three seconds plus jitter keeps that bounded.
constexpr uint32_t HELLO_INTERVAL_MS = 3000;
constexpr uint32_t HELLO_JITTER_MS = 1000;
constexpr uint32_t PEER_EXPIRY_MS = 12000;
constexpr uint32_t PEER_LIST_REFRESH_MS = 1000;
constexpr uint32_t INVITE_TIMEOUT_MS = 15000;
/** Longer than the invite times out, so the challenger withdraws before this ever fires. */
constexpr uint32_t CONFIRM_TIMEOUT_MS = 25000;
/** Invites are unacknowledged broadcasts, so they are repeated while waiting for an answer. */
constexpr uint32_t INVITE_RETRY_MS = 1000;
/** Handshake retry cadence while both sides settle into the match. */
constexpr uint32_t HANDSHAKE_RETRY_MS = 300;
/** Peers held in memory, the weakest signal dropped when the room is bigger. Holding many keeps
 *  the listed top-20 close to the true strongest rather than to whoever announced first. */
constexpr size_t MAX_PEERS = 256;
/** Peers shown in the lobby, strongest signal first. */
constexpr size_t MAX_LISTED_PEERS = 20;
constexpr uint32_t MATCH_TIMEOUT_MS = 4000;
constexpr uint32_t RESULT_RESEND_MS = 500;
/** How long a rematch request waits for the other badge before giving up on it. */
constexpr uint32_t REMATCH_TIMEOUT_MS = 6000;
/** Keeps the paddle moving between key repeats, and stops it shortly after the key is let go. */
constexpr uint32_t INPUT_HOLD_MS = 90;

enum class Phase : uint8_t {
    Lobby,
    Inviting,  /**< Waiting for the challenged badge to accept */
    Confirming, /**< Deciding on an incoming challenge, where the match code is compared */
    Playing,
    GameOver
};

struct Peer {
    MacAddress identity;
    MacAddress radioAddress;
    std::string name;
    int8_t rssi;
    uint32_t lastSeenMs;
};

struct GameState {
    int16_t ballX = FIELD_SIZE / 2;
    int16_t ballY = FIELD_SIZE / 2;
    int16_t ballVx = BALL_SPEED_X;
    int16_t ballVy = 0;
    int16_t hostPaddleY = FIELD_SIZE / 2;
    int16_t guestPaddleY = FIELD_SIZE / 2;
    uint8_t hostScore = 0;
    uint8_t guestScore = 0;
    bool over = false;
    /** A serve nobody has returned yet scores nothing, so missing your own serve is free. */
    bool rallyStarted = false;
    bool serveTowardsHost = false;
    uint8_t serveDelayTicks = 0;
};

constexpr int NET_SEGMENTS = 9;

struct Widgets {
    bool built = false;
    Phase builtPhase = Phase::Lobby;
    lv_obj_t* content = nullptr;
    lv_obj_t* peerList = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* field = nullptr;
    lv_obj_t* leftPaddle = nullptr;
    lv_obj_t* rightPaddle = nullptr;
    lv_obj_t* ball = nullptr;
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* codeLabel = nullptr;
    lv_obj_t* net[NET_SEGMENTS] = {};
};

struct Context {
    uint32_t appInstanceId = 0;
    /** Advertised identity. Equals the radio address on native ESP-NOW. */
    MacAddress myIdentity {};
    /** The access point to rejoin on the way out, empty when none was connected. */
    std::string restoreSsid;
    std::string myName;

    Phase phase = Phase::Lobby;
    bool isHost = false;
    MacAddress peerIdentity {};
    MacAddress peerRadioAddress {};
    std::string peerName;
    int8_t peerRssi = 0;
    uint8_t session = 0;

    /** A declined challenge is remembered, so a repeated invite is answered rather than re-asked. */
    bool hasDeclined = false;
    MacAddress declinedPeer {};
    uint8_t declinedSession = 0;

    KeyAgreement keys;
    LocalMasterKey matchKey {};
    uint16_t matchCode = 0;
    bool encryptedPeerAdded = false;
    /** Set once a match packet from the opponent proves both sides installed the same key. */
    bool heardOpponent = false;
    uint32_t lastHandshakeSendMs = 0;
    uint32_t lastInviteSendMs = 0;
    bool rematchPending = false;
    uint32_t rematchRequestMs = 0;
    uint32_t phaseStartMs = 0;
    uint32_t lastPeerPacketMs = 0;
    uint32_t lastResultSendMs = 0;
    std::string statusText;
    std::string renderedStatusText;

    GameState game;
    /** The local paddle is predicted locally so it tracks input without a network round trip. */
    int16_t localPaddleY = FIELD_SIZE / 2;

    std::vector<Peer> peers;
    /** Identities currently shown in the lobby, so a click resolves to the right badge. */
    std::vector<MacAddress> listedPeers;
    Widgets widgets;
    bool keypadTuned = false;

    /** Written by the LVGL key callback, read by the game loop. */
    std::atomic<int> inputDirection {0};
    std::atomic<uint32_t> inputTimeMs {0};
    std::atomic<bool> leaveRequested {false};
    std::atomic<bool> confirmRequested {false};
    std::atomic<bool> declineRequested {false};
    std::atomic<bool> rematchRequested {false};
    std::atomic<int> challengeRequest {-1};

    /** A packet as it arrived, with the transport details the header does not carry. */
    struct Incoming {
        Packet packet;
        MacAddress radioAddress;
        int8_t rssi;
    };

    /** Guards the inbox, which the ESP-NOW receive task fills from another thread. */
    RecursiveMutex inboxMutex;
    std::vector<Incoming> inbox;
};

} // namespace tt::app::pingpong

#endif // CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SLAVE_SOC_WIFI_SUPPORTED
