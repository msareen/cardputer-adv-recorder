#pragma once

#include <cstdint>
#include <string>

namespace cardputer_recorder {

// The one desktop this device is paired with (v1: one owner per device),
// plus the WiFi network it provisioned. Stored in NVS namespace cprec_peer.
struct Peer {
    bool paired = false;
    std::uint8_t hostPublicKey[65] = {};
    std::string hostId;    // key id of hostPublicKey
    std::string hostName;  // owner display name from the desktop
    std::string hostCertificateDer;
    std::string wifiSsid;
    std::string wifiPassword;
};

class PeerStore {
public:
    bool load(Peer& peer);
    bool savePeer(const Peer& peer);
    bool saveWifi(const std::string& ssid, const std::string& password);
    // Wipes the peer and WiFi credentials.
    bool forget();
};

}  // namespace cardputer_recorder
