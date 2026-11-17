#include "recorder/hardware/wifi_service.h"

#include <ESPmDNS.h>
#include <WiFi.h>

namespace cardputer_recorder {

bool WifiService::begin(const char* ssid, const char* password)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return false;
    }
    WiFi.persistent(false);
    // Full throughput while transferring, but keep modem sleep while
    // Bluetooth is up (the WiFi test during pairing): coexistence aborts in
    // coex_core_enable when either radio starts while WiFi runs without
    // modem sleep. The core applies this setting when the station starts.
    WiFi.setSleep(btStarted());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    active_ = true;
    return true;
}

std::vector<std::string> WifiService::scan(std::size_t maxNetworks,
                                           bool keepRadioOn)
{
    std::vector<std::string> networks;
    WiFi.persistent(false);
    // Pairing starts Bluetooth next, with this radio still on.
    WiFi.setSleep(true);
    WiFi.mode(WIFI_STA);
    const int found = WiFi.scanNetworks();
    // scanNetworks returns results sorted by signal strength.
    for (int i = 0; i < found && networks.size() < maxNetworks; ++i) {
        const std::string ssid = WiFi.SSID(i).c_str();
        if (ssid.empty()) {
            continue;  // hidden network
        }
        bool seen = false;
        for (const std::string& existing : networks) {
            seen = seen || existing == ssid;
        }
        if (!seen) {
            networks.push_back(ssid);
        }
    }
    WiFi.scanDelete();
    if (keepRadioOn) {
        radioOn_ = true;
    } else if (!active_ && !radioOn_) {
        WiFi.mode(WIFI_OFF);
    }
    return networks;
}

bool WifiService::connected() const
{
    return active_ && WiFi.status() == WL_CONNECTED;
}

String WifiService::ipAddress() const
{
    return connected() ? WiFi.localIP().toString() : String();
}

int WifiService::rssi() const
{
    return connected() ? WiFi.RSSI() : 0;
}

bool WifiService::advertise(const std::string& hostname, std::uint16_t port,
                            const std::string& deviceId)
{
    if (!connected() || !MDNS.begin(hostname.c_str())) {
        return false;
    }
    mdnsActive_ = MDNS.addService("cprec", "tcp", port);
    if (mdnsActive_) {
        MDNS.addServiceTxt("cprec", "tcp", "id", deviceId.c_str());
        MDNS.addServiceTxt("cprec", "tcp", "v", "1");
    }
    return mdnsActive_;
}

void WifiService::end()
{
    if (mdnsActive_) {
        MDNS.end();
        mdnsActive_ = false;
    }
    if (!active_ && !radioOn_) {
        return;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    // The core remembers the sleep setting across restarts of the radio;
    // put back the default that is safe with Bluetooth.
    WiFi.setSleep(true);
    active_ = false;
    radioOn_ = false;
}

}  // namespace cardputer_recorder
