#pragma once

#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

#if defined(CONFIG_SOC_WIFI_SUPPORTED) || defined(CONFIG_SLAVE_SOC_WIFI_SUPPORTED)

#include <array>
#include <cstddef>
#include <cstdint>

namespace tt::app::pingpong {

constexpr size_t PUBLIC_KEY_SIZE = 32;
constexpr size_t LMK_SIZE = 16;

using PublicKey = std::array<uint8_t, PUBLIC_KEY_SIZE>;
using LocalMasterKey = std::array<uint8_t, LMK_SIZE>;

/**
 * X25519 key agreement for one match. The badge's ESP32-S3 has no ECC accelerator, so the two
 * scalar multiplications run in software; they happen once, before the match starts.
 */
class KeyAgreement {

    std::array<uint8_t, 32> privateKey {};
    PublicKey publicKey {};
    bool valid = false;

public:

    /** Generates a fresh key pair. Returns false when the RNG or the curve is unavailable. */
    bool generate();

    const PublicKey& getPublicKey() const { return publicKey; }

    /**
     * Derives the ESP-NOW local master key and the short authentication string from the shared
     * secret. Both sides sort the public keys before hashing, so neither needs to have sent first.
     *
     * @param[in] peerPublicKey the other side's public key
     * @param[out] outKey the derived 16-byte LMK
     * @param[out] outCode the 4-digit code both players compare on screen
     */
    bool derive(const PublicKey& peerPublicKey, LocalMasterKey& outKey, uint16_t& outCode) const;
};

} // namespace tt::app::pingpong

#endif // CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SLAVE_SOC_WIFI_SUPPORTED
