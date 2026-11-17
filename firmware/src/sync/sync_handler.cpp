#include "recorder/sync/sync_handler.h"

#include <Arduino.h>

#include <cstring>

#include "recorder/sync/sha256.h"

namespace cardputer_recorder {
namespace {

// Streams from SD in reads of the scratch buffer's minimum size.
constexpr std::size_t kReadChunkBytes = SyncHandler::kMinScratchBytes;

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

bool writeText(SyncReply& reply, const std::string& text)
{
    return reply.write(reinterpret_cast<const std::uint8_t*>(text.data()),
                       text.size());
}

}  // namespace

bool SyncHandler::begin(StorageService& storage, SyncIndex& index,
                        std::mutex& indexMutex, const std::string& hostId,
                        std::uint8_t* scratch, std::size_t scratchSize,
                        SyncCallbacks callbacks)
{
    if (scratch == nullptr || scratchSize < kMinScratchBytes) {
        return false;
    }
    storage_ = &storage;
    index_ = &index;
    indexMutex_ = &indexMutex;
    hostId_ = hostId;
    scratch_ = scratch;
    scratchSize_ = scratchSize;
    callbacks_ = std::move(callbacks);
    return true;
}

bool SyncHandler::sendJson(SyncReply& reply, int status,
                           const std::string& body)
{
    return reply.start(status, "application/json", std::string()) &&
           writeText(reply, body) && reply.finish();
}

bool SyncHandler::sendError(SyncReply& reply, int status, const char* code,
                            const std::string& message)
{
    return sendJson(reply, status, protocol::encodeError(code, message));
}

bool SyncHandler::handle(const SyncRequest& request, SyncReply& reply)
{
    if (callbacks_.request) {
        callbacks_.request();
    }
    const protocol::RouteMatch match =
        protocol::matchRoute(request.method, request.path);

    switch (match.route) {
        case protocol::Route::kInfo:
            return handleInfo(reply);
        case protocol::Route::kTime:
            return handleTime(request, reply);
        case protocol::Route::kManifest:
            return handleManifest(request, reply);
        case protocol::Route::kDone: {
            const bool ok = reply.start(204, "application/json",
                                        std::string()) &&
                            reply.finish();
            if (callbacks_.done) {
                callbacks_.done();
            }
            return ok;
        }
        case protocol::Route::kDownload:
            return handleDownload(request, match.id, reply);
        case protocol::Route::kSha256:
            return handleSha256(match.id, reply);
        case protocol::Route::kAck:
            return handleAck(request, match.id, reply);
        case protocol::Route::kDelete:
            return handleDelete(match.id, reply);
        case protocol::Route::kMethodNotAllowed:
            return sendError(reply, 405, "bad_request", "method not allowed");
        default:
            return sendError(reply, 404, "not_found", "no such endpoint");
    }
}

bool SyncHandler::handleInfo(SyncReply& reply)
{
    protocol::DeviceInfo info;
    if (callbacks_.info) {
        info = callbacks_.info();
    }
    return sendJson(reply, 200, protocol::encodeInfo(info));
}

bool SyncHandler::handleTime(const SyncRequest& request, SyncReply& reply)
{
    std::int64_t unixMs = 0;
    if (!protocol::extractJsonInt64(request.body, "unix_ms", unixMs) ||
        unixMs <= 0) {
        return sendError(reply, 400, "bad_request", "unix_ms required");
    }
    std::int64_t offset = 0;
    protocol::extractJsonInt64(request.body, "tz_offset_min", offset);
    if (callbacks_.setClock) {
        callbacks_.setClock(unixMs, static_cast<std::int32_t>(offset));
    }
    return sendJson(reply, 200, "{}");
}

bool SyncHandler::handleManifest(const SyncRequest& request,
                                 SyncReply& reply)
{
    std::uint32_t since = 0;
    if (request.hasSince && !protocol::parseUint32(request.since, since)) {
        return sendError(reply, 400, "bad_request", "since must be a number");
    }

    // Stream entry by entry (a full manifest could be tens of KB). The
    // index lock is held for the whole listing so it is consistent.
    if (!reply.start(200, "application/json", std::string())) {
        return false;
    }
    std::lock_guard<std::mutex> lock(*indexMutex_);
    if (!writeText(reply, protocol::encodeManifestHead(index_->cursor()))) {
        return false;
    }
    bool first = true;
    for (const SyncEntry& entry : index_->entries()) {
        if (request.hasSince && entry.changed <= since) {
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
        if (!writeText(reply, item)) {
            return false;
        }
    }
    return writeText(reply, protocol::manifestTail()) && reply.finish();
}

bool SyncHandler::handleDownload(const SyncRequest& request,
                                 const std::string& id, SyncReply& reply)
{
    const EntryCopy entry = copyEntry(*index_, *indexMutex_, id);
    if (!entry.found) {
        return sendError(reply, 404, "not_found", "no such recording");
    }
    const std::string path = "/" + entry.name;
    File file = storage_->open(path.c_str(), FILE_READ);
    if (!file) {
        return sendError(reply, 500, "sd_error", "could not open recording");
    }
    const std::uint32_t size = static_cast<std::uint32_t>(file.size());
    if (size == 0) {
        file.close();
        return sendError(reply, 500, "sd_error", "empty recording");
    }

    protocol::ByteRange range;
    range.start = 0;
    range.end = size - 1;
    if (request.hasRange &&
        !protocol::parseRange(request.range, size, range)) {
        file.close();
        return sendError(reply, 416, "bad_range", "bad Range");
    }
    if (range.start > 0 && !file.seek(range.start)) {
        file.close();
        return sendError(reply, 500, "sd_error", "seek failed");
    }

    const std::uint32_t length = range.end - range.start + 1;
    const bool wholeFile = range.start == 0 && range.end == size - 1;
    std::string contentRange;
    if (request.hasRange) {
        contentRange = "bytes " + std::to_string(range.start) + "-" +
                       std::to_string(range.end) + "/" +
                       std::to_string(size);
    }
    if (!reply.start(request.hasRange ? 206 : 200, "audio/wav",
                     contentRange)) {
        file.close();
        return false;
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
        if (!reply.write(scratch_, got)) {
            ok = false;  // desktop went away; it resumes with Range
            break;
        }
        sent += got;
        if (callbacks_.progress) {
            callbacks_.progress(entry.name, sent, length);
        }
    }
    file.close();

    // SD-to-transport throughput and heap headroom.
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
        return false;
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
    return reply.finish();
}

bool SyncHandler::sha256For(const std::string& id, std::string& sha256)
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

bool SyncHandler::handleSha256(const std::string& id, SyncReply& reply)
{
    if (!copyEntry(*index_, *indexMutex_, id).found) {
        return sendError(reply, 404, "not_found", "no such recording");
    }
    std::string sha256;
    if (!sha256For(id, sha256)) {
        return sendError(reply, 500, "sd_error", "could not read recording");
    }
    return sendJson(reply, 200, protocol::encodeSha256(sha256));
}

bool SyncHandler::handleAck(const SyncRequest& request,
                            const std::string& id, SyncReply& reply)
{
    std::string claimed;
    if (!protocol::extractJsonString(request.body, "sha256", claimed)) {
        return sendError(reply, 400, "bad_request", "sha256 required");
    }
    const EntryCopy entry = copyEntry(*index_, *indexMutex_, id);
    if (!entry.found) {
        return sendError(reply, 404, "not_found", "no such recording");
    }
    std::string actual;
    if (!sha256For(id, actual)) {
        return sendError(reply, 500, "sd_error", "could not read recording");
    }
    if (claimed != actual) {
        return sendError(reply, 409, "hash_mismatch", "ack hash differs");
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
        return sendError(reply, 500, "sd_error", "could not save sync index");
    }
    return sendJson(reply, 200, protocol::encodeAckResult(true, deleted));
}

bool SyncHandler::handleDelete(const std::string& id, SyncReply& reply)
{
    const EntryCopy entry = copyEntry(*index_, *indexMutex_, id);
    if (!entry.found) {
        return sendError(reply, 404, "not_found", "no such recording");
    }
    if (callbacks_.isLocked && callbacks_.isLocked(entry.name)) {
        return sendError(reply, 409, "locked", "recording is locked");
    }
    const std::string path = "/" + entry.name;
    if (!storage_->remove(path.c_str())) {
        return sendError(reply, 500, "sd_error", "delete failed");
    }
    {
        std::lock_guard<std::mutex> lock(*indexMutex_);
        index_->removeById(id);
        if (callbacks_.saveIndex) {
            callbacks_.saveIndex();
        }
    }
    return sendJson(reply, 200, protocol::encodeDeleteResult(true));
}

}  // namespace cardputer_recorder
