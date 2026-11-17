#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

namespace cardputer_recorder {

// Station-mode WiFi used only while syncing. end() turns the radio fully
// off so recording and playback never run with WiFi on.
class WifiService {
public:
    bool begin(const char* ssid, const char* password);
    // Blocking scan (a few seconds). Unique SSIDs, strongest first. WiFi is
    // off again afterwards unless `keepRadioOn`: pairing keeps the radio up
    // until end(), because starting WiFi while Bluetooth runs can abort in
    // the coexistence setup (coex_core_enable).
    std::vector<std::string> scan(std::size_t maxNetworks,
                                  bool keepRadioOn = false);
    bool connected() const;
    String ipAddress() const;
    int rssi() const;
    // Announces _cprec._tcp.local with TXT id=<device id> so paired
    // desktops find the device without an address (milestone 5).
    bool advertise(const std::string& hostname, std::uint16_t port,
                   const std::string& deviceId);
    void end();
    bool active() const { return active_; }

private:
    bool active_ = false;
    bool radioOn_ = false;  // STA mode without a connection (pairing)
    bool mdnsActive_ = false;
};

}  // namespace cardputer_recorder
