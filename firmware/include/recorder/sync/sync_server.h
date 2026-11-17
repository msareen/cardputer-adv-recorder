#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "recorder/sync/sync_handler.h"

struct httpd_req;

namespace cardputer_recorder {

struct SyncTls {
    std::string serverCertificatePem;
    std::string serverKeyPem;
    // The paired desktop's self-signed certificate: the only client
    // certificate the handshake accepts.
    std::string clientCertificatePem;
};

// HTTPS /v1 server with mutual TLS (esp_https_server). Requests run on the
// server's own task, one at a time, and go to a SyncHandler (shared with
// the BLE fallback).
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

    void* handle_ = nullptr;
    SyncHandler handler_;
    SyncTls tls_;
};

}  // namespace cardputer_recorder
