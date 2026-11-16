#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cardputer_recorder {

// Pure parts of BLE pairing (docs/ble-protocol.md), shared with the desktop's
// src-tauri/src/pairing.rs and checked against the same vectors. ECDH and
// AES-GCM run on mbedtls in device-only code.

constexpr std::size_t kPublicKeySize = 65;  // uncompressed P-256
constexpr std::size_t kPairNonceSize = 16;
constexpr std::uint8_t kFramePlain = 0x00;
constexpr std::uint8_t kFrameSecure = 0x01;
constexpr std::uint8_t kDirectionHost = 'H';
constexpr std::uint8_t kDirectionDevice = 'D';
constexpr std::size_t kMaxControlMessage = 2048;

void hmacSha256(const std::uint8_t* key, std::size_t keySize,
                const std::uint8_t* data, std::size_t size,
                std::uint8_t out[32]);
void hkdfSha256(const std::uint8_t* salt, std::size_t saltSize,
                const std::uint8_t* ikm, std::size_t ikmSize,
                const std::uint8_t* info, std::size_t infoSize,
                std::uint8_t* out, std::size_t outSize);

void pairingCommitment(const std::uint8_t hostPk[kPublicKeySize],
                       const std::uint8_t hostNonce[kPairNonceSize],
                       std::uint8_t out[32]);
void pairingSessionKey(const std::uint8_t shared[32],
                       const std::uint8_t hostPk[kPublicKeySize],
                       const std::uint8_t devicePk[kPublicKeySize],
                       const std::uint8_t hostNonce[kPairNonceSize],
                       const std::uint8_t deviceNonce[kPairNonceSize],
                       std::uint8_t out[32]);
std::string pairingCode(const std::uint8_t hostPk[kPublicKeySize],
                        const std::uint8_t devicePk[kPublicKeySize],
                        const std::uint8_t hostNonce[kPairNonceSize],
                        const std::uint8_t deviceNonce[kPairNonceSize]);
// First 16 hex chars of SHA-256(pk): device ids and host ids.
std::string keyIdHex(const std::uint8_t pk[kPublicKeySize]);
void gcmNonce(std::uint8_t direction, std::uint64_t counter,
              std::uint8_t out[12]);

// BLE fragments: byte 0 is 1 on the last fragment, 0 otherwise.
std::vector<std::string> fragmentMessage(const std::string& message,
                                         std::size_t maxFragment);

class Reassembler {
public:
    enum class Result : std::uint8_t {
        kPending,
        kComplete,
        kError,
    };
    Result push(const std::uint8_t* fragment, std::size_t size,
                std::string& message);
    void reset() { buffer_.clear(); }

private:
    std::string buffer_;
};

}  // namespace cardputer_recorder
