#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "recorder/sync/pairing_math.h"

class NimBLECharacteristic;
class NimBLEServer;

namespace cardputer_recorder {

// GATT peripheral "Recorder Sync" (docs/ble-protocol.md). Runs only in
// pairing mode and the BLE sync fallback; end() frees the stack. NimBLE callbacks
// run on the BLE host task and only queue data; all protocol logic runs in
// the app's main loop through takeMessage()/send().
class BleService {
public:
    // `deviceId` is the 8-byte id; `info` the CBOR value of the Info
    // characteristic. `syncMode` advertises the BLE sync fallback instead
    // of pairing mode.
    bool begin(const std::uint8_t deviceId[8], bool hasPeer,
               const std::string& info, bool syncMode = false);
    void end();
    bool active() const { return active_; }

    bool connected() const;
    // True once after each disconnect, so the app can reset its session.
    bool takeDisconnect();
    bool takeMessage(std::string& message);
    bool send(const std::string& frame);

    // Called from NimBLE callbacks.
    void onConnect(std::uint16_t handle);
    void onDisconnect();
    void onMtu(std::uint16_t mtu);
    void onWrite(const std::uint8_t* data, std::size_t size);

private:
    bool active_ = false;
    NimBLEServer* server_ = nullptr;
    NimBLECharacteristic* control_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<std::string> inbox_;
    Reassembler reassembler_;
    bool connected_ = false;
    bool disconnected_ = false;
    std::uint16_t mtu_ = 23;
    std::uint16_t connHandle_ = 0;
};

}  // namespace cardputer_recorder
