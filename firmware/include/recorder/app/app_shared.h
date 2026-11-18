#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>

#include "recorder/hardware/audio_service.h"
#include "recorder/media/wav_format.h"

namespace cardputer_recorder {

constexpr WavSpec kRecordingSpec{16000, 1, 16};
constexpr AudioFormat kAudioFormat{16000, 1, 16};
constexpr std::uint64_t kMinimumFreeBytes = 64 * 1024;
constexpr unsigned long kActiveDrawIntervalMs = 200;
constexpr std::uint8_t kPlaybackQueueCapacity = 2;
constexpr unsigned long kCaptureStartTimeoutMs = 1000;
constexpr unsigned long kCaptureDrainTimeoutMs = 1000;
constexpr unsigned long kSaveSettleMs = 750;
constexpr unsigned long kRecordingStopGuardMs = 500;
constexpr std::uint8_t kSettingsCount = 8;
constexpr std::uint8_t kSettingMicMode = 4;
constexpr std::uint8_t kSettingSync = 5;
constexpr std::uint8_t kSettingReset = 6;
constexpr std::uint8_t kSettingVersion = 7;
// Settings > Mic Mode values (RecorderApp::Settings::micMode).
constexpr std::uint8_t kMicModeNatural = 0;
constexpr std::uint8_t kMicModeVoice = 1;
constexpr std::uint8_t kMicModeCount = 2;
constexpr std::uint8_t kScreenSaverSettingsCount = 4;
// Settings > Sync rows.
constexpr std::uint8_t kSyncSettingsCount = 7;
constexpr std::uint8_t kSyncSettingPair = 0;
constexpr std::uint8_t kSyncSettingBluetooth = 1;
constexpr std::uint8_t kSyncSettingPeer = 2;
constexpr std::uint8_t kSyncSettingWifi = 3;
constexpr std::uint8_t kSyncSettingDeleteAfterSync = 4;
constexpr std::uint8_t kSyncSettingAutoSync = 5;
constexpr std::uint8_t kSyncSettingForget = 6;
constexpr unsigned long kIdleScreenSaverDelayMs = 30000;
constexpr unsigned long kActiveScreenSaverDelayMs = 15000;
constexpr std::uint8_t kDimBrightness = 12;
constexpr const char* kAppVersion = "1.5.0-dev";

extern M5Canvas recorderCanvas;

String formatTime(unsigned long milliseconds);
String formatByteCount(std::uint64_t bytes);
const char* screenModeText(std::uint8_t mode);
const char* playbackSpeedText(std::uint8_t index);
const char* lowBatterySaveText(std::uint8_t percent);
const char* seekStepText(std::uint8_t seconds);
const char* micModeText(std::uint8_t mode);

}  // namespace cardputer_recorder
