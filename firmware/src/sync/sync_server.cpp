#include "recorder/sync/sync_server.h"

#include <Arduino.h>
#include <esp_https_server.h>

#include <cstring>

namespace cardputer_recorder {
namespace {

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

bool readBody(httpd_req* request, std::string& body)
{
    body.clear();
    if (request->content_len > kMaxSyncBodyBytes) {
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

// A response with one body write goes out with Content-Length; longer ones
// (manifest, downloads) are chunked.
class HttpReply : public SyncReply {
public:
    explicit HttpReply(httpd_req* request) : request_(request) {}

    bool start(int status, const char* contentType,
               const std::string& contentRange) override
    {
        httpd_resp_set_status(request_, statusLine(status));
        httpd_resp_set_type(request_, contentType);
        if (std::strcmp(contentType, "audio/wav") == 0) {
            httpd_resp_set_hdr(request_, "Accept-Ranges", "bytes");
        }
        if (!contentRange.empty()) {
            // Header values must outlive the response.
            contentRange_ = contentRange;
            httpd_resp_set_hdr(request_, "Content-Range",
                               contentRange_.c_str());
        }
        return true;
    }

    bool write(const std::uint8_t* data, std::size_t size) override
    {
        // Hold back only a small first write (a JSON reply), never a 32 KB
        // download chunk: copying that does not fit in the heap.
        if (!chunked_ && !hasPending_ && size <= kMaxHeldBytes) {
            pending_.assign(reinterpret_cast<const char*>(data), size);
            hasPending_ = true;
            return true;
        }
        if (!flushPending()) {
            return false;
        }
        return httpd_resp_send_chunk(request_,
                                     reinterpret_cast<const char*>(data),
                                     size) == ESP_OK;
    }

    bool finish() override
    {
        if (!chunked_) {
            return httpd_resp_send(request_, pending_.data(),
                                   pending_.size()) == ESP_OK;
        }
        return httpd_resp_send_chunk(request_, nullptr, 0) == ESP_OK;
    }

private:
    bool flushPending()
    {
        chunked_ = true;
        if (!hasPending_) {
            return true;
        }
        hasPending_ = false;
        const bool ok = httpd_resp_send_chunk(request_, pending_.data(),
                                              pending_.size()) == ESP_OK;
        pending_.clear();
        return ok;
    }

    static constexpr std::size_t kMaxHeldBytes = 1024;

    httpd_req* request_;
    std::string contentRange_;
    std::string pending_;
    bool hasPending_ = false;
    bool chunked_ = false;
};

}  // namespace

bool SyncServer::begin(std::uint16_t port, StorageService& storage,
                       SyncIndex& index, std::mutex& indexMutex,
                       const std::string& hostId, std::uint8_t* scratch,
                       std::size_t scratchSize, SyncTls tls,
                       SyncCallbacks callbacks)
{
    end();
    if (tls.serverCertificatePem.empty() || tls.serverKeyPem.empty() ||
        tls.clientCertificatePem.empty() ||
        !handler_.begin(storage, index, indexMutex, hostId, scratch,
                        scratchSize, std::move(callbacks))) {
        return false;
    }
    tls_ = std::move(tls);

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = port;
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    config.httpd.stack_size = 10240;
    // Each TLS session costs about 40 KB (16 KB in and out record buffers
    // are fixed in the prebuilt mbedtls), and two leave the heap nearly
    // empty mid-download (measured: 7.6 KB). The desktop needs one at a
    // time; LRU purge closes an idle one when it reconnects.
    config.httpd.max_open_sockets = 1;
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

int SyncServer::handle(httpd_req* request)
{
    SyncRequest sync;
    sync.method = methodName(request->method);
    sync.path = request->uri;
    const std::size_t query = sync.path.find('?');
    if (query != std::string::npos) {
        sync.path.resize(query);
    }

    char queryText[64];
    char value[16];
    if (httpd_req_get_url_query_str(request, queryText, sizeof(queryText)) ==
            ESP_OK &&
        httpd_query_key_value(queryText, "since", value, sizeof(value)) ==
            ESP_OK) {
        sync.hasSince = true;
        sync.since = value;
    }
    const std::size_t rangeLength =
        httpd_req_get_hdr_value_len(request, "Range");
    if (rangeLength > 0 && rangeLength < 64) {
        char header[64];
        httpd_req_get_hdr_value_str(request, "Range", header,
                                    sizeof(header));
        sync.hasRange = true;
        sync.range = header;
    }

    HttpReply reply(request);
    if (!readBody(request, sync.body)) {
        reply.start(400, "application/json", std::string());
        const std::string error =
            protocol::encodeError("bad_request", "bad request body");
        reply.write(reinterpret_cast<const std::uint8_t*>(error.data()),
                    error.size());
        reply.finish();
        return ESP_OK;
    }
    // ESP_FAIL closes the connection mid-body, so the desktop resumes.
    return handler_.handle(sync, reply) ? ESP_OK : ESP_FAIL;
}

}  // namespace cardputer_recorder
