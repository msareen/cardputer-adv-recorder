#include "recorder/recorder_app.h"

#include <esp_heap_caps.h>

#include <cstring>

// The StampS3 has no PSRAM. Recording needs 80 KB of audio buffers; sync
// needs WiFi, one TLS session (about 40 KB of fixed mbedtls record buffers)
// and its transfer buffers. Both fit only if they take turns, so the audio
// buffers live on the heap and are released whenever the radios start.
// States guarantee audio is idle then (sync and pairing are entered only
// from the library or settings).

namespace cardputer_recorder {

bool RecorderApp::ensureAudioBuffers()
{
    for (std::int16_t*& buffer : audioBuffers_) {
        if (buffer != nullptr) {
            continue;
        }
        buffer = static_cast<std::int16_t*>(heap_caps_malloc(
            kAudioSamplesPerBuffer * sizeof(std::int16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (buffer == nullptr) {
            Serial.printf("[RECORDER] Audio buffers: out of memory. "
                          "free=%u largest=%u\n",
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(ESP.getMaxAllocHeap()));
            releaseAudioBuffers();
            return false;
        }
        std::memset(buffer, 0, kAudioSamplesPerBuffer * sizeof(std::int16_t));
    }
    return true;
}

void RecorderApp::releaseAudioBuffers()
{
    for (std::int16_t*& buffer : audioBuffers_) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
}

bool RecorderApp::ensureSyncScratch()
{
    if (syncScratch_ == nullptr) {
        syncScratch_ = static_cast<std::uint8_t*>(heap_caps_malloc(
            SyncHandler::kMinScratchBytes,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return syncScratch_ != nullptr;
}

void RecorderApp::releaseSyncScratch()
{
    heap_caps_free(syncScratch_);
    syncScratch_ = nullptr;
}

}  // namespace cardputer_recorder
