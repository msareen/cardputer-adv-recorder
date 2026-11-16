#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "recorder/hardware/storage_service.h"
#include "recorder/sync/protocol.h"
#include "recorder/sync/sync_index.h"

struct httpd_req;

namespace cardputer_recorder {

// What the server needs from the app, kept as callbacks so the server does
// not depend on RecorderApp. All of them run on the HTTPS server's task.
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

struct SyncTls {
    std::string serverCertificatePem;
    std::string serverKeyPem;
    // The paired desktop's self-signed certificate: the only client
    // certificate the handshake accepts.
    std::string clientCertificatePem;
};

// HTTPS /v1 server with mutual TLS (esp_https_server). Requests run on the
// server's own task, one at a time; `indexMutex` guards the sync index
// against the main loop. Streams from the SD card through a caller-provided
// scratch buffer, so it allocates no large buffers of its own.
class SyncServer {
public:
    bool begin(std::uint16_t port, StorageService& storage,
               SyncIndex& index, std::mutex& indexMutex,
               const std::string& hostId, std::uint8_t* scratch,
               std::size_t scratchSize, SyncTls tls,
               SyncCallbacks callbacks);
    void end();
    bool running() const { return handle_ != nullptr; }

private:
    static int dispatch(httpd_req* request);
    int handle(httpd_req* request);
    int handleInfo(httpd_req* request);
    int handleTime(httpd_req* request);
    int handleManifest(httpd_req* request);
    int handleDownload(httpd_req* request, const std::string& id);
    int handleSha256(httpd_req* request, const std::string& id);
    int handleAck(httpd_req* request, const std::string& id);
    int handleDelete(httpd_req* request, const std::string& id);
    int sendJson(httpd_req* request, int status, const std::string& body);
    int sendError(httpd_req* request, int status, const char* code,
                  const std::string& message);
    bool readBody(httpd_req* request, std::string& body);
    // Cached hash, or computed from the file (and cached).
    bool sha256For(const std::string& id, std::string& sha256);

    void* handle_ = nullptr;
    StorageService* storage_ = nullptr;
    SyncIndex* index_ = nullptr;
    std::mutex* indexMutex_ = nullptr;
    std::string hostId_;
    std::uint8_t* scratch_ = nullptr;
    std::size_t scratchSize_ = 0;
    SyncTls tls_;
    SyncCallbacks callbacks_;
    // Header values must outlive the response they are attached to.
    std::string contentRange_;
};

}  // namespace cardputer_recorder
