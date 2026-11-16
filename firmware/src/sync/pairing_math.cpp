#include "recorder/sync/pairing_math.h"

#include <cstring>

#include "recorder/sync/sha256.h"

namespace cardputer_recorder {
namespace {

void sha256Of(const std::string& data, std::uint8_t out[32])
{
    Sha256 hash;
    hash.update(data.data(), data.size());
    hash.finish(out);
}

std::string bytesToString(const std::uint8_t* data, std::size_t size)
{
    return std::string(reinterpret_cast<const char*>(data), size);
}

}  // namespace

void hmacSha256(const std::uint8_t* key, std::size_t keySize,
                const std::uint8_t* data, std::size_t size,
                std::uint8_t out[32])
{
    std::uint8_t block[64] = {};
    if (keySize > sizeof(block)) {
        Sha256 hash;
        hash.update(key, keySize);
        hash.finish(block);
    } else {
        std::memcpy(block, key, keySize);
    }
    std::uint8_t pad[64];
    for (int i = 0; i < 64; ++i) {
        pad[i] = block[i] ^ 0x36;
    }
    Sha256 inner;
    inner.update(pad, sizeof(pad));
    inner.update(data, size);
    std::uint8_t innerDigest[32];
    inner.finish(innerDigest);
    for (int i = 0; i < 64; ++i) {
        pad[i] = block[i] ^ 0x5C;
    }
    Sha256 outer;
    outer.update(pad, sizeof(pad));
    outer.update(innerDigest, sizeof(innerDigest));
    outer.finish(out);
}

void hkdfSha256(const std::uint8_t* salt, std::size_t saltSize,
                const std::uint8_t* ikm, std::size_t ikmSize,
                const std::uint8_t* info, std::size_t infoSize,
                std::uint8_t* out, std::size_t outSize)
{
    static const std::uint8_t kZeroSalt[32] = {};
    std::uint8_t prk[32];
    if (salt == nullptr || saltSize == 0) {
        hmacSha256(kZeroSalt, sizeof(kZeroSalt), ikm, ikmSize, prk);
    } else {
        hmacSha256(salt, saltSize, ikm, ikmSize, prk);
    }

    std::string previous;
    std::size_t done = 0;
    for (std::uint8_t counter = 1; done < outSize; ++counter) {
        std::string block = previous;
        block.append(reinterpret_cast<const char*>(info), infoSize);
        block += static_cast<char>(counter);
        std::uint8_t t[32];
        hmacSha256(prk, sizeof(prk),
                   reinterpret_cast<const std::uint8_t*>(block.data()),
                   block.size(), t);
        const std::size_t take = outSize - done < 32 ? outSize - done : 32;
        std::memcpy(out + done, t, take);
        done += take;
        previous = bytesToString(t, sizeof(t));
    }
}

void pairingCommitment(const std::uint8_t hostPk[kPublicKeySize],
                       const std::uint8_t hostNonce[kPairNonceSize],
                       std::uint8_t out[32])
{
    std::string data = "cprec v1 commit";
    data += bytesToString(hostPk, kPublicKeySize);
    data += bytesToString(hostNonce, kPairNonceSize);
    sha256Of(data, out);
}

void pairingSessionKey(const std::uint8_t shared[32],
                       const std::uint8_t hostPk[kPublicKeySize],
                       const std::uint8_t devicePk[kPublicKeySize],
                       const std::uint8_t hostNonce[kPairNonceSize],
                       const std::uint8_t deviceNonce[kPairNonceSize],
                       std::uint8_t out[32])
{
    std::uint8_t salt[2 * kPairNonceSize];
    std::memcpy(salt, hostNonce, kPairNonceSize);
    std::memcpy(salt + kPairNonceSize, deviceNonce, kPairNonceSize);
    std::string info = "cprec v1 session";
    info += bytesToString(hostPk, kPublicKeySize);
    info += bytesToString(devicePk, kPublicKeySize);
    hkdfSha256(salt, sizeof(salt), shared, 32,
               reinterpret_cast<const std::uint8_t*>(info.data()),
               info.size(), out, 32);
}

std::string pairingCode(const std::uint8_t hostPk[kPublicKeySize],
                        const std::uint8_t devicePk[kPublicKeySize],
                        const std::uint8_t hostNonce[kPairNonceSize],
                        const std::uint8_t deviceNonce[kPairNonceSize])
{
    std::string data = "cprec v1 code";
    data += bytesToString(hostPk, kPublicKeySize);
    data += bytesToString(devicePk, kPublicKeySize);
    data += bytesToString(hostNonce, kPairNonceSize);
    data += bytesToString(deviceNonce, kPairNonceSize);
    std::uint8_t digest[32];
    sha256Of(data, digest);
    const std::uint32_t value =
        (static_cast<std::uint32_t>(digest[0]) << 24 |
         static_cast<std::uint32_t>(digest[1]) << 16 |
         static_cast<std::uint32_t>(digest[2]) << 8 |
         static_cast<std::uint32_t>(digest[3])) %
        1000000U;
    char text[8];
    // Six digits with leading zeros, without pulling in printf.
    std::uint32_t rest = value;
    for (int i = 5; i >= 0; --i) {
        text[i] = static_cast<char>('0' + rest % 10);
        rest /= 10;
    }
    text[6] = '\0';
    return text;
}

std::string keyIdHex(const std::uint8_t pk[kPublicKeySize])
{
    Sha256 hash;
    hash.update(pk, kPublicKeySize);
    std::uint8_t digest[32];
    hash.finish(digest);
    return toHex(digest, 8);
}

void gcmNonce(std::uint8_t direction, std::uint64_t counter,
              std::uint8_t out[12])
{
    std::memset(out, 0, 12);
    out[0] = direction;
    for (int i = 0; i < 8; ++i) {
        out[4 + i] = static_cast<std::uint8_t>(counter >> (56 - 8 * i));
    }
}

std::vector<std::string> fragmentMessage(const std::string& message,
                                         std::size_t maxFragment)
{
    const std::size_t body = (maxFragment < 2 ? 2 : maxFragment) - 1;
    std::vector<std::string> fragments;
    std::size_t offset = 0;
    do {
        const std::size_t take =
            message.size() - offset < body ? message.size() - offset : body;
        const bool last = offset + take >= message.size();
        std::string fragment(1, static_cast<char>(last ? 1 : 0));
        fragment.append(message, offset, take);
        fragments.push_back(fragment);
        offset += take;
    } while (offset < message.size());
    return fragments;
}

Reassembler::Result Reassembler::push(const std::uint8_t* fragment,
                                      std::size_t size,
                                      std::string& message)
{
    if (fragment == nullptr || size == 0) {
        buffer_.clear();
        return Result::kError;
    }
    if (buffer_.size() + size - 1 > kMaxControlMessage) {
        buffer_.clear();
        return Result::kError;
    }
    buffer_.append(reinterpret_cast<const char*>(fragment + 1), size - 1);
    if ((fragment[0] & 1) == 0) {
        return Result::kPending;
    }
    message.swap(buffer_);
    buffer_.clear();
    return Result::kComplete;
}

}  // namespace cardputer_recorder
