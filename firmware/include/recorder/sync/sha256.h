#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cardputer_recorder {

// Small portable SHA-256 (FIPS 180-4). Used for file hashes during sync and
// for legacy recording ids. SD reads, not hashing, limit sync throughput.
class Sha256 {
public:
    Sha256();
    void reset();
    void update(const void* data, std::size_t size);
    void finish(std::uint8_t digest[32]);

private:
    void transform(const std::uint8_t block[64]);

    std::uint32_t state_[8];
    std::uint64_t bitCount_;
    std::uint8_t buffer_[64];
    std::size_t bufferSize_;
};

std::string toHex(const std::uint8_t* data, std::size_t size);
std::string sha256Hex(const void* data, std::size_t size);

}  // namespace cardputer_recorder
