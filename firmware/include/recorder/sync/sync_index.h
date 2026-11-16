#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cardputer_recorder {

// One recording as the sync protocol sees it. Stored in /RECORDER.SYN.
struct SyncEntry {
    std::string id;  // 32 lowercase hex chars (128-bit recording id)
    std::string name;
    std::uint32_t size = 0;
    std::int64_t createdMs = 0;
    std::uint32_t durationMs = 0;
    // Cursor value at this entry's last visible change (add, rename, ack).
    std::uint32_t changed = 0;
    std::string sha256;   // cached file hash, empty until computed
    std::string ackedBy;  // host id that acked this file, empty if none
};

struct CardFile {
    std::string name;
    std::uint32_t size = 0;
};

// Pure in-memory sync index: no Arduino or SD access, so it is unit-tested
// on the host. The device code loads and saves it as text.
class SyncIndex {
public:
    std::uint32_t cursor() const { return cursor_; }
    const std::vector<SyncEntry>& entries() const { return entries_; }

    SyncEntry* findById(const std::string& id);
    SyncEntry* findByName(const std::string& name);
    const SyncEntry* findById(const std::string& id) const;

    // Adds a new entry, or replaces the one with the same name or id.
    void upsert(SyncEntry entry);
    bool rename(const std::string& from, const std::string& to);
    bool removeByName(const std::string& name);
    bool removeById(const std::string& id);
    bool markAcked(const std::string& id, const std::string& host);
    // Hash cache only: not a visible change, so the cursor is not bumped.
    bool setSha256(const std::string& id, const std::string& sha256);
    void clearAcks();

    // Drops entries whose file is gone or changed size, and returns the
    // names of card files the index does not know yet (in card order).
    std::vector<std::string> reconcile(const std::vector<CardFile>& files);

    std::size_t unsyncedCount(const std::string& host) const;

    std::string serialize() const;
    bool parse(const std::string& text);

private:
    std::uint32_t bump();

    std::uint32_t cursor_ = 0;
    std::vector<SyncEntry> entries_;
};

// Id for files recorded by firmware that did not write a cprc chunk:
// 32 hex chars of SHA-256(NAME|size|sdTimestamp).
std::string legacyRecordingId(const std::string& name, std::uint32_t size,
                              std::int64_t sdTimestamp);

bool namesEqualIgnoreCase(const std::string& left, const std::string& right);

}  // namespace cardputer_recorder
