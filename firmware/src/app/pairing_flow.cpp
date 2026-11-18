#include "recorder/recorder_app.h"

#include <esp_system.h>

#include <cstring>

#include "recorder/app/app_shared.h"
#include "recorder/sync/pairing_math.h"

namespace cardputer_recorder {
namespace {

// Pairing mode closes itself after two minutes (architecture doc).
constexpr unsigned long kPairingTimeoutMs = 120000;
constexpr unsigned long kPairWifiTimeoutMs = 15000;
constexpr std::size_t kMaxNetworks = 12;
constexpr std::size_t kMaxSsid = 32;
constexpr std::size_t kMaxPassword = 63;
constexpr std::size_t kMaxHostName = 32;

}  // namespace

void RecorderApp::openPairing()
{
    if (!identity_.ready() && !identity_.begin()) {
        message_ = "Device key unavailable.";
        forceRedraw_ = true;
        return;
    }
    releaseAudioBuffers();  // WiFi and BLE need the memory
    state_ = State::kPairing;
    pairPhase_ = PairPhase::kScanning;
    pairResult_ = "";
    resetPairSession();
    message_ = "Scanning WiFi...";
    forceRedraw_ = true;
    draw();

    // Scan before BLE starts, so the desktop can offer a list of networks
    // and the user never types an SSID on the Cardputer.
    // The radio stays on (idle) until closePairing, so the WiFi test never
    // starts WiFi while Bluetooth is up.
    pairNetworks_ = wifi_.scan(kMaxNetworks, true);

    CborWriter info;
    info.map(4)
        .text("v")
        .uint(1)
        .text("id")
        .text(identity_.deviceIdHex())
        .text("fw")
        .text(kAppVersion)
        .text("paired")
        .boolean(peer_.paired);
    if (!ble_.begin(identity_.deviceId(), peer_.paired, info.data())) {
        closePairing("Bluetooth failed to start.");
        return;
    }
    pairPhase_ = PairPhase::kAdvertising;
    pairStartedMs_ = millis();
    message_ = "On the desktop: Devices > Pair.";
    Serial.printf("[PAIR] Advertising. free heap=%u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));
    forceRedraw_ = true;
}

void RecorderApp::resetPairSession()
{
    pairChannel_.end();
    pairGotHello_ = false;
    pairHostKnown_ = false;
    pairCode_.clear();
    std::memset(pairHostKey_, 0, sizeof(pairHostKey_));
    std::memset(pairCommit_, 0, sizeof(pairCommit_));
    std::memset(pairDeviceNonce_, 0, sizeof(pairDeviceNonce_));
}

void RecorderApp::closePairing(const String& message)
{
    resetPairSession();
    ble_.end();
    wifi_.end();
    Serial.printf("[PAIR] Closed: %s min free heap=%u\n", message.c_str(),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    state_ = State::kSettings;
    settingsPage_ = SettingsPage::kSync;
    selectedSetting_ = 0;
    message_ = message;
    forceRedraw_ = true;
}

bool RecorderApp::sendPlain(const CborWriter& message)
{
    std::string frame(1, static_cast<char>(kFramePlain));
    frame += message.data();
    return ble_.send(frame);
}

bool RecorderApp::sendSecure(const CborWriter& message)
{
    std::string frame;
    return pairChannel_.seal(message.data(), frame) && ble_.send(frame);
}

void RecorderApp::sendPairError(const char* code)
{
    CborWriter error;
    error.map(2).text("t").text("error").text("e").text(code);
    sendPlain(error);
}

void RecorderApp::sendNetworks()
{
    CborWriter networks;
    networks.map(2).text("t").text("networks").text("list").array(
        pairNetworks_.size());
    for (const std::string& ssid : pairNetworks_) {
        networks.text(ssid);
    }
    sendSecure(networks);
}

void RecorderApp::startPairWifi(const std::string& ssid,
                                const std::string& password)
{
    pairJoinSsid_ = ssid;
    pairPhase_ = PairPhase::kJoining;
    pairJoinStartedMs_ = millis();
    message_ = ssid.empty() ? String("Bluetooth only.")
                            : String("Joining ") + ssid.c_str() + "...";
    forceRedraw_ = true;
    Serial.printf("[PAIR] Testing WiFi. free heap=%u largest=%u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
    if (ssid.empty() || !wifi_.begin(ssid.c_str(), password.c_str())) {
        // Reported as a failed join on the next service pass.
        pairJoinStartedMs_ = millis() - kPairWifiTimeoutMs - 1;
    }
}

void RecorderApp::handleControlMessage(const std::string& frame)
{
    if (frame.empty()) {
        return;
    }
    const auto type = static_cast<std::uint8_t>(frame[0]);
    CborValue message;

    if (type == kFramePlain) {
        if (!cborDecode(reinterpret_cast<const std::uint8_t*>(frame.data()) + 1,
                        frame.size() - 1, message) ||
            message.type != CborValue::Type::kMap) {
            return;
        }
        const std::string kind = message.text("t");

        if (kind == "hello" && (pairPhase_ == PairPhase::kAdvertising ||
                                pairPhase_ == PairPhase::kExchanging)) {
            const std::string hostKey = message.bytesOf("pk");
            const std::string commit = message.bytesOf("c");
            if (hostKey.size() != sizeof(pairHostKey_) ||
                commit.size() != sizeof(pairCommit_)) {
                sendPairError("bad hello");
                return;
            }
            resetPairSession();
            std::memcpy(pairHostKey_, hostKey.data(), sizeof(pairHostKey_));
            std::memcpy(pairCommit_, commit.data(), sizeof(pairCommit_));
            esp_fill_random(pairDeviceNonce_, sizeof(pairDeviceNonce_));
            pairHostKnown_ =
                peer_.paired &&
                std::memcmp(pairHostKey_, peer_.hostPublicKey,
                            sizeof(pairHostKey_)) == 0;
            if (pairForSync_ && !pairHostKnown_) {
                sendPairError("not_paired");
                resetPairSession();
                return;
            }
            pairGotHello_ = true;

            CborWriter reply;
            reply.map(4)
                .text("t")
                .text("hello")
                .text("pk")
                .bytes(identity_.publicKey(), kPublicKeySize)
                .text("n")
                .bytes(pairDeviceNonce_, sizeof(pairDeviceNonce_))
                .text("known")
                .boolean(pairHostKnown_);
            sendPlain(reply);
            pairPhase_ = PairPhase::kExchanging;
            message_ = "Connecting...";
            forceRedraw_ = true;
            return;
        }

        if (kind == "reveal" && pairPhase_ == PairPhase::kExchanging &&
            pairGotHello_) {
            const std::string hostNonce = message.bytesOf("n");
            std::uint8_t expected[32];
            if (hostNonce.size() != kPairNonceSize) {
                sendPairError("bad reveal");
                return;
            }
            pairingCommitment(
                pairHostKey_,
                reinterpret_cast<const std::uint8_t*>(hostNonce.data()),
                expected);
            if (std::memcmp(expected, pairCommit_, sizeof(expected)) != 0) {
                // The desktop changed its nonce after seeing ours.
                sendPairError("commitment");
                resetPairSession();
                pairPhase_ = PairPhase::kAdvertising;
                return;
            }
            std::uint8_t shared[32];
            std::uint8_t key[32];
            if (!identity_.ecdh(pairHostKey_, shared)) {
                sendPairError("bad key");
                resetPairSession();
                pairPhase_ = PairPhase::kAdvertising;
                return;
            }
            const auto* nonce =
                reinterpret_cast<const std::uint8_t*>(hostNonce.data());
            pairingSessionKey(shared, pairHostKey_, identity_.publicKey(),
                              nonce, pairDeviceNonce_, key);
            pairChannel_.begin(key);
            std::memset(shared, 0, sizeof(shared));
            std::memset(key, 0, sizeof(key));

            if (pairHostKnown_) {
                CborWriter session;
                session.map(1).text("t").text("session");
                sendSecure(session);
                pairPhase_ = PairPhase::kSession;
                if (pairForSync_) {
                    message_ = "Desktop connected.";
                } else {
                    sendNetworks();
                    message_ = "Connected. Waiting for WiFi.";
                }
            } else {
                pairCode_ = pairingCode(pairHostKey_, identity_.publicKey(),
                                        nonce, pairDeviceNonce_);
                pairPhase_ = PairPhase::kConfirm;
                message_ = "Compare the code.";
            }
            forceRedraw_ = true;
        }
        return;
    }

    if (type != kFrameSecure || !pairChannel_.active()) {
        return;
    }
    std::string plain;
    if (!pairChannel_.open(frame, plain) ||
        !cborDecode(reinterpret_cast<const std::uint8_t*>(plain.data()),
                    plain.size(), message) ||
        message.type != CborValue::Type::kMap) {
        return;
    }
    const std::string kind = message.text("t");
    if (pairForSync_) {
        if (kind == "req" && pairPhase_ == PairPhase::kSession) {
            handleBleSyncRequest(message);
        }
        return;  // no pairing or WiFi changes over the sync fallback
    }
    const std::string ssid = message.text("ssid");
    const std::string password = message.text("pass");
    if (ssid.size() > kMaxSsid || password.size() > kMaxPassword) {
        sendPairError("bad wifi");
        return;
    }

    // Only reachable after Enter was pressed on this device.
    if (kind == "pair" && pairPhase_ == PairPhase::kAwaitingHost &&
        !pairHostKnown_) {
        const std::string certificate = message.bytesOf("cert");
        if (!certificateHasKey(certificate, pairHostKey_)) {
            sendPairError("bad certificate");
            closePairing("Pairing failed: bad certificate.");
            return;
        }
        Peer peer;
        peer.paired = true;
        std::memcpy(peer.hostPublicKey, pairHostKey_,
                    sizeof(peer.hostPublicKey));
        peer.hostId = keyIdHex(pairHostKey_);
        peer.hostName = message.text("name").substr(0, kMaxHostName);
        peer.hostCertificateDer = certificate;
        peer.wifiSsid = ssid;
        peer.wifiPassword = password;
        if (!peerStore_.savePeer(peer)) {
            sendPairError("storage");
            closePairing("Pairing failed: could not save.");
            return;
        }
        // One owner per device: acks from a previous desktop no longer
        // count.
        const bool replacedOtherHost =
            peer_.paired && peer_.hostId != peer.hostId;
        peer_ = peer;
        if (replacedOtherHost && ensureSyncIndex()) {
            syncIndex_.clearAcks();
            saveSyncIndex();
        }
        Serial.printf("[PAIR] Paired with %s (%s)\n", peer_.hostName.c_str(),
                      peer_.hostId.c_str());
        refreshUnsyncedCount();
        startPairWifi(ssid, password);
        return;
    }

    if (kind == "set_wifi" && pairPhase_ == PairPhase::kSession &&
        pairHostKnown_) {
        if (!peerStore_.saveWifi(ssid, password)) {
            sendPairError("storage");
            return;
        }
        peer_.wifiSsid = ssid;
        peer_.wifiPassword = password;
        startPairWifi(ssid, password);
    }
}

void RecorderApp::servicePairing()
{
    if (pairPhase_ == PairPhase::kScanning || pairPhase_ == PairPhase::kDone) {
        return;
    }

    if (ble_.takeDisconnect() && pairPhase_ != PairPhase::kJoining) {
        resetPairSession();
        pairPhase_ = PairPhase::kAdvertising;
        message_ = "Desktop disconnected.";
        forceRedraw_ = true;
    }

    std::string frame;
    while (ble_.takeMessage(frame)) {
        handleControlMessage(frame);
        if (state_ != State::kPairing) {
            return;
        }
    }

    if (pairPhase_ == PairPhase::kJoining) {
        const bool joined = wifi_.connected();
        if (joined ||
            millis() - pairJoinStartedMs_ > kPairWifiTimeoutMs) {
            const String ip = joined ? wifi_.ipAddress() : String();
            CborWriter result;
            result.map(3)
                .text("t")
                .text("wifi")
                .text("ok")
                .boolean(joined)
                .text("ip")
                .text(ip.c_str());
            sendSecure(result);
            // WiFi goes off in closePairing, after Bluetooth.
            pairPhase_ = PairPhase::kDone;
            Serial.printf("[PAIR] WiFi test %s\n", joined ? "ok" : "failed");
            pairResult_ =
                pairJoinSsid_.empty()
                    ? String("Bluetooth only: press S to sync.")
                : joined ? String("WiFi OK: ") + pairJoinSsid_.c_str()
                         : String("WiFi failed: ") + pairJoinSsid_.c_str();
            message_ = peer_.hostName.empty()
                           ? String("Paired.")
                           : String("Paired with ") +
                                 peer_.hostName.c_str() + ".";
            forceRedraw_ = true;
        }
        return;
    }

    if (millis() - pairStartedMs_ > kPairingTimeoutMs) {
        sendPairError("timeout");
        closePairing("Pairing timed out.");
        return;
    }
    // Redraw once a second for the countdown.
    static unsigned long lastTickMs = 0;
    if (millis() - lastTickMs >= 1000) {
        lastTickMs = millis();
        forceRedraw_ = true;
    }
}

void RecorderApp::handlePairingInput(const InputEvent& event)
{
    if (pairPhase_ == PairPhase::kDone) {
        if (event.confirm || event.back) {
            closePairing(pairResult_);
        }
        return;
    }
    if (event.back) {
        sendPairError("cancelled");
        closePairing("Pairing cancelled.");
        return;
    }
    // The physical Enter press that numeric comparison requires.
    if (event.confirm && pairPhase_ == PairPhase::kConfirm) {
        CborWriter confirmed;
        confirmed.map(1).text("t").text("confirmed");
        sendSecure(confirmed);
        sendNetworks();
        pairPhase_ = PairPhase::kAwaitingHost;
        message_ = "Finish on the desktop.";
        forceRedraw_ = true;
    }
}

void RecorderApp::forgetPairing()
{
    peerStore_.forget();
    peer_ = Peer();
    if (ensureSyncIndex()) {
        syncIndex_.clearAcks();
        saveSyncIndex();
    }
    refreshUnsyncedCount();
    message_ = "Pairing and WiFi forgotten.";
    forceRedraw_ = true;
}

void RecorderApp::drawPairingScreen()
{
    auto& display = recorderCanvas;
    constexpr std::uint16_t background = 0x0841;
    constexpr std::uint16_t panel = 0x10C3;
    constexpr std::uint16_t muted = 0x8410;
    constexpr std::uint16_t accent = 0x05FF;

    const char* footerLeft = "ESC CANCEL";
    const char* footerRight = "";
    display.setTextColor(TFT_WHITE, background);

    if (pairPhase_ == PairPhase::kConfirm) {
        display.setTextFont(2);
        display.setTextColor(muted, background);
        display.setCursor(8, 30);
        display.print("Same code on the desktop?");
        const String code = String(pairCode_.substr(0, 3).c_str()) + " " +
                            pairCode_.substr(3).c_str();
        display.setTextFont(4);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, background);
        display.drawString(code, display.width() / 2, 70);
        display.setTextDatum(top_left);
        footerRight = "ENTER IF SAME";
    } else {
        display.setTextFont(2);
        display.setCursor(8, 30);
        switch (pairPhase_) {
            case PairPhase::kScanning:
                display.print("Scanning WiFi");
                break;
            case PairPhase::kAdvertising:
                display.print("Pair with desktop");
                break;
            case PairPhase::kJoining:
                display.print("Testing WiFi");
                break;
            case PairPhase::kDone:
                display.print(pairResult_);
                footerLeft = "";
                footerRight = "ENTER CLOSE";
                break;
            default:
                display.print("Pairing");
                break;
        }
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setCursor(8, 54);
        display.print(message_);
        if (pairPhase_ == PairPhase::kAdvertising ||
            pairPhase_ == PairPhase::kExchanging ||
            pairPhase_ == PairPhase::kAwaitingHost ||
            pairPhase_ == PairPhase::kSession) {
            const unsigned long elapsed = millis() - pairStartedMs_;
            const unsigned long left =
                elapsed < kPairingTimeoutMs
                    ? (kPairingTimeoutMs - elapsed) / 1000
                    : 0;
            display.setCursor(8, 72);
            display.printf("Device %s", identity_.deviceIdHex().c_str());
            display.setCursor(8, 88);
            display.printf("Closes in %lu s", left);
        }
    }

    display.fillRect(0, 111, display.width(), 24, panel);
    display.setTextFont(1);
    display.setTextColor(muted, panel);
    display.setCursor(8, 120);
    display.print(footerLeft);
    display.setTextDatum(top_right);
    display.setTextColor(accent, panel);
    display.drawString(footerRight, display.width() - 8, 120);
    display.setTextDatum(top_left);
}

}  // namespace cardputer_recorder
