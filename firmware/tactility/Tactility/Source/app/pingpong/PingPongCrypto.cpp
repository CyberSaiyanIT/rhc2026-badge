#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

#if defined(CONFIG_SOC_WIFI_SUPPORTED) || defined(CONFIG_SLAVE_SOC_WIFI_SUPPORTED)

#include <Tactility/app/pingpong/PingPongCrypto.h>

#include <tactility/log.h>

#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

namespace tt::app::pingpong {

namespace {

constexpr auto* TAG = "PingPongCrypto";

int randomSource(void*, unsigned char* output, size_t length) {
    esp_fill_random(output, length);
    return 0;
}

/** Domain-separated SHA-256 over the shared secret and both public keys, in a fixed order. */
void hashTranscript(
    const uint8_t* secret,
    const PublicKey& first,
    const PublicKey& second,
    const char* label,
    uint8_t (&out)[32]
) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char*>(label), strlen(label));
    mbedtls_sha256_update(&sha, secret, 32);
    mbedtls_sha256_update(&sha, first.data(), first.size());
    mbedtls_sha256_update(&sha, second.data(), second.size());
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
}

} // namespace

bool KeyAgreement::generate() {
    mbedtls_ecp_group group;
    mbedtls_mpi secret;
    mbedtls_ecp_point point;

    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&secret);
    mbedtls_ecp_point_init(&point);

    valid = false;
    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
        mbedtls_ecdh_gen_public(&group, &secret, &point, randomSource, nullptr) == 0 &&
        mbedtls_mpi_write_binary_le(&secret, privateKey.data(), privateKey.size()) == 0 &&
        mbedtls_mpi_write_binary_le(&point.MBEDTLS_PRIVATE(X), publicKey.data(), publicKey.size()) == 0) {
        valid = true;
    } else {
        LOG_E(TAG, "Failed to generate key pair");
    }

    mbedtls_ecp_point_free(&point);
    mbedtls_mpi_free(&secret);
    mbedtls_ecp_group_free(&group);
    return valid;
}

bool KeyAgreement::derive(const PublicKey& peerPublicKey, LocalMasterKey& outKey, uint16_t& outCode) const {
    if (!valid) {
        return false;
    }

    mbedtls_ecp_group group;
    mbedtls_mpi ownSecret;
    mbedtls_mpi sharedSecret;
    mbedtls_ecp_point peerPoint;

    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&ownSecret);
    mbedtls_mpi_init(&sharedSecret);
    mbedtls_ecp_point_init(&peerPoint);

    uint8_t secret[32];
    bool success = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
        mbedtls_mpi_read_binary_le(&ownSecret, privateKey.data(), privateKey.size()) == 0 &&
        mbedtls_mpi_read_binary_le(&peerPoint.MBEDTLS_PRIVATE(X), peerPublicKey.data(), peerPublicKey.size()) == 0 &&
        mbedtls_mpi_lset(&peerPoint.MBEDTLS_PRIVATE(Z), 1) == 0 &&
        mbedtls_ecdh_compute_shared(&group, &sharedSecret, &peerPoint, &ownSecret, randomSource, nullptr) == 0 &&
        mbedtls_mpi_write_binary_le(&sharedSecret, secret, sizeof(secret)) == 0;

    if (success) {
        const PublicKey& first = (publicKey < peerPublicKey) ? publicKey : peerPublicKey;
        const PublicKey& second = (publicKey < peerPublicKey) ? peerPublicKey : publicKey;

        uint8_t keyHash[32];
        hashTranscript(secret, first, second, "tactility-pingpong-lmk", keyHash);
        std::copy_n(keyHash, outKey.size(), outKey.begin());

        uint8_t codeHash[32];
        hashTranscript(secret, first, second, "tactility-pingpong-sas", codeHash);
        outCode = static_cast<uint16_t>(((codeHash[0] << 8) | codeHash[1]) % 10000);

        memset(keyHash, 0, sizeof(keyHash));
        memset(codeHash, 0, sizeof(codeHash));
    } else {
        LOG_E(TAG, "Failed to derive shared secret");
    }

    memset(secret, 0, sizeof(secret));
    mbedtls_ecp_point_free(&peerPoint);
    mbedtls_mpi_free(&sharedSecret);
    mbedtls_mpi_free(&ownSecret);
    mbedtls_ecp_group_free(&group);
    return success;
}

} // namespace tt::app::pingpong

#endif // CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SLAVE_SOC_WIFI_SUPPORTED
