#include "recorder/media/recording_chunk.h"

#include <cstring>

namespace cardputer_recorder {
namespace {

constexpr std::size_t kFormatEnd = 36;  // RIFF header + fmt chunk

std::uint32_t read32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8 |
           static_cast<std::uint32_t>(data[2]) << 16 |
           static_cast<std::uint32_t>(data[3]) << 24;
}

void write32(std::uint8_t* data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8);
    data[2] = static_cast<std::uint8_t>(value >> 16);
    data[3] = static_cast<std::uint8_t>(value >> 24);
}

void writeChunkAndData(std::uint8_t* destination,
                       const std::uint8_t* pcmHeader,
                       const RecordingIdentity& identity)
{
    // pcmHeader is a canonical 44-byte header; splice cprc in before "data".
    std::memcpy(destination, pcmHeader, kFormatEnd);
    std::uint8_t* chunk = destination + kFormatEnd;
    std::memcpy(chunk, "cprc", 4);
    write32(chunk + 4, kCprcPayloadSize);
    encodeCprcPayload(chunk + 8, identity);
    std::memcpy(destination + kFormatEnd + kCprcChunkSize,
                pcmHeader + kFormatEnd, kPcmWavHeaderSize - kFormatEnd);
}

}  // namespace

void encodeCprcPayload(std::uint8_t* destination,
                       const RecordingIdentity& identity)
{
    std::memset(destination, 0, kCprcPayloadSize);
    destination[0] = kCprcVersion;
    std::memcpy(destination + 4, identity.recordingId, 16);
    std::memcpy(destination + 20, identity.deviceId, 8);
    const std::uint64_t start =
        static_cast<std::uint64_t>(identity.startUnixMs);
    write32(destination + 28, static_cast<std::uint32_t>(start));
    write32(destination + 32, static_cast<std::uint32_t>(start >> 32));
}

bool decodeCprcPayload(const std::uint8_t* payload, std::size_t size,
                       RecordingIdentity& output)
{
    output = {};
    if (payload == nullptr || size < kCprcPayloadSize ||
        payload[0] != kCprcVersion) {
        return false;
    }
    std::memcpy(output.recordingId, payload + 4, 16);
    std::memcpy(output.deviceId, payload + 20, 8);
    const std::uint64_t start =
        static_cast<std::uint64_t>(read32(payload + 28)) |
        static_cast<std::uint64_t>(read32(payload + 32)) << 32;
    output.startUnixMs = static_cast<std::int64_t>(start);
    return true;
}

void encodeRecordingWavHeader(std::uint8_t* destination,
                              const WavSpec& spec, std::uint32_t dataSize,
                              const RecordingIdentity& identity)
{
    std::uint8_t pcm[kPcmWavHeaderSize];
    encodePcmWavHeader(pcm, spec, dataSize);
    writeChunkAndData(destination, pcm, identity);
    write32(destination + 4,
            static_cast<std::uint32_t>(kRecordingWavHeaderSize - 8) +
                dataSize);
}

void encodeStreamingRecordingWavHeader(std::uint8_t* destination,
                                       const WavSpec& spec,
                                       const RecordingIdentity& identity)
{
    std::uint8_t pcm[kPcmWavHeaderSize];
    encodeStreamingPcmWavHeader(pcm, spec);
    writeChunkAndData(destination, pcm, identity);
}

bool hasCprcAfterFormat(const std::uint8_t* header, std::size_t size)
{
    return header != nullptr && size >= kFormatEnd + 8 &&
           std::memcmp(header + kFormatEnd, "cprc", 4) == 0 &&
           read32(header + kFormatEnd + 4) == kCprcPayloadSize;
}

WavError parseRecordingWavHeader(const std::uint8_t* header,
                                 std::size_t headerSize,
                                 std::uint32_t fileSize, WavInfo& info,
                                 RecordingIdentity& identity)
{
    info = {};
    identity = {};
    if (header == nullptr || headerSize < kRecordingWavHeaderSize ||
        fileSize < kRecordingWavHeaderSize) {
        return WavError::kTruncated;
    }
    if (!hasCprcAfterFormat(header, headerSize)) {
        return WavError::kMissingData;
    }

    // Rebuild the canonical 44-byte header without the cprc chunk and reuse
    // the PCM parser, then shift offsets back by the chunk size.
    std::uint8_t pcm[kPcmWavHeaderSize];
    std::memcpy(pcm, header, kFormatEnd);
    std::memcpy(pcm + kFormatEnd, header + kFormatEnd + kCprcChunkSize,
                kPcmWavHeaderSize - kFormatEnd);
    const WavError error = parsePcmWavHeader(
        pcm, sizeof(pcm),
        fileSize - static_cast<std::uint32_t>(kCprcChunkSize), info);
    if (error != WavError::kNone) {
        info = {};
        return error;
    }
    info.dataOffset += kCprcChunkSize;
    if (!decodeCprcPayload(header + kFormatEnd + 8, kCprcPayloadSize,
                           identity)) {
        identity = {};
    }
    return WavError::kNone;
}

bool findRecordingIdentity(const std::uint8_t* data, std::size_t size,
                           RecordingIdentity& output)
{
    output = {};
    if (data == nullptr || size < 12 ||
        std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    std::size_t offset = 12;
    while (offset + 8 <= size) {
        const std::uint8_t* chunk = data + offset;
        const std::uint32_t chunkSize = read32(chunk + 4);
        if (std::memcmp(chunk, "data", 4) == 0) {
            return false;
        }
        if (std::memcmp(chunk, "cprc", 4) == 0) {
            if (offset + 8 + chunkSize > size) {
                return false;
            }
            return decodeCprcPayload(chunk + 8, chunkSize, output);
        }
        offset += 8 + static_cast<std::size_t>(chunkSize) + (chunkSize & 1U);
    }
    return false;
}

}  // namespace cardputer_recorder
