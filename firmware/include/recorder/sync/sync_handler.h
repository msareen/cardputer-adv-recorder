#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "recorder/hardware/storage_service.h"
#include "recorder/sync/protocol.h"
#include "recorder/sync/sync_index.h"

namespace cardputer_recorder {

// What the handler needs from the app, kept as callbacks so it does not
// depend on RecorderApp. They run on whatever task calls handle(): the
// HTTPS server's task for WiFi, the main loop for the BLE fallback.
struct SyncCallbacks {
    std::function<protocol::DeviceInfo()> info;
    std::function<bool(const std::string& name)> isLocked;
    std::function<bool()> deleteAfterSync;
    std::function<bool()> saveIndex;  // called with the index mutex held
    std::function<void(std::int64_t unixMs, std::int32_t tzOffsetMin)>
        setClock;
    std::function<void(const std::string& name, std::uint32_t done,
                       std::uint32_t total)>
        progress;
    std::function<void()> done;
    // Every request, so the app can end sync mode when no desktop comes.
    std::function<void()> request;
};

// One /v1 request, whatever carried it.
struct SyncRequest {
    std::string method;  // "GET", "POST", "DELETE"
    std::string path;    // without the query
    bool hasSince = false;
    std::string since;   // raw `since` query value
    bool hasRange = false;
    std::string range;   // raw Range header value
    std::string body;    // at most kMaxSyncBodyBytes
};

constexpr std::size_t kMaxSyncBodyBytes = 1024;

// Where a response goes. start() once, then write() any number of times,
// then finish(). A false return means the peer went away.
class SyncReply {
public:
    virtual ~SyncReply() = default;
    // `contentRange` is empty unless the status is 206.
    virtual bool start(int status, const char* contentType,
                       const std::string& contentRange) = 0;
    virtual bool write(const std::uint8_t* data, std::size_t size) = 0;
    virtual bool finish() = 0;
};

// The /v1 endpoints over any transport. Streams from the SD card through a
// caller-provided scratch buffer (at least 16 KB), so it allocates no large
// buffers. `indexMutex` guards the sync index against the main loop.
class SyncHandler {
public:
    // Heap is tight while one TLS session is open: 16 KB SD reads.
    static constexpr std::size_t kMinScratchBytes = 16 * 1024;

    bool begin(StorageService& storage, SyncIndex& index,
               std::mutex& indexMutex, const std::string& hostId,
               std::uint8_t* scratch, std::size_t scratchSize,
               SyncCallbacks callbacks);
    // False when the response could not be completed (the transport should
    // drop the connection so the desktop resumes or retries).
    bool handle(const SyncRequest& request, SyncReply& reply);

private:
    bool sendJson(SyncReply& reply, int status, const std::string& body);
    bool sendError(SyncReply& reply, int status, const char* code,
                   const std::string& message);
    bool handleInfo(SyncReply& reply);
    bool handleTime(const SyncRequest& request, SyncReply& reply);
    bool handleManifest(const SyncRequest& request, SyncReply& reply);
    bool handleDownload(const SyncRequest& request, const std::string& id,
                        SyncReply& reply);
    bool handleSha256(const std::string& id, SyncReply& reply);
    bool handleAck(const SyncRequest& request, const std::string& id,
                   SyncReply& reply);
    bool handleDelete(const std::string& id, SyncReply& reply);
    // Cached hash, or computed from the file (and cached).
    bool sha256For(const std::string& id, std::string& sha256);

    StorageService* storage_ = nullptr;
    SyncIndex* index_ = nullptr;
    std::mutex* indexMutex_ = nullptr;
    std::string hostId_;
    std::uint8_t* scratch_ = nullptr;
    std::size_t scratchSize_ = 0;
    SyncCallbacks callbacks_;
};

}  // namespace cardputer_recorder
