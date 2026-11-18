#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mutex>
#include <vector>

#include "recorder/hardware/audio_service.h"
#include "recorder/hardware/ble_service.h"
#include "recorder/hardware/board_service.h"
#include "recorder/hardware/power_service.h"
#include "recorder/hardware/storage_service.h"
#include "recorder/hardware/wifi_service.h"
#include "recorder/input.h"
#include "recorder/media/recording_chunk.h"
#include "recorder/media/voice_processor.h"
#include "recorder/media/wav_file.h"
#include "recorder/sync/cbor.h"
#include "recorder/sync/device_identity.h"
#include "recorder/sync/peer_store.h"
#include "recorder/sync/secure_channel.h"
#include "recorder/sync/sync_index.h"
#include "recorder/sync/sync_server.h"

namespace cardputer_recorder {

class RecorderApp {
public:
    void begin();
    void update();

private:
    enum class State : std::uint8_t {
        kBrowsing,
        kRecording,
        kSaving,
        kPlaying,
        kSettings,
        kRename,
        kHelp,
        kError,
        // Radios are on only in these two states. kSyncing is entered from
        // kBrowsing (S key), kPairing from Settings > Sync.
        kSyncing,
        kPairing,
    };

    enum class PairPhase : std::uint8_t {
        kScanning,     // WiFi scan for the desktop's network list
        kAdvertising,  // BLE on, waiting for a desktop
        kExchanging,   // hello received, waiting for reveal
        kConfirm,      // showing the code, waiting for Enter
        kAwaitingHost, // Enter pressed, waiting for the pair message
        kSession,      // known desktop connected (change WiFi)
        kJoining,      // trying the provisioned WiFi once
        kDone,
    };

    enum class SyncPhase : std::uint8_t {
        kJoining,
        kServing,
        kWifiFailed,  // offering the BLE fallback (Enter)
        kBluetooth,   // BLE fallback: /v1 over the encrypted BLE channel
    };

    struct Settings {
        std::uint8_t brightnessPercent = 70;
        std::uint8_t idleScreenMode = 1;
        std::uint8_t recordingScreenMode = 1;
        std::uint8_t playbackScreenMode = 1;
        std::uint8_t lowBatterySavePercent = 10;
        std::uint8_t seekStepSeconds = 5;
        std::uint8_t micMode = 1;  // kMicModeVoice
        bool vadEnabled = false;
        bool triplePressWake = false;
        bool deleteAfterSync = false;
        bool autoSyncWhenCharging = true;
    };

    enum class ScreenSaverState : std::uint8_t {
        kAwake,
        kDim,
        kOff,
    };

    enum class SettingsPage : std::uint8_t {
        kMain,
        kScreenSaver,
        kSync,
    };

    void handleInput(const InputEvent& event);
    bool startRecording();
    void serviceRecording();
    bool drainRecording();
    void stopRecording(bool keepFile);
    bool beginSaving();
    void serviceSaving();
    void finishSaving(bool success, const String& detail);
    bool startPlayback();
    void servicePlayback();
    void togglePlaybackPause();
    bool seekPlayback(int seconds);
    bool changePlaybackSpeed(int offset);
    void stopPlayback();
    void openSettings();
    void closeSettings();
    void handleSettingsInput(const InputEvent& event);
    void openHelp();
    void handleHelpInput(const InputEvent& event);
    void beginRenameSelected();
    void handleRenameInput(const InputEvent& event);
    void commitRename();
    void cycleSelectedSetting(int offset);
    void resetSettingsToDefault();
    void loadSettings();
    void saveSettings();
    void applyBrightness();
    String settingValueText(std::uint8_t index) const;
    bool anyInput(const InputEvent& event) const;
    bool screenSaverAllowed() const;
    std::uint8_t screenSaverModeForState() const;
    void serviceScreenSaver();
    void resetScreenSaverTimer();
    void enterScreenSaver(bool manual);
    bool handleScreenSaverWake(const InputEvent& event);
    void resetWakeConfirmation();
    void wakeScreen();
    void drawScreenSaver(unsigned long now);
    void drawSplash(const char* status, unsigned long startedMs);
    void holdSplash(const char* status, unsigned long startedMs);
    void deleteSelected();
    // Settings > Clear Recordings: deletes every unlocked recording after
    // a second Enter press.
    void clearAllRecordings();
    void toggleLockSelected();
    bool isLocked(const String& filename) const;
    void loadLocks();
    bool saveLocks();
    void scanFiles();
    bool chooseRecordingPath(char* path, std::size_t capacity);
    bool primeCapture();
    bool startCaptureWriter();
    void stopCaptureWriter();
    bool queueCaptureBuffer();
    bool serviceCompletedCapture(bool refill);
    void processCaptureBuffer(std::uint8_t bufferIndex);
    static void captureWriterTaskEntry(void* context);
    void captureWriterTask();
    void amplifyPlayback(std::int16_t* samples, std::size_t count) const;
    void amplifyOriginalCapture(std::int16_t* samples,
                                std::size_t count) const;
    void adjustVolume(int offset);
    void updateBattery(bool force = false);
    bool shouldAutoSaveForLowBattery() const;
    unsigned long playbackElapsedMs() const;
    std::uint32_t playbackBytesPerSecond() const;
    std::uint32_t playbackOutputSampleRate() const;
    String storageUsageText() const;
    String selectedRecordingDetail();
    void setError(const String& message);
    void draw();

    // Sync (app/sync_flow.cpp).
    void startSync(bool automatic = false);
    bool prepareSync();
    SyncCallbacks makeSyncCallbacks();
    void serviceAutoSync();
    void refreshUnsyncedCount();
    void serviceSync();
    void stopSync(const String& message);
    void handleSyncInput(const InputEvent& event);
    void drawSyncScreen();
    void onSyncProgress(const std::string& name, std::uint32_t done,
                        std::uint32_t total);
    bool ensureSyncIndex();
    bool saveSyncIndex();
    void reconcileSyncIndex();
    void syncNoteSaved(const String& path,
                       const RecordingIdentity& identity,
                       std::uint32_t fileSize, std::uint32_t dataBytes);
    void syncNoteRenamed(const String& from, const String& to);
    void syncNoteDeleted(const String& name);
    RecordingIdentity makeRecordingIdentity();
    bool clockIsSet() const;

    // BLE fallback transfer (app/ble_sync_flow.cpp).
    void startBleSync();
    void serviceBleSync();
    void handleBleSyncRequest(const CborValue& message);

    // Pairing over BLE (app/pairing_flow.cpp).
    void openPairing();
    void servicePairing();
    void closePairing(const String& message);
    void handlePairingInput(const InputEvent& event);
    void drawPairingScreen();
    void handleControlMessage(const std::string& frame);
    void resetPairSession();
    bool sendPlain(const CborWriter& message);
    bool sendSecure(const CborWriter& message);
    void sendPairError(const char* code);
    void sendNetworks();
    void startPairWifi(const std::string& ssid, const std::string& password);
    void finishPairWifi(bool joined);
    void forgetPairing();

    // Memory handover between audio and the radios (app/memory.cpp).
    bool ensureAudioBuffers();
    void releaseAudioBuffers();
    bool ensureSyncScratch();
    void releaseSyncScratch();

    // M5Unified exposes two microphone queue slots. Extra buffers absorb SD
    // latency while a dedicated task writes completed blocks.
    static constexpr std::size_t kCaptureQueueDepth = 2;
    static constexpr std::size_t kAudioBufferCount = 10;
    static constexpr std::size_t kAudioSamplesPerBuffer = 4096;
    static constexpr std::uint8_t kCaptureStopMarker = 0xFF;

    BoardService board_;
    StorageService storage_;
    AudioService audio_;
    PowerService power_;
    InputController input_;
    WavWriter writer_;
    WavReader reader_;
    Preferences preferences_;

    State state_ = State::kBrowsing;
    Settings settings_;
    std::uint8_t selectedSetting_ = 0;
    std::uint8_t helpPage_ = 0;
    bool resetSettingsConfirm_ = false;
    bool clearRecordingsConfirm_ = false;
    SettingsPage settingsPage_ = SettingsPage::kMain;
    ScreenSaverState screenSaverState_ = ScreenSaverState::kAwake;
    bool screenSaverManual_ = false;
    char wakeConfirmKey_ = '\0';
    std::uint8_t wakeConfirmCount_ = 0;
    unsigned long wakeConfirmLastMs_ = 0;
    unsigned long lastUserActivityMs_ = 0;
    std::vector<String> files_;
    std::vector<String> lockedFiles_;
    int selected_ = 0;
    bool deleteConfirm_ = false;
    String deleteConfirmName_;
    String renameOriginalName_;
    String renameText_;
    // Heap-allocated while recording or playing and released when sync or
    // pairing starts: the radios and one TLS session need that memory
    // (the StampS3 has no PSRAM). See ensureAudioBuffers().
    std::int16_t* audioBuffers_[kAudioBufferCount] = {};
    // SD read buffer for sync (SyncHandler), allocated only in sync mode.
    std::uint8_t* syncScratch_ = nullptr;
    std::uint8_t captureActive_[kCaptureQueueDepth] = {};
    std::size_t captureActiveHead_ = 0;
    std::size_t captureActiveCount_ = 0;
    bool captureQueueActive_ = false;
    QueueHandle_t captureFreeQueue_ = nullptr;
    QueueHandle_t captureReadyQueue_ = nullptr;
    TaskHandle_t captureWriterTaskHandle_ = nullptr;
    std::atomic<bool> captureWriterFailed_{false};
    std::atomic<bool> captureWriterStopped_{true};
    std::size_t captureWriterQueuePeak_ = 0;
    std::uint32_t captureStarvationCount_ = 0;
    std::atomic<std::uint8_t> recordingLevel_{0};
    std::atomic<std::uint32_t> recordingBytes_{0};
    float recordingNoiseFloor_ = 0.0F;
    // Voice mic mode; runs on the writer task. Set before the
    // task starts and not changed while it runs.
    VoiceProcessor voice_;
    bool voiceEnabled_ = false;
    // Original mic mode: the 1.4.0 digital gain and peak compression.
    bool originalCapture_ = false;
    std::uint8_t playbackBufferIndex_ = 0;
    std::uint8_t playbackVolume_ = 255;
    std::uint8_t playbackSpeedIndex_ = 1;
    std::uint32_t playbackDurationMs_ = 0;
    std::uint32_t playbackBaseElapsedMs_ = 0;
    bool playbackPaused_ = false;
    std::uint32_t saveTotalBytes_ = 0;
    std::uint32_t saveCopiedBytes_ = 0;
    bool lowBatteryAutoSave_ = false;
    unsigned long saveSettledAtMs_ = 0;
    bool saveAwaitingSettle_ = false;
    String currentPath_;
    String message_;
    unsigned long operationStartedMs_ = 0;
    unsigned long lastBatteryReadMs_ = 0;
    unsigned long lastDrawMs_ = 0;
    BatteryReading battery_;
    bool forceRedraw_ = true;

    DeviceIdentity identity_;
    WifiService wifi_;
    SyncServer syncServer_;
    // Serves /v1 on the main loop in the BLE fallback.
    SyncHandler bleSyncHandler_;
    unsigned long syncLastDrawMs_ = 0;
    SyncIndex syncIndex_;
    bool syncIndexLoaded_ = false;
    RecordingIdentity currentIdentity_;
    SyncPhase syncPhase_ = SyncPhase::kJoining;
    unsigned long syncPhaseStartedMs_ = 0;
    std::atomic<bool> syncDoneRequested_{false};
    std::atomic<bool> syncDirty_{false};
    // Guards syncIndex_ and the progress fields below while the HTTPS
    // server task is running.
    std::mutex syncMutex_;
    String syncAddress_;
    std::string syncFile_;
    std::uint32_t syncDone_ = 0;
    std::uint32_t syncTotal_ = 0;
    std::uint32_t syncUnsyncedAtStart_ = 0;
    std::uint32_t syncUnsyncedShown_ = 0;
    // Hands-free sync (milestone 5).
    bool syncAutomatic_ = false;
    std::atomic<unsigned long> syncLastRequestMs_{0};
    bool externalPower_ = false;
    bool autoSyncSuppressed_ = false;  // Esc pressed; cleared on unplug
    unsigned long autoSyncBlockedAtMs_ = 0;
    bool autoSyncBlocked_ = false;
    unsigned long lastAutoCheckMs_ = 0;
    std::uint32_t libraryUnsynced_ = 0;
    // Local minus UTC, from the desktop with the time (set on the httpd task).
    std::atomic<std::int32_t> tzOffsetMin_{0};

    BleService ble_;
    PeerStore peerStore_;
    Peer peer_;
    SecureChannel pairChannel_;
    PairPhase pairPhase_ = PairPhase::kScanning;
    unsigned long pairStartedMs_ = 0;
    unsigned long pairJoinStartedMs_ = 0;
    std::uint8_t pairHostKey_[65] = {};
    std::uint8_t pairCommit_[32] = {};
    std::uint8_t pairDeviceNonce_[16] = {};
    bool pairGotHello_ = false;
    bool pairHostKnown_ = false;
    // BLE is up for the sync fallback, not pairing: only the paired desktop
    // may connect, and it sends /v1 requests instead of pair/set_wifi.
    bool pairForSync_ = false;
    std::string pairCode_;
    String pairResult_;
    std::vector<std::string> pairNetworks_;
    std::string pairJoinSsid_;
    bool forgetConfirm_ = false;
};

}  // namespace cardputer_recorder
