#include "recorder/sync/peer_store.h"

#include <Preferences.h>

#include <vector>

namespace cardputer_recorder {
namespace {

constexpr const char* kNamespace = "cprec_peer";

std::string getString(Preferences& preferences, const char* key)
{
    return std::string(preferences.getString(key, "").c_str());
}

}  // namespace

bool PeerStore::load(Peer& peer)
{
    peer = Peer();
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) {
        return false;  // namespace absent until first pairing
    }
    peer.wifiSsid = getString(preferences, "ssid");
    peer.wifiPassword = getString(preferences, "pass");
    peer.hostId = getString(preferences, "host_id");
    peer.hostName = getString(preferences, "host_name");
    const std::size_t certSize = preferences.getBytesLength("host_cert");
    if (certSize > 0 && certSize < 4000) {
        std::vector<std::uint8_t> cert(certSize);
        preferences.getBytes("host_cert", cert.data(), cert.size());
        peer.hostCertificateDer.assign(cert.begin(), cert.end());
    }
    peer.paired =
        preferences.getBytes("host_pk", peer.hostPublicKey,
                             sizeof(peer.hostPublicKey)) ==
            sizeof(peer.hostPublicKey) &&
        !peer.hostId.empty() && !peer.hostCertificateDer.empty();
    preferences.end();
    return true;
}

bool PeerStore::savePeer(const Peer& peer)
{
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        return false;
    }
    const bool ok =
        preferences.putBytes("host_pk", peer.hostPublicKey,
                             sizeof(peer.hostPublicKey)) ==
            sizeof(peer.hostPublicKey) &&
        preferences.putString("host_id", peer.hostId.c_str()) > 0 &&
        preferences.putString("host_name", peer.hostName.c_str()) ==
            peer.hostName.size() &&
        preferences.putBytes("host_cert", peer.hostCertificateDer.data(),
                             peer.hostCertificateDer.size()) ==
            peer.hostCertificateDer.size() &&
        preferences.putString("ssid", peer.wifiSsid.c_str()) ==
            peer.wifiSsid.size() &&
        preferences.putString("pass", peer.wifiPassword.c_str()) ==
            peer.wifiPassword.size();
    preferences.end();
    return ok;
}

bool PeerStore::saveWifi(const std::string& ssid,
                         const std::string& password)
{
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        return false;
    }
    const bool ok =
        preferences.putString("ssid", ssid.c_str()) == ssid.size() &&
        preferences.putString("pass", password.c_str()) == password.size();
    preferences.end();
    return ok;
}

bool PeerStore::forget()
{
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        return false;
    }
    const bool ok = preferences.clear();
    preferences.end();
    return ok;
}

}  // namespace cardputer_recorder
