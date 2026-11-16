#pragma once

#include <cstddef>
#include <cstdint>

#include "recorder/media/wav_format.h"

namespace cardputer_recorder {

// Identity written into every new recording as a custom RIFF chunk ("cprc")
// placed between "fmt " and "data". Readers that do not know the chunk skip
// it, so files stay playable everywhere.
struct RecordingIdentity {
    std::uint8_t recordingId[16] = {};
    std::uint8_t deviceId[8] = {};
    // Unix time in milliseconds when recording started; 0 when the device
    // clock had not been set yet.
    std::int64_t startUnixMs = 0;
};

constexpr std::uint8_t kCprcVersion = 1;
constexpr std::size_t kCprcPayloadSize = 40;
constexpr std::size_t kCprcChunkSize = 8 + kCprcPayloadSize;
// RIFF(12) + fmt (24) + cprc(48) + data header(8).
constexpr std::size_t kRecordingWavHeaderSize =
    kPcmWavHeaderSize + kCprcChunkSize;
constexpr std::size_t kMaxWavHeaderSize = kRecordingWavHeaderSize;

void encodeCprcPayload(std::uint8_t* destination,
                       const RecordingIdentity& identity);
bool decodeCprcPayload(const std::uint8_t* payload, std::size_t size,
                       RecordingIdentity& output);

void encodeRecordingWavHeader(std::uint8_t* destination,
                              const WavSpec& spec, std::uint32_t dataSize,
                              const RecordingIdentity& identity);
void encodeStreamingRecordingWavHeader(std::uint8_t* destination,
                                       const WavSpec& spec,
                                       const RecordingIdentity& identity);

// True when the first 44 bytes of a file announce the cprc layout written
// by encodeRecordingWavHeader (cprc chunk directly after "fmt ").
bool hasCprcAfterFormat(const std::uint8_t* header, std::size_t size);

// Parses the fixed kRecordingWavHeaderSize-byte layout without seeking.
WavError parseRecordingWavHeader(const std::uint8_t* header,
                                 std::size_t headerSize,
                                 std::uint32_t fileSize, WavInfo& info,
                                 RecordingIdentity& identity);

// Walks RIFF chunks in an in-memory prefix of a file and decodes the cprc
// chunk wherever it appears before "data".
bool findRecordingIdentity(const std::uint8_t* data, std::size_t size,
                           RecordingIdentity& output);

}  // namespace cardputer_recorder
