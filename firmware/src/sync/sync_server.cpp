#include "recorder/sync/sync_server.h"

#include <Arduino.h>
#include <esp_https_server.h>

#include <cstring>

#include "recorder/sync/sha256.h"

namespace cardputer_recorder {
namespace {

// The protocol streams from SD in 32 KB reads.
constexpr std::size_t kReadChunkBytes = 32 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024;

const char* statusLine(int status)
{
    switch (status) {
        case 200:
            return "200 OK";
        case 204:
            return "204 No Content";
        case 206:
            return "206 Partial Content";
        case 400:
            return "400 Bad Request";
        case 404:
            return "404 Not Found";
        case 405:
            return "405 Method Not Allowed";
        case 409:
            return "409 Conflict";
        case 416:
            return "416 Range Not Satisfiable";
        default:
            return "500 Internal Server Error";
    }
}

const char* methodName(int method)
{
    switch (method) {
        case HTTP_GET:
            return "GET";
        case HTTP_POST:
            return "POST";
        case HTTP_DELETE:
            return "DELETE";
        default:
            return "OTHER";
    }
}

struct EntryCopy {
    bool found = false;
    std::string name;
    std::string sha256;
};

EntryCopy copyEntry(SyncIndex& index, std::mutex& mutex,
                    const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex);
    EntryCopy copy;
    if (const SyncEntry* entry = index.findById(id)) {
        copy.found = true;
        copy.name = entry->name;
        copy.sha256 = entry->sha256;
    }
    return copy;
}

}  // namespace

bool SyncServer::begin(std::uint16_t port, StorageService& storage,
                       SyncIndex& index, std::mutex& indexMutex,
                       const std::string& hostId, std::uint8_t* scratch,
                       std::size_t scratchSize, SyncTls tls,
                       SyncCallbacks callbacks)
{
    end();
    if (scratch == nullptr || scratchSize < kReadChunkBytes ||
        tls.serverCertificatePem.empty() || tls.serverKeyPem.empty() ||
        tls.clientCertificatePem.empty()) {
        return false;
    }
    storage_ = &storage;
    index_ = &index;
    indexMutex_ = &indexMutex;
    hostId_ = hostId;
    scratch_ = scratch;
    scratchSize_ = scratchSize;
    tls_ = std::move(tls);
    callbacks_ = std::move(callbacks);

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = port;
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    config.httpd.stack_size = 10240;
    // Each open TLS session costs heap; the desktop needs one at a time.
    config.httpd.max_open_sockets = 2;
    config.httpd.lru_purge_enable = true;
    // PEM lengths include the terminating NUL.
    config.cacert_pem =
        reinterpret_cast<const std::uint8_t*>(tls_.serverCertificatePem.c_str());
    config.cacert_len = tls_.serverCertificatePem.size() + 1;
    config.prvtkey_pem =
        reinterpret_cast<const std::uint8_t*>(tls_.serverKeyPem.c_str());
    config.prvtkey_len = tls_.serverKeyPem.size() + 1;
    config.client_verify_cert_pem = reinterpret_cast<const std::uint8_t*>(
        tls_.clientCertificatePem.c_str());
    config.client_verify_cert_len = tls_.clientCertificatePem.size() + 1;

    httpd_handle_t handle = nullptr;
    if (httpd_ssl_start(&handle, &config) != ESP_OK) {
        return false;
    }
    for (const httpd_method_t method : {HTTP_GET, HTTP_POST, HTTP_DELETE}) {
        httpd_uri_t route = {};
        route.uri = "/v1/*";
        route.method = method;
        route.handler = &SyncServer::dispatch;
        route.user_ctx = this;
        httpd_register_uri_handler(handle, &route);
    }
    handle_ = handle;
    return true;
}

void SyncServer::end()
{
    if (handle_ != nullptr) {
        httpd_ssl_stop(static_cast<httpd_handle_t>(handle_));
        handle_ = nullptr;
    }
}

int SyncServer::dispatch(httpd_req* request)
{
    return static_cast<SyncServer*>(request->user_ctx)->handle(request);
}

int SyncServer::sendJson(httpd_req* request, int status,
                         const std::string& body)
{
    httpd_resp_set_status(request, statusLine(status));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body.data(), body.size());
}

int SyncServer::sendError(httpd_req* request, int status, const char* code,
                          const std::string& message)
{
    return sendJson(request, status, protocol::encodeError(code, message));
}

bool SyncServer::readBody(httpd_req* request, std::string& body)
{
    body.clear();
    if (request->content_len > kMaxBodyBytes) {
        return false;
    }
    body.resize(request->content_len);
    std::size_t done = 0;
    while (done < body.size()) {
        const int got =
            httpd_req_recv(request, &body[done], body.size() - done);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(got);
    }
    return true;
}

int SyncServer::handle(httpd_req* request)
{
    if (callbacks_.request) {
        callbacks_.request();
    }
    std::string path = request->uri;
    const std::size_t query = path.find('?');
    if (query != std::string::npos) {
        path.resize(query);
    }
    const protocol::RouteMatch match =
        protocol::matchRoute(methodName(request->method), path);

    switch (match.route) {
        case protocol::Route::kInfo:
            return handleInfo(request);
        case protocol::Route::kTime:
            return handleTime(request);
        case protocol::Route::kManifest:
            return handleManifest(request);
        case protocol::Route::kDone: {
            httpd_resp_set_status(request, statusLine(204));
            const int result = httpd_resp_send(request, nullptr, 0);
            if (callbacks_.done) {
                callbacks_.done();
            }
            return result;
        }
        case protocol::Route::kDownload:
            return handleDownload(request, match.id);
        case protocol::Route::kSha256:
            return handleSha256(request, match.id);
        case protocol::Route::kAck:
            return handleAck(request, match.id);
        case protocol::Route::kDelete:
            return handleDelete(request, match.id);
        case protocol::Route::kMethodNotAllowed:
            return sendError(request, 405, "bad_request",
                             "method not allowed");
        default:
            return sendError(request, 404, "not_found", "no such endpoint");
    }
}

int SyncServer::handleInfo(httpd_req* request)
{
    protocol::DeviceInfo info;
    if (callbacks_.info) {
        info = callbacks_.info();
    }
    return sendJson(request, 200, protocol::encodeInfo(info));
}

int SyncServer::handleTime(httpd_req* request)
{
    std::string body;
    std::int64_t unixMs = 0;
    if (!readBody(request, body) ||
        !protocol::extractJsonInt64(body, "unix_ms", unixMs) ||
        unixMs <= 0) {
        return sendError(request, 400, "bad_request", "unix_ms required");
    }
    std::int64_t offset = 0;
    protocol::extractJsonInt64(body, "tz_offset_min", offset);
    if (callbacks_.setClock) {
        callbacks_.setClock(unixMs, static_cast<std::int32_t>(offset));
    }
    return sendJson(request, 200, "{}");
}

int SyncServer::handleManifest(httpd_req* request)
{
    std::uint32_t since = 0;
    bool hasSince = false;
    char query[64];
    char value[16];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) ==
            ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof(value)) ==
            ESP_OK) {
        hasSince = protocol::parseUint32(value, since);
        if (!hasSince) {
            return sendError(request, 400, "bad_request",
                             "since must be a number");
        }
    }

    // Stream entry by entry (a full manifest could be tens of KB). The
    // index lock is held for the whole listing so it is consistent.
    httpd_resp_set_type(request, "application/json");
    std::lock_guard<std::mutex> lock(*indexMutex_);
    std::string head = protocol::encodeManifestHead(index_->cursor());
    if (httpd_resp_send_chunk(request, head.data(), head.size()) != ESP_OK) {
        return ESP_FAIL;
    }
    bool first = true;
    for (const SyncEntry& entry : index_->entries()) {
        if (hasSince && entry.changed <= since) {
            continue;
        }
        const bool locked =
            callbacks_.isLocked && callbacks_.isLocked(entry.name);
        std::string item = protocol::encodeManifestEntry(
            entry, locked, entry.ackedBy == hostId_);
        if (!first) {
            item.insert(item.begin(), ',');
        }
        first = false;
        if (httpd_resp_send_chunk(request, item.data(), item.size()) !=
            ESP_OK) {
            return ESP_FAIL;
        }
    }
    const char* tail = protocol::manifestTail();
    if (httpd_resp_send_chunk(request, tail, std::strlen(tail)) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

int SyncServer::handleDownload(httpd_req* request, const std::string& id)
{
    const EntryCopy entry = copyEntry(*index_, *indexMutex_, id);
    if (!entry.found) {
        return sendError(request, 404, "not_found", "no such recording");
    }
    const std::string path = "/" + entry.name;
    File file = storage_->open(path.c_str(), FILE_READ);
    if (!file) {
        return sendError(request, 500, "sd_error", "could not open recording");
    }
    const std::uint32_t size = static_cast<std::uint32_t>(file.size());
    if (size == 0) {
        file.close();
        return sendError(request, 500, "sd_error", "empty recording");
    }

    protocol::ByteRange range;
    range.start = 0;
    range.end = size - 1;
    bool partial = false;
    const std::size_t rangeLength =
        httpd_req_get_hdr_value_len(request, "Range");
    if (rangeLength > 0 && rangeLength < 64) {
        char header[64];
        httpd_req_get_hdr_value_str(request, "Range", header,
                                    sizeof(header));
        partial = true;
        if (!protocol::parseRange(header, size, range)) {
            file.close();
            return sendError(request, 416, "bad_range", "bad Range");
        }
    }
    if (range.start > 0 && !file.seek(range.start)) {
        file.close();
        return sendError(request, 500, "sd_error", "seek failed");
    }

    const std::uint32_t length = range.end - range.start + 1;
    const bool wholeFile = range.start == 0 && range.end == size - 1;
    httpd_resp_set_type(request, "audio/wav");
    httpd_resp_set_hdr(request, "Accept-Ranges", "bytes");
    if (partial) {
        contentRange_ = "bytes " + std::to_string(range.start) + "-" +
                        std::to_string(range.end) + "/" +
                        std::to_string(size);
        httpd_resp_set_status(request, statusLine(206));
        httpd_resp_set_hdr(request, "Content-Range", contentRange_.c_str());
    }

    // Hash while streaming a whole file, so the desktop's sha256 request
    // right after the download is answered from the cache.
    Sha256 hash;
    const unsigned long startedMs = millis();
    std::uint32_t sent = 0;
    bool ok = true;
    while (sent < length) {
        std::size_t want = length - sent;
        if (want > kReadChunkBytes) {
            want = kReadChunkBytes;
        }
        const std::size_t got = file.read(scratch_, want);
        if (got == 0) {
            ok = false;
            break;
        }
        if (wholeFile) {
            hash.update(scratch_, got);
        }
        if (httpd_resp_send_chunk(request,
                                  reinterpret_cast<const char*>(scratch_),
                                  got) != ESP_OK) {
            ok = false;  // desktop went away; it resumes with Range
            break;
        }
        sent += got;
        if (callbacks_.progress) {
            callbacks_.progress(entry.name, sent, length);
        }
    }
    file.close();

    // Milestone 2 measurement: SD-to-network throughput and heap headroom.
    const unsigned long elapsedMs = millis() - startedMs;
    Serial.printf("[SYNC] %s %s %lu/%lu bytes in %lu ms (%lu KB/s) "
                  "min heap=%u\n",
                  ok ? "sent" : "aborted", entry.name.c_str(),
                  static_cast<unsigned long>(sent),
                  static_cast<unsigned long>(length), elapsedMs,
                  elapsedMs > 0 ? static_cast<unsigned long>(
                                      sent / elapsedMs)
                                : 0UL,
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    if (!ok) {
        return ESP_FAIL;  // closes the connection mid-body
    }
    if (wholeFile && entry.sha256.empty()) {
        std::uint8_t digest[32];
        hash.finish(digest);
        std::lock_guard<std::mutex> lock(*indexMutex_);
        index_->setSha256(id, toHex(digest, sizeof(digest)));
        if (callbacks_.saveIndex) {
            callbacks_.saveIndex();
        }
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

bool SyncServer::sha256For(const std::string& id, std::string& sha256)
{
    const EntryCopy entry = copyEntry(*index_, *indexMutex_, id);
    if (!entry.found) {
        return false;
    }
    if (!entry.sha256.empty()) {
        sha256 = entry.sha256;
        return true;
    }
    const std::string path = "/" + entry.name;
    File file = storage_->open(path.c_str(), FILE_READ);
    if (!file) {
        return false;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(file.size());
    Sha256 hash;
    std::uint32_t done = 0;
    while (done < size) {
        const std::size_t got = file.read(scratch_, kReadChunkBytes);
        if (got == 0) {
            file.close();
            return false;
        }
        hash.update(scratch_, got);
        done += got;
        if (callbacks_.progress) {
            callbacks_.progress(entry.name, done, size);
        }
    }
    file.close();
    std::uint8_t digest[32];
    hash.finish(digest);
    sha256 = toHex(digest, sizeof(digest));
    std::lock_guard<std::mutex> lock(*indexMutex_);
    index_->setSha256(id, sha256);
    if (callbacks_.saveIndex) {
        callbacks_.saveIndex();
    }
    return true;
}

int SyncServer::handleSha256(httpd_req* request, const std::string& id)
{
    if (!copyEntry(*index_, *indexMutex_, id).found) {
        return sendError(request, 404, "not_found", "no such recording");
    }
    std::string sha256;
    if (!sha256For(id, sha256)) {
        return sendError(request, 500, "sd_error",
                         "could not read recording");
    }
    return sendJson(request, 200, protocol::encodeSha256(sha256));
}

int SyncServer::handleAck(httpd_req* request, const std::string& id)
{
    std::string body;
    std::string claimed;
    if (!readBody(request, body) ||
        !protocol::extractJsonString(body, "sha256", claimed)) {
        return sendError(request, 400, "bad_request", "sha256 required");
    }
    const EntryCopy entry = copyEntry(*index_, *indexMutex_, id);
    if (!entry.found) {
        return sendError(request, 404, "not_found", "no such recording");
    }
    std::string actual;
    if (!sha256For(id, actual)) {
        return sendError(request, 500, "sd_error",
                         "could not read recording");
    }
    if (claimed != actual) {
        return sendError(request, 409, "hash_mismatch", "ack hash differs");
    }

    // Delete only after a verified ack, only when the user enabled it, and
    // never a locked file.
    const bool locked =
        callbacks_.isLocked && callbacks_.isLocked(entry.name);
    const bool removeFile = callbacks_.deleteAfterSync &&
                            callbacks_.deleteAfterSync() && !locked;
    bool deleted = false;
    bool saved = true;
    {
        std::lock_guard<std::mutex> lock(*indexMutex_);
        index_->markAcked(id, hostId_);
        if (removeFile) {
            const std::string path = "/" + entry.name;
            if (storage_->remove(path.c_str())) {
                deleted = true;
                index_->removeById(id);
            }
        }
        if (callbacks_.saveIndex) {
            saved = callbacks_.saveIndex();
        }
    }
    if (!saved) {
        return sendError(request, 500, "sd_error",
                         "could not save sync index");
    }
    return sendJson(request, 200, protocol::encodeAckResult(true, deleted));
}

int SyncServer::handleDelete(httpd_req* request, const std::string& id)
{
    const EntryCopy entry = copyEntry(*index_, *indexMutex_, id);
    if (!entry.found) {
        return sendError(request, 404, "not_found", "no such recording");
    }
    if (callbacks_.isLocked && callbacks_.isLocked(entry.name)) {
        return sendError(request, 409, "locked", "recording is locked");
    }
    const std::string path = "/" + entry.name;
    if (!storage_->remove(path.c_str())) {
        return sendError(request, 500, "sd_error", "delete failed");
    }
    {
        std::lock_guard<std::mutex> lock(*indexMutex_);
        index_->removeById(id);
        if (callbacks_.saveIndex) {
            callbacks_.saveIndex();
        }
    }
    return sendJson(request, 200, protocol::encodeDeleteResult(true));
}

}  // namespace cardputer_recorder
