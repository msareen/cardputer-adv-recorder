#include "recorder/recorder_app.h"

#include <esp_system.h>
#include <sys/time.h>

#include <cstring>
#include <ctime>

#include "recorder/app/app_shared.h"
#include "recorder/sync/sha256.h"

namespace cardputer_recorder {
namespace {

constexpr const char* kSyncIndexPath = "/RECORDER.SYN";
constexpr const char* kSyncIndexTempPath = "/RECORDER.SYT";
constexpr std::uint16_t kSyncPort = 8443;
constexpr unsigned long kWifiJoinTimeoutMs = 15000;
// Sync mode ends when no desktop has made a request for this long.
constexpr unsigned long kAutoSyncIdleMs = 3UL * 60UL * 1000UL;
constexpr unsigned long kManualSyncIdleMs = 10UL * 60UL * 1000UL;
// After any sync attempt, auto-sync waits this long before trying again.
constexpr unsigned long kAutoSyncRetryMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kAutoSyncCheckMs = 2000;
// Do not start a sync under someone who is using the device.
constexpr unsigned long kAutoSyncIdleInputMs = 15000;
// 2000-01-01: anything earlier is an unset clock or a FAT default date.
constexpr std::time_t kEarliestValidTime = 946684800;

std::string toStd(const String& text)
{
    return std::string(text.c_str());
}

String baseName(const String& path)
{
    return path.startsWith("/") ? path.substring(1) : path;
}

std::uint32_t durationMsFor(const WavInfo& info)
{
    const std::uint32_t bytesPerSecond =
        info.spec.sampleRate * info.spec.channels *
        (info.spec.bitsPerSample / 8);
    if (bytesPerSecond == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(info.dataSize) * 1000ULL /
        bytesPerSecond);
}

bool readWholeFile(File& file, std::string& out)
{
    out.clear();
    const std::size_t size = file.size();
    out.resize(size);
    std::size_t done = 0;
    while (done < size) {
        const std::size_t got = file.read(
            reinterpret_cast<std::uint8_t*>(&out[done]), size - done);
        if (got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

}  // namespace

bool RecorderApp::clockIsSet() const
{
    return std::time(nullptr) > kEarliestValidTime;
}

RecordingIdentity RecorderApp::makeRecordingIdentity()
{
    RecordingIdentity identity;
    esp_fill_random(identity.recordingId, sizeof(identity.recordingId));
    if (identity_.ready()) {
        std::memcpy(identity.deviceId, identity_.deviceId(),
                    sizeof(identity.deviceId));
    }
    if (clockIsSet()) {
        timeval now{};
        gettimeofday(&now, nullptr);
        identity.startUnixMs =
            static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
    }
    return identity;
}

bool RecorderApp::ensureSyncIndex()
{
    if (syncIndexLoaded_) {
        return true;
    }
    if (!storage_.isMounted()) {
        return false;
    }
    syncIndex_ = SyncIndex();
    // A crash between removing the old index and renaming the new one
    // leaves only the temp file, so fall back to it.
    for (const char* path : {kSyncIndexPath, kSyncIndexTempPath}) {
        File file = storage_.open(path, FILE_READ);
        if (!file) {
            continue;
        }
        std::string text;
        const bool read = readWholeFile(file, text);
        file.close();
        if (read && syncIndex_.parse(text)) {
            break;
        }
        syncIndex_ = SyncIndex();
    }
    syncIndexLoaded_ = true;
    return true;
}

bool RecorderApp::saveSyncIndex()
{
    if (!syncIndexLoaded_ || !storage_.isMounted()) {
        return false;
    }
    const std::string text = syncIndex_.serialize();
    if (storage_.exists(kSyncIndexTempPath)) {
        storage_.remove(kSyncIndexTempPath);
    }
    File file = storage_.open(kSyncIndexTempPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    const std::size_t written =
        file.write(reinterpret_cast<const std::uint8_t*>(text.data()),
                   text.size());
    file.close();
    if (written != text.size()) {
        storage_.remove(kSyncIndexTempPath);
        return false;
    }
    if (storage_.exists(kSyncIndexPath) && !storage_.remove(kSyncIndexPath)) {
        return false;
    }
    return storage_.rename(kSyncIndexTempPath, kSyncIndexPath);
}

void RecorderApp::reconcileSyncIndex()
{
    if (!ensureSyncIndex()) {
        return;
    }
    std::vector<CardFile> card;
    card.reserve(files_.size());
    for (const String& name : files_) {
        const String path = "/" + name;
        CardFile file;
        file.name = toStd(name);
        file.size = storage_.fileSize(path.c_str());
        card.push_back(file);
    }
    const std::uint32_t before = syncIndex_.cursor();
    const std::vector<std::string> unknown = syncIndex_.reconcile(card);

    for (const std::string& name : unknown) {
        const String path = String("/") + name.c_str();
        File file = storage_.open(path.c_str(), FILE_READ);
        if (!file) {
            continue;
        }
        const std::uint32_t size = static_cast<std::uint32_t>(file.size());
        const std::time_t modified = file.getLastWrite();
        WavReader reader;
        SyncEntry entry;
        entry.name = name;
        entry.size = size;
        if (reader.begin(file)) {
            entry.durationMs = durationMsFor(reader.info());
            if (reader.hasIdentity()) {
                entry.id = toHex(reader.identity().recordingId, 16);
                entry.createdMs = reader.identity().startUnixMs;
            }
            reader.end();
        } else {
            file.close();
        }
        // Files without a cprc chunk (older firmware), or copies that share
        // another file's id, get an id derived from name, size and SD time.
        if (entry.id.empty() || syncIndex_.findById(entry.id) != nullptr) {
            entry.id = legacyRecordingId(name, size,
                                         static_cast<std::int64_t>(modified));
        }
        if (entry.createdMs == 0 && modified > kEarliestValidTime) {
            entry.createdMs = static_cast<std::int64_t>(modified) * 1000;
        }
        syncIndex_.upsert(entry);
    }
    if (syncIndex_.cursor() != before) {
        saveSyncIndex();
    }
}

void RecorderApp::syncNoteSaved(const String& path,
                                const RecordingIdentity& identity,
                                std::uint32_t fileSize,
                                std::uint32_t dataBytes)
{
    if (!ensureSyncIndex()) {
        return;
    }
    SyncEntry entry;
    entry.id = toHex(identity.recordingId, sizeof(identity.recordingId));
    entry.name = toStd(baseName(path));
    entry.size = fileSize;
    entry.createdMs = identity.startUnixMs;
    WavInfo info;
    info.spec = kRecordingSpec;
    info.dataSize = dataBytes;
    entry.durationMs = durationMsFor(info);
    syncIndex_.upsert(entry);
    if (!saveSyncIndex()) {
        Serial.println("[SYNC] Could not save sync index after recording.");
    }
    refreshUnsyncedCount();
}

void RecorderApp::syncNoteRenamed(const String& from, const String& to)
{
    if (ensureSyncIndex() && syncIndex_.rename(toStd(from), toStd(to))) {
        saveSyncIndex();
    }
    refreshUnsyncedCount();
}

void RecorderApp::refreshUnsyncedCount()
{
    libraryUnsynced_ =
        peer_.paired && ensureSyncIndex()
            ? static_cast<std::uint32_t>(
                  syncIndex_.unsyncedCount(peer_.hostId))
            : 0;
}

void RecorderApp::serviceAutoSync()
{
    const unsigned long now = millis();
    if (now - lastAutoCheckMs_ < kAutoSyncCheckMs) {
        return;
    }
    lastAutoCheckMs_ = now;
    const bool external = power_.externalPowerLikely(battery_);
    if (external != externalPower_) {
        externalPower_ = external;
        forceRedraw_ = true;
    }
    if (!externalPower_) {
        autoSyncSuppressed_ = false;  // unplugging re-arms auto-sync
        return;
    }
    if (!settings_.autoSyncWhenCharging || !peer_.paired ||
        peer_.wifiSsid.empty() || autoSyncSuppressed_ || deleteConfirm_ ||
        !storage_.isMounted() ||
        now - lastUserActivityMs_ < kAutoSyncIdleInputMs ||
        (autoSyncBlocked_ && now - autoSyncBlockedAtMs_ < kAutoSyncRetryMs)) {
        return;
    }
    refreshUnsyncedCount();
    if (libraryUnsynced_ == 0) {
        return;
    }
    Serial.printf("[SYNC] Auto-sync: %u unsynced, on external power\n",
                  static_cast<unsigned>(libraryUnsynced_));
    startSync(true);
    if (state_ != State::kSyncing) {
        autoSyncBlocked_ = true;
        autoSyncBlockedAtMs_ = now;
    }
}

void RecorderApp::syncNoteDeleted(const String& name)
{
    if (ensureSyncIndex() && syncIndex_.removeByName(toStd(name))) {
        saveSyncIndex();
    }
    refreshUnsyncedCount();
}

void RecorderApp::startSync(bool automatic)
{
    syncAutomatic_ = automatic;
    if (!peer_.paired) {
        message_ = "Pair first: G0 > Sync > Pair.";
        forceRedraw_ = true;
        return;
    }
    if (peer_.wifiSsid.empty()) {
        message_ = "No WiFi set. Change it from the desktop.";
        forceRedraw_ = true;
        return;
    }
    if (!storage_.isMounted()) {
        setError("E1: microSD mount failed.");
        return;
    }
    if (shouldAutoSaveForLowBattery()) {
        message_ = "Battery low: charge to sync.";
        forceRedraw_ = true;
        return;
    }
    if (!identity_.ready() && !identity_.begin()) {
        message_ = "Device key unavailable.";
        forceRedraw_ = true;
        return;
    }

    deleteConfirm_ = false;
    message_ = "Indexing recordings...";
    forceRedraw_ = true;
    draw();
    reconcileSyncIndex();
    syncUnsyncedAtStart_ =
        static_cast<std::uint32_t>(syncIndex_.unsyncedCount(peer_.hostId));
    syncUnsyncedShown_ = syncUnsyncedAtStart_;

    if (!wifi_.begin(peer_.wifiSsid.c_str(), peer_.wifiPassword.c_str())) {
        message_ = "WiFi start failed.";
        forceRedraw_ = true;
        return;
    }
    state_ = State::kSyncing;
    syncPhase_ = SyncPhase::kJoining;
    syncPhaseStartedMs_ = millis();
    syncDoneRequested_ = false;
    syncDirty_ = false;
    syncAddress_ = "";
    syncFile_.clear();
    syncDone_ = 0;
    syncTotal_ = 0;
    message_ = String("Joining ") + peer_.wifiSsid.c_str() + "...";
    Serial.printf("[SYNC] Start. free heap=%u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));
    forceRedraw_ = true;
}

void RecorderApp::serviceSync()
{
    if (syncPhase_ == SyncPhase::kJoining) {
        if (wifi_.connected()) {
            if (!clockIsSet()) {
                // Fallback when no desktop sends /v1/time.
                configTime(0, 0, "pool.ntp.org", "time.google.com");
            }
            message_ = "Starting secure server...";
            forceRedraw_ = true;
            draw();

            SyncTls tls;
            if (!identity_.makeTlsCredentials(tls.serverCertificatePem,
                                              tls.serverKeyPem) ||
                !certificateDerToPem(peer_.hostCertificateDer,
                                     tls.clientCertificatePem)) {
                stopSync("Could not build TLS keys.");
                return;
            }

            SyncCallbacks callbacks;
            callbacks.info = [this]() {
                protocol::DeviceInfo info;
                info.deviceId = identity_.deviceIdHex();
                info.firmware = kAppVersion;
                info.batteryPct = battery_.valid ? battery_.levelPercent : -1;
                const std::uint64_t capacity = storage_.capacityBytes();
                const std::uint64_t used = storage_.usedBytes();
                info.freeBytes = capacity > used ? capacity - used : 0;
                std::lock_guard<std::mutex> lock(syncMutex_);
                info.unsynced = static_cast<std::uint32_t>(
                    syncIndex_.unsyncedCount(peer_.hostId));
                info.clockSet = clockIsSet();
                return info;
            };
            callbacks.isLocked = [this](const std::string& name) {
                const String exact = name.c_str();
                String upper = exact;
                upper.toUpperCase();
                return isLocked(exact) || isLocked(upper);
            };
            callbacks.deleteAfterSync = [this]() {
                return settings_.deleteAfterSync;
            };
            // The server holds syncMutex_ when it calls this.
            callbacks.saveIndex = [this]() {
                const bool saved = saveSyncIndex();
                syncUnsyncedShown_ = static_cast<std::uint32_t>(
                    syncIndex_.unsyncedCount(peer_.hostId));
                syncDirty_ = true;
                return saved;
            };
            callbacks.setClock = [](std::int64_t unixMs, std::int32_t) {
                timeval now{};
                now.tv_sec = static_cast<time_t>(unixMs / 1000);
                now.tv_usec = static_cast<suseconds_t>((unixMs % 1000) * 1000);
                settimeofday(&now, nullptr);
            };
            callbacks.progress = [this](const std::string& name,
                                        std::uint32_t done,
                                        std::uint32_t total) {
                onSyncProgress(name, done, total);
            };
            callbacks.done = [this]() { syncDoneRequested_ = true; };
            callbacks.request = [this]() { syncLastRequestMs_ = millis(); };

            // Audio is idle in sync mode, so the 80 KB audio buffers become
            // the transfer buffer instead of a new heap allocation.
            if (!syncServer_.begin(
                    kSyncPort, storage_, syncIndex_, syncMutex_,
                    peer_.hostId,
                    reinterpret_cast<std::uint8_t*>(audioBuffers_),
                    sizeof(audioBuffers_), std::move(tls),
                    std::move(callbacks))) {
                stopSync("Secure server failed.");
                return;
            }
            syncPhase_ = SyncPhase::kServing;
            syncLastRequestMs_ = millis();
            // Paired desktops find the device by id, whatever its address.
            if (!wifi_.advertise("cprec-" + identity_.deviceIdHex(),
                                 kSyncPort, identity_.deviceIdHex())) {
                Serial.println("[SYNC] mDNS advertisement failed.");
            }
            syncAddress_ = wifi_.ipAddress() + ":" + String(kSyncPort);
            message_ = "Waiting for " +
                       String(peer_.hostName.empty() ? "desktop"
                                                     : peer_.hostName.c_str()) +
                       "...";
            Serial.printf(
                "[SYNC] Serving https://%s rssi=%d free heap=%u min=%u\n",
                syncAddress_.c_str(), wifi_.rssi(),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()));
            forceRedraw_ = true;
        } else if (millis() - syncPhaseStartedMs_ > kWifiJoinTimeoutMs) {
            stopSync("WiFi failed: change it from the desktop.");
        }
        return;
    }

    // The HTTPS server runs on its own task; the loop only watches it.
    if (syncDirty_.exchange(false)) {
        forceRedraw_ = true;
    }
    if (syncDoneRequested_) {
        const std::uint32_t remaining = syncUnsyncedShown_;
        const std::uint32_t synced = syncUnsyncedAtStart_ > remaining
                                         ? syncUnsyncedAtStart_ - remaining
                                         : 0;
        stopSync("Sync done: " + String(synced) + " synced.");
    } else if (!wifi_.connected()) {
        stopSync("WiFi lost.");
    } else if (millis() - syncLastRequestMs_ >
               (syncAutomatic_ ? kAutoSyncIdleMs : kManualSyncIdleMs)) {
        stopSync("No desktop connected.");
    }
}

// Runs on the HTTPS server task.
void RecorderApp::onSyncProgress(const std::string& name,
                                 std::uint32_t done, std::uint32_t total)
{
    {
        std::lock_guard<std::mutex> lock(syncMutex_);
        syncFile_ = name;
        syncDone_ = done;
        syncTotal_ = total;
    }
    syncDirty_ = true;
}

void RecorderApp::stopSync(const String& message)
{
    syncServer_.end();
    wifi_.end();
    Serial.printf("[SYNC] Stop: %s min free heap=%u\n", message.c_str(),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    state_ = State::kBrowsing;
    syncAddress_ = "";
    syncFile_.clear();
    scanFiles();
    refreshUnsyncedCount();
    // Whatever happened, auto-sync waits before trying again.
    autoSyncBlocked_ = true;
    autoSyncBlockedAtMs_ = millis();
    syncAutomatic_ = false;
    message_ = message;
    forceRedraw_ = true;
}

void RecorderApp::handleSyncInput(const InputEvent& event)
{
    if (event.back) {
        // The user stopped it: no auto-sync until the device is unplugged.
        autoSyncSuppressed_ = true;
        stopSync("Sync stopped.");
    }
}

void RecorderApp::drawSyncScreen()
{
    auto& display = recorderCanvas;
    constexpr std::uint16_t background = 0x0841;
    constexpr std::uint16_t panel = 0x10C3;
    constexpr std::uint16_t muted = 0x8410;
    constexpr std::uint16_t accent = 0x05FF;

    // Copy progress without blocking the loop if the server holds the lock.
    std::string file;
    std::uint32_t done = 0;
    std::uint32_t total = 0;
    if (syncMutex_.try_lock()) {
        file = syncFile_;
        done = syncDone_;
        total = syncTotal_;
        syncMutex_.unlock();
    }

    display.setTextFont(2);
    display.setTextColor(TFT_WHITE, background);
    display.setCursor(8, 30);
    display.print(syncPhase_ == SyncPhase::kJoining ? "Joining WiFi"
                                                    : syncAddress_);
    display.setTextFont(1);
    display.setTextColor(muted, background);
    display.setCursor(8, 50);
    display.print(message_);

    if (total > 0) {
        display.setCursor(8, 66);
        display.setTextColor(TFT_WHITE, background);
        display.print(file.c_str());
        const std::uint32_t percent = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(done) * 100 / total);
        display.setTextDatum(top_right);
        display.drawString(String(percent) + "%", display.width() - 8, 66);
        display.setTextDatum(top_left);
        display.drawRoundRect(8, 80, display.width() - 16, 10, 4, muted);
        display.fillRoundRect(10, 82,
                              (display.width() - 20) * percent / 100, 6, 3,
                              accent);
    }
    display.setTextColor(muted, background);
    display.setCursor(8, 96);
    display.printf("Unsynced %u", static_cast<unsigned>(syncUnsyncedShown_));

    display.fillRect(0, 111, display.width(), 24, panel);
    display.setTextColor(accent, panel);
    display.setCursor(8, 120);
    display.print(syncAutomatic_ ? "AUTO SYNC" : "SECURE SYNC");
    display.setTextDatum(top_right);
    display.setTextColor(muted, panel);
    display.drawString("ESC STOP", display.width() - 8, 120);
    display.setTextDatum(top_left);
}

}  // namespace cardputer_recorder
