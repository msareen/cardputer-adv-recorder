#include "recorder/sync/secure_channel.h"

#include <mbedtls/gcm.h>

#include <cstring>

#include "recorder/sync/pairing_math.h"

namespace cardputer_recorder {
namespace {

constexpr std::size_t kHeaderSize = 9;
constexpr std::size_t kTagSize = 16;

}  // namespace

void SecureChannel::begin(const std::uint8_t key[32])
{
    std::memcpy(key_, key, sizeof(key_));
    sendCounter_ = 0;
    receiveCounter_ = 0;
    active_ = true;
}

void SecureChannel::end()
{
    std::memset(key_, 0, sizeof(key_));
    active_ = false;
}

bool SecureChannel::seal(const std::string& plaintext, std::string& frame)
{
    if (!active_) {
        return false;
    }
    ++sendCounter_;
    std::uint8_t header[kHeaderSize];
    header[0] = kFrameSecure;
    for (int i = 0; i < 8; ++i) {
        header[1 + i] =
            static_cast<std::uint8_t>(sendCounter_ >> (56 - 8 * i));
    }
    std::uint8_t nonce[12];
    gcmNonce(kDirectionDevice, sendCounter_, nonce);

    frame.assign(reinterpret_cast<const char*>(header), kHeaderSize);
    frame.resize(kHeaderSize + plaintext.size() + kTagSize);
    auto* out = reinterpret_cast<std::uint8_t*>(&frame[kHeaderSize]);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const bool ok =
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key_, 256) == 0 &&
        mbedtls_gcm_crypt_and_tag(
            &gcm, MBEDTLS_GCM_ENCRYPT, plaintext.size(), nonce,
            sizeof(nonce), header, kHeaderSize,
            reinterpret_cast<const std::uint8_t*>(plaintext.data()), out,
            kTagSize, out + plaintext.size()) == 0;
    mbedtls_gcm_free(&gcm);
    return ok;
}

bool SecureChannel::open(const std::string& frame, std::string& plaintext)
{
    if (!active_ || frame.size() < kHeaderSize + kTagSize ||
        static_cast<std::uint8_t>(frame[0]) != kFrameSecure) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame.data());
    std::uint64_t counter = 0;
    for (int i = 0; i < 8; ++i) {
        counter = counter << 8 | bytes[1 + i];
    }
    if (counter <= receiveCounter_) {
        return false;  // replay or reordering
    }
    std::uint8_t nonce[12];
    gcmNonce(kDirectionHost, counter, nonce);
    const std::size_t size = frame.size() - kHeaderSize - kTagSize;
    plaintext.resize(size);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const bool ok =
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key_, 256) == 0 &&
        mbedtls_gcm_auth_decrypt(
            &gcm, size, nonce, sizeof(nonce), bytes, kHeaderSize,
            bytes + kHeaderSize + size, kTagSize, bytes + kHeaderSize,
            reinterpret_cast<std::uint8_t*>(&plaintext[0])) == 0;
    mbedtls_gcm_free(&gcm);
    if (!ok) {
        plaintext.clear();
        return false;
    }
    receiveCounter_ = counter;
    return true;
}

}  // namespace cardputer_recorder
