#include "recorder/sync/protocol.h"

#include <cstdio>

namespace cardputer_recorder {
namespace protocol {
namespace {

constexpr const char* kRecordingsPrefix = "/v1/recordings/";

bool isHexLower(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Position just after `"key":` (whitespace skipped), or npos.
std::size_t findValue(const std::string& body, const char* key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    std::size_t at = body.find(quoted);
    while (at != std::string::npos) {
        std::size_t pos = at + quoted.size();
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' ||
                                     body[pos] == '\n' || body[pos] == '\r')) {
            ++pos;
        }
        if (pos < body.size() && body[pos] == ':') {
            ++pos;
            while (pos < body.size() &&
                   (body[pos] == ' ' || body[pos] == '\t' ||
                    body[pos] == '\n' || body[pos] == '\r')) {
                ++pos;
            }
            return pos;
        }
        at = body.find(quoted, at + 1);
    }
    return std::string::npos;
}

}  // namespace

bool isValidRecordingId(const std::string& id)
{
    if (id.size() != 32) {
        return false;
    }
    for (const char c : id) {
        if (!isHexLower(c)) {
            return false;
        }
    }
    return true;
}

RouteMatch matchRoute(const std::string& method, const std::string& path)
{
    RouteMatch match;
    auto expect = [&](const char* wanted, Route route) {
        match.route = method == wanted ? route : Route::kMethodNotAllowed;
        return match;
    };

    if (path == "/v1/info") {
        return expect("GET", Route::kInfo);
    }
    if (path == "/v1/time") {
        return expect("POST", Route::kTime);
    }
    if (path == "/v1/manifest") {
        return expect("GET", Route::kManifest);
    }
    if (path == "/v1/done") {
        return expect("POST", Route::kDone);
    }

    const std::string prefix = kRecordingsPrefix;
    if (path.compare(0, prefix.size(), prefix) != 0) {
        return match;
    }
    const std::string rest = path.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    const std::string id = rest.substr(0, slash);
    if (!isValidRecordingId(id)) {
        return match;
    }
    match.id = id;
    if (slash == std::string::npos) {
        if (method == "GET") {
            match.route = Route::kDownload;
        } else if (method == "DELETE") {
            match.route = Route::kDelete;
        } else {
            match.route = Route::kMethodNotAllowed;
        }
        return match;
    }
    const std::string action = rest.substr(slash);
    if (action == "/sha256") {
        return expect("GET", Route::kSha256);
    }
    if (action == "/ack") {
        return expect("POST", Route::kAck);
    }
    match.id.clear();
    return match;
}

bool parseUint32(const std::string& text, std::uint32_t& value)
{
    if (text.empty() || text.size() > 10) {
        return false;
    }
    std::uint64_t result = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        result = result * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (result > 0xFFFFFFFFULL) {
        return false;
    }
    value = static_cast<std::uint32_t>(result);
    return true;
}

bool parseRange(const std::string& header, std::uint32_t size,
                ByteRange& range)
{
    const std::string prefix = "bytes=";
    if (size == 0 || header.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::string spec = header.substr(prefix.size());
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos || spec.find(',') != std::string::npos) {
        return false;
    }
    std::uint32_t start = 0;
    if (!parseUint32(spec.substr(0, dash), start)) {
        return false;
    }
    std::uint32_t end = size - 1;
    const std::string endText = spec.substr(dash + 1);
    if (!endText.empty()) {
        if (!parseUint32(endText, end)) {
            return false;
        }
        if (end > size - 1) {
            end = size - 1;
        }
    }
    if (start > end) {
        return false;
    }
    range.start = start;
    range.end = end;
    return true;
}

std::string jsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string encodeError(const char* code, const std::string& message)
{
    return std::string("{\"code\":\"") + code + "\",\"message\":\"" +
           jsonEscape(message) + "\"}";
}

std::string encodeInfo(const DeviceInfo& info)
{
    std::string out = "{\"device_id\":\"" + jsonEscape(info.deviceId) +
                      "\",\"firmware\":\"" + jsonEscape(info.firmware) +
                      "\",\"battery_pct\":";
    out += info.batteryPct < 0 ? "null" : std::to_string(info.batteryPct);
    out += ",\"free_bytes\":" + std::to_string(info.freeBytes);
    out += ",\"unsynced\":" + std::to_string(info.unsynced);
    out += ",\"clock_set\":";
    out += info.clockSet ? "true" : "false";
    out += "}";
    return out;
}

std::string encodeManifestHead(std::uint32_t cursor)
{
    return "{\"cursor\":" + std::to_string(cursor) + ",\"recordings\":[";
}

std::string encodeManifestEntry(const SyncEntry& entry, bool locked,
                                bool acked)
{
    std::string out = "{\"id\":\"" + entry.id + "\",\"name\":\"" +
                      jsonEscape(entry.name) + "\"";
    out += ",\"size\":" + std::to_string(entry.size);
    out += ",\"created_ms\":" + std::to_string(entry.createdMs);
    out += ",\"duration_ms\":" + std::to_string(entry.durationMs);
    out += ",\"locked\":";
    out += locked ? "true" : "false";
    out += ",\"acked\":";
    out += acked ? "true" : "false";
    out += "}";
    return out;
}

const char* manifestTail()
{
    return "]}";
}

std::string encodeSha256(const std::string& sha256)
{
    return "{\"sha256\":\"" + sha256 + "\"}";
}

std::string encodeAckResult(bool acked, bool deleted)
{
    return std::string("{\"acked\":") + (acked ? "true" : "false") +
           ",\"deleted\":" + (deleted ? "true" : "false") + "}";
}

std::string encodeDeleteResult(bool deleted)
{
    return std::string("{\"deleted\":") + (deleted ? "true" : "false") + "}";
}

bool extractJsonString(const std::string& body, const char* key,
                       std::string& value)
{
    std::size_t pos = findValue(body, key);
    if (pos == std::string::npos || pos >= body.size() || body[pos] != '"') {
        return false;
    }
    ++pos;
    std::string out;
    while (pos < body.size() && body[pos] != '"') {
        if (body[pos] == '\\') {
            return false;  // escapes are never needed for our values
        }
        out += body[pos++];
    }
    if (pos >= body.size()) {
        return false;
    }
    value = out;
    return true;
}

bool extractJsonInt64(const std::string& body, const char* key,
                      std::int64_t& value)
{
    std::size_t pos = findValue(body, key);
    if (pos == std::string::npos || pos >= body.size()) {
        return false;
    }
    bool negative = false;
    if (body[pos] == '-') {
        negative = true;
        ++pos;
    }
    std::size_t digits = 0;
    std::int64_t result = 0;
    while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
        if (++digits > 18) {
            return false;
        }
        result = result * 10 + (body[pos] - '0');
        ++pos;
    }
    if (digits == 0) {
        return false;
    }
    value = negative ? -result : result;
    return true;
}

}  // namespace protocol
}  // namespace cardputer_recorder
