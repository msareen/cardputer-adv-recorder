#pragma once

#include <cstdint>
#include <string>

namespace cardputer_recorder {

// Device end of the encrypted BLE channel: AES-256-GCM, frame
// 0x01 | counter (8, BE) | ciphertext | tag(16), header as AAD, counters
// strictly increasing per direction. Mirrors SecureChannel in
// desktop/src-tauri/src/pairing.rs.
class SecureChannel {
public:
    void begin(const std::uint8_t key[32]);
    void end();
    bool active() const { return active_; }
    bool seal(const std::string& plaintext, std::string& frame);
    bool open(const std::string& frame, std::string& plaintext);

private:
    std::uint8_t key_[32] = {};
    std::uint64_t sendCounter_ = 0;
    std::uint64_t receiveCounter_ = 0;
    bool active_ = false;
};

}  // namespace cardputer_recorder
