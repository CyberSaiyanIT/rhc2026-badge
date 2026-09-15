#pragma once

#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

#if defined(CONFIG_SOC_WIFI_SUPPORTED) || defined(CONFIG_SLAVE_SOC_WIFI_SUPPORTED)

#include "PingPongCrypto.h"

#include <esp_now.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tt::app::pingpong {

constexpr uint32_t PROTOCOL_MAGIC = 0x474E4950; // bytes 'P','I','N','G' read little-endian
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr size_t HEADER_SIZE = 19;
constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + PUBLIC_KEY_SIZE;

using MacAddress = std::array<uint8_t, ESP_NOW_ETH_ALEN>;
constexpr MacAddress BROADCAST_ADDRESS = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

enum class PacketType : uint8_t {
    Hello = 1,   /**< Lobby presence announcement */
    Invite = 2,  /**< Challenge, carries the sender's public key. The sender hosts the match. */
    Accept = 3,  /**< Challenge accepted, carries the other public key */
    Decline = 4, /**< Challenge refused or timed out */
    State = 5,   /**< Host's authoritative field state */
    Input = 6,   /**< Guest's paddle position */
    Bye = 7,     /**< Sender left the match */
    Rematch = 8  /**< Guest asks the host to start another match on the same key */
};

struct Packet {
    PacketType type = PacketType::Hello;
    uint8_t session = 0;
    MacAddress from {};
    MacAddress to = BROADCAST_ADDRESS;

    PublicKey publicKey {}; // Invite, Accept

    // State
    int16_t ballX = 0;
    int16_t ballY = 0;
    int16_t ballVx = 0;
    int16_t ballVy = 0;
    int16_t hostPaddleY = 0;
    int16_t guestPaddleY = 0;
    uint8_t hostScore = 0;
    uint8_t guestScore = 0;
    uint8_t over = 0;

    int16_t paddleY = 0; // Input
};

/** Names are derived from the identity address, so two badges can never show the same name. */
std::string nameOf(const MacAddress& address);

/** @return the number of bytes written to out, or 0 when the packet does not fit or is invalid */
size_t serialize(const Packet& packet, uint8_t* out, size_t outSize);

bool deserialize(const uint8_t* data, size_t length, Packet& out);

} // namespace tt::app::pingpong

#endif // CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SLAVE_SOC_WIFI_SUPPORTED
