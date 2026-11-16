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
    // Blocking scan (a few seconds); WiFi is off again afterwards. Unique
    // SSIDs, strongest first.
    std::vector<std::string> scan(std::size_t maxNetworks);
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
    bool mdnsActive_ = false;
};

}  // namespace cardputer_recorder
