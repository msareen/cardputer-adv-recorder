#include "recorder/sync/sync_index.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "recorder/sync/sha256.h"

namespace cardputer_recorder {
namespace {

constexpr const char* kHeaderTag = "CPREC-SYN 1";

std::vector<std::string> split(const std::string& line, char separator)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = line.find(separator, start);
        if (end == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
}

bool parseUnsigned(const std::string& text, std::uint64_t& value)
{
    if (text.empty() || text.size() > 20) {
        return false;
    }
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return true;
}

bool parseSigned(const std::string& text, std::int64_t& value)
{
    if (!text.empty() && text[0] == '-') {
        std::uint64_t magnitude = 0;
        if (!parseUnsigned(text.substr(1), magnitude)) {
            return false;
        }
        value = -static_cast<std::int64_t>(magnitude);
        return true;
    }
    std::uint64_t magnitude = 0;
    if (!parseUnsigned(text, magnitude)) {
        return false;
    }
    value = static_cast<std::int64_t>(magnitude);
    return true;
}

std::string upper(std::string text)
{
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

}  // namespace

bool namesEqualIgnoreCase(const std::string& left, const std::string& right)
{
    return left.size() == right.size() && upper(left) == upper(right);
}

std::uint32_t SyncIndex::bump()
{
    return ++cursor_;
}

SyncEntry* SyncIndex::findById(const std::string& id)
{
    for (SyncEntry& entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const SyncEntry* SyncIndex::findById(const std::string& id) const
{
    for (const SyncEntry& entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

SyncEntry* SyncIndex::findByName(const std::string& name)
{
    for (SyncEntry& entry : entries_) {
        if (namesEqualIgnoreCase(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

void SyncIndex::upsert(SyncEntry entry)
{
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const SyncEntry& existing) {
                           return existing.id == entry.id ||
                                  namesEqualIgnoreCase(existing.name,
                                                       entry.name);
                       }),
        entries_.end());
    entry.changed = bump();
    entries_.push_back(std::move(entry));
}

bool SyncIndex::rename(const std::string& from, const std::string& to)
{
    SyncEntry* entry = findByName(from);
    if (entry == nullptr) {
        return false;
    }
    entry->name = to;
    entry->changed = bump();
    return true;
}

bool SyncIndex::removeByName(const std::string& name)
{
    const std::size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const SyncEntry& entry) {
                                      return namesEqualIgnoreCase(entry.name,
                                                                  name);
                                  }),
                   entries_.end());
    if (entries_.size() == before) {
        return false;
    }
    bump();
    return true;
}

bool SyncIndex::removeById(const std::string& id)
{
    const std::size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const SyncEntry& entry) {
                                      return entry.id == id;
                                  }),
                   entries_.end());
    if (entries_.size() == before) {
        return false;
    }
    bump();
    return true;
}

bool SyncIndex::markAcked(const std::string& id, const std::string& host)
{
    SyncEntry* entry = findById(id);
    if (entry == nullptr) {
        return false;
    }
    entry->ackedBy = host;
    entry->changed = bump();
    return true;
}

bool SyncIndex::setSha256(const std::string& id, const std::string& sha256)
{
    SyncEntry* entry = findById(id);
    if (entry == nullptr) {
        return false;
    }
    entry->sha256 = sha256;
    return true;
}

void SyncIndex::clearAcks()
{
    bool changed = false;
    for (SyncEntry& entry : entries_) {
        if (!entry.ackedBy.empty()) {
            entry.ackedBy.clear();
            changed = true;
        }
    }
    if (changed) {
        const std::uint32_t value = bump();
        for (SyncEntry& entry : entries_) {
            entry.changed = value;
        }
    }
}

std::vector<std::string> SyncIndex::reconcile(
    const std::vector<CardFile>& files)
{
    bool removed = false;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        const auto match =
            std::find_if(files.begin(), files.end(), [&](const CardFile& f) {
                return namesEqualIgnoreCase(f.name, entry->name);
            });
        if (match == files.end() || match->size != entry->size) {
            entry = entries_.erase(entry);
            removed = true;
        } else {
            ++entry;
        }
    }
    if (removed) {
        bump();
    }

    std::vector<std::string> unknown;
    for (const CardFile& file : files) {
        if (findByName(file.name) == nullptr) {
            unknown.push_back(file.name);
        }
    }
    return unknown;
}

std::size_t SyncIndex::unsyncedCount(const std::string& host) const
{
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(),
                      [&](const SyncEntry& entry) {
                          return entry.ackedBy != host;
                      }));
}

std::string SyncIndex::serialize() const
{
    std::string out = kHeaderTag;
    out += '\t';
    out += std::to_string(cursor_);
    out += '\n';
    for (const SyncEntry& entry : entries_) {
        out += entry.id;
        out += '\t';
        out += entry.name;
        out += '\t';
        out += std::to_string(entry.size);
        out += '\t';
        out += std::to_string(entry.createdMs);
        out += '\t';
        out += std::to_string(entry.durationMs);
        out += '\t';
        out += std::to_string(entry.changed);
        out += '\t';
        out += entry.sha256;
        out += '\t';
        out += entry.ackedBy;
        out += '\n';
    }
    return out;
}

bool SyncIndex::parse(const std::string& text)
{
    cursor_ = 0;
    entries_.clear();
    std::vector<std::string> lines = split(text, '\n');
    if (lines.empty()) {
        return false;
    }
    for (std::string& line : lines) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }
    const std::vector<std::string> header = split(lines[0], '\t');
    std::uint64_t cursor = 0;
    if (header.size() != 2 || header[0] != kHeaderTag ||
        !parseUnsigned(header[1], cursor)) {
        return false;
    }
    cursor_ = static_cast<std::uint32_t>(cursor);

    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) {
            continue;
        }
        const std::vector<std::string> fields = split(lines[i], '\t');
        if (fields.size() != 8 || fields[0].size() != 32 ||
            fields[1].empty()) {
            continue;  // skip damaged lines, keep the rest
        }
        SyncEntry entry;
        std::uint64_t size = 0;
        std::uint64_t duration = 0;
        std::uint64_t changed = 0;
        if (!parseUnsigned(fields[2], size) ||
            !parseSigned(fields[3], entry.createdMs) ||
            !parseUnsigned(fields[4], duration) ||
            !parseUnsigned(fields[5], changed)) {
            continue;
        }
        entry.id = fields[0];
        entry.name = fields[1];
        entry.size = static_cast<std::uint32_t>(size);
        entry.durationMs = static_cast<std::uint32_t>(duration);
        entry.changed = static_cast<std::uint32_t>(changed);
        entry.sha256 = fields[6];
        entry.ackedBy = fields[7];
        if (entry.changed > cursor_) {
            cursor_ = entry.changed;
        }
        entries_.push_back(std::move(entry));
    }
    return true;
}

std::string legacyRecordingId(const std::string& name, std::uint32_t size,
                              std::int64_t sdTimestamp)
{
    const std::string key = upper(name) + "|" + std::to_string(size) + "|" +
                            std::to_string(sdTimestamp);
    return sha256Hex(key.data(), key.size()).substr(0, 32);
}

}  // namespace cardputer_recorder
