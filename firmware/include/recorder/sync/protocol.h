#pragma once

#include <cstdint>
#include <string>

#include "recorder/sync/sync_index.h"

namespace cardputer_recorder {
namespace protocol {

// Pure encoding and parsing for the /v1 sync protocol
// (docs/architecture). Shared shape with tools/mock-device.

enum class Route : std::uint8_t {
    kInfo,
    kTime,
    kManifest,
    kDownload,
    kSha256,
    kAck,
    kDelete,
    kDone,
    kNotFound,
    kMethodNotAllowed,
};

struct RouteMatch {
    Route route = Route::kNotFound;
    std::string id;
};

RouteMatch matchRoute(const std::string& method, const std::string& path);
bool isValidRecordingId(const std::string& id);

struct ByteRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;  // inclusive
};

// Parses "bytes=N-" or "bytes=N-M". False when malformed or unsatisfiable.
bool parseRange(const std::string& header, std::uint32_t size,
                ByteRange& range);
bool parseUint32(const std::string& text, std::uint32_t& value);

struct DeviceInfo {
    std::string deviceId;
    std::string firmware;
    int batteryPct = -1;  // -1 when unknown
    std::uint64_t freeBytes = 0;
    std::uint32_t unsynced = 0;
    bool clockSet = false;
};

std::string jsonEscape(const std::string& text);
std::string encodeError(const char* code, const std::string& message);
std::string encodeInfo(const DeviceInfo& info);
std::string encodeManifestHead(std::uint32_t cursor);
std::string encodeManifestEntry(const SyncEntry& entry, bool locked,
                                bool acked);
const char* manifestTail();
std::string encodeSha256(const std::string& sha256);
std::string encodeAckResult(bool acked, bool deleted);
std::string encodeDeleteResult(bool deleted);

// Minimal readers for the flat JSON request bodies of /time and /ack.
bool extractJsonString(const std::string& body, const char* key,
                       std::string& value);
bool extractJsonInt64(const std::string& body, const char* key,
                      std::int64_t& value);

}  // namespace protocol
}  // namespace cardputer_recorder
