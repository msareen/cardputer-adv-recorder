#include "recorder/recorder_app.h"

#include <functional>

#include "recorder/app/app_shared.h"

// BLE fallback transfer (milestone 6): when WiFi is unavailable, the paired
// desktop connects over BLE, opens the known-host session (hello / hello /
// reveal, then `session`) and sends /v1 requests as encrypted Control
// messages. Wire format: docs/ble-protocol.md, "Sync fallback".

namespace cardputer_recorder {
namespace {

// Plaintext bytes per `body` message. With CBOR and AES-GCM overhead a
// message stays well under kMaxControlMessage (2048).
constexpr std::size_t kBodyChunkBytes = 1536;
constexpr unsigned long kProgressDrawMs = 500;

// Frames a /v1 response as `res`, `body`..., `end` messages.
class BleReply : public SyncReply {
public:
    explicit BleReply(std::function<bool(const CborWriter&)> send)
        : send_(std::move(send))
    {
    }

    bool start(int status, const char* contentType,
               const std::string& contentRange) override
    {
        CborWriter head;
        head.map(4)
            .text("t")
            .text("res")
            .text("s")
            .uint(static_cast<std::uint64_t>(status))
            .text("ct")
            .text(contentType)
            .text("cr")
            .text(contentRange);
        return send_(head);
    }

    bool write(const std::uint8_t* data, std::size_t size) override
    {
        // Small writes (manifest entries) are batched into full chunks.
        while (size > 0) {
            const std::size_t room = kBodyChunkBytes - pending_.size();
            const std::size_t take = size < room ? size : room;
            pending_.append(reinterpret_cast<const char*>(data), take);
            data += take;
            size -= take;
            if (pending_.size() == kBodyChunkBytes && !flush()) {
                return false;
            }
        }
        return true;
    }

    bool finish() override
    {
        if (!pending_.empty() && !flush()) {
            return false;
        }
        CborWriter end;
        end.map(1).text("t").text("end");
        return send_(end);
    }

private:
    bool flush()
    {
        CborWriter body;
        body.map(2).text("t").text("body").text("d").bytes(pending_);
        pending_.clear();
        return send_(body);
    }

    std::function<bool(const CborWriter&)> send_;
    std::string pending_;
};

}  // namespace

void RecorderApp::startBleSync()
{
    // From Settings the index has not been prepared yet; from the
    // WiFi-failed screen it has, and preparing again is cheap.
    syncAutomatic_ = false;
    if (!prepareSync()) {
        return;
    }
    syncServer_.end();
    wifi_.end();

    SyncCallbacks callbacks = makeSyncCallbacks();
    // The handler runs on the main loop here, so draw progress from it
    // (the loop itself is blocked while a file streams).
    callbacks.progress = [this](const std::string& name, std::uint32_t done,
                                std::uint32_t total) {
        onSyncProgress(name, done, total);
        if (millis() - syncLastDrawMs_ >= kProgressDrawMs) {
            syncLastDrawMs_ = millis();
            forceRedraw_ = true;
            draw();
        }
    };
    if (!bleSyncHandler_.begin(storage_, syncIndex_, syncMutex_,
                               peer_.hostId, syncScratch_,
                               SyncHandler::kMinScratchBytes,
                               std::move(callbacks))) {
        stopSync("Bluetooth sync failed.");
        return;
    }

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
    resetPairSession();
    pairForSync_ = true;
    pairPhase_ = PairPhase::kAdvertising;
    if (!ble_.begin(identity_.deviceId(), peer_.paired, info.data(), true)) {
        pairForSync_ = false;
        state_ = State::kSyncing;  // so stopSync restores the browser
        stopSync("Bluetooth failed to start.");
        return;
    }
    state_ = State::kSyncing;
    syncPhase_ = SyncPhase::kBluetooth;
    syncPhaseStartedMs_ = millis();
    syncLastRequestMs_ = millis();
    message_ = "On the desktop: Sync over Bluetooth.";
    Serial.printf("[SYNC] BLE fallback advertising. free heap=%u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));
    forceRedraw_ = true;
}

void RecorderApp::serviceBleSync()
{
    if (ble_.takeDisconnect()) {
        resetPairSession();
        pairPhase_ = PairPhase::kAdvertising;
        message_ = "Desktop disconnected.";
        forceRedraw_ = true;
    }
    std::string frame;
    while (state_ == State::kSyncing && ble_.takeMessage(frame)) {
        handleControlMessage(frame);
    }
    if (state_ != State::kSyncing) {
        return;
    }
    if (syncDirty_.exchange(false)) {
        forceRedraw_ = true;
    }
    if (syncDoneRequested_) {
        const std::uint32_t remaining = syncUnsyncedShown_;
        const std::uint32_t synced = syncUnsyncedAtStart_ > remaining
                                         ? syncUnsyncedAtStart_ - remaining
                                         : 0;
        stopSync("Sync done: " + String(synced) + " synced.");
    } else if (millis() - syncLastRequestMs_ > 10UL * 60UL * 1000UL) {
        stopSync("No desktop connected.");
    }
}

void RecorderApp::handleBleSyncRequest(const CborValue& message)
{
    SyncRequest request;
    request.method = message.text("m");
    request.path = message.text("p");
    if (const CborValue* since = message.find("since")) {
        request.hasSince = since->type == CborValue::Type::kText;
        request.since = since->str;
    }
    if (const CborValue* range = message.find("range")) {
        request.hasRange = range->type == CborValue::Type::kText;
        request.range = range->str;
    }
    request.body = message.text("body");
    if (request.body.size() > kMaxSyncBodyBytes) {
        request.body.clear();
    }
    message_ = "Syncing over Bluetooth...";
    forceRedraw_ = true;

    BleReply reply([this](const CborWriter& out) { return sendSecure(out); });
    if (!bleSyncHandler_.handle(request, reply)) {
        Serial.printf("[SYNC] BLE request %s %s did not finish\n",
                      request.method.c_str(), request.path.c_str());
    }
}

}  // namespace cardputer_recorder
