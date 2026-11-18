#include "recorder/recorder_app.h"

#include <algorithm>
#include <ctime>

#include "recorder/app/app_shared.h"
#include "recorder/media/file_naming.h"

namespace cardputer_recorder {
namespace {

constexpr const char* kLockIndexPath = "/RECORDER.LCK";
constexpr std::size_t kMaxRenameBaseLength = 32;

bool isSafeRenameCharacter(char character)
{
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') ||
           character == '-' || character == '_' || character == ' ';
}

String basenameWithoutWav(const String& filename)
{
    String base = filename;
    if (base.endsWith(".WAV") || base.endsWith(".wav")) {
        base.remove(base.length() - 4);
    }
    return base;
}

}  // namespace

void RecorderApp::deleteSelected()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return;
    }
    const String filename = files_[selected_];
    if (isLocked(filename)) {
        message_ = "Locked";
        deleteConfirm_ = false;
        forceRedraw_ = true;
        return;
    }
    if (!deleteConfirm_ || deleteConfirmName_ != filename) {
        deleteConfirm_ = true;
        deleteConfirmName_ = filename;
        message_ = "ENTER confirms delete.";
        forceRedraw_ = true;
        return;
    }

    const String path = "/" + filename;
    if (!storage_.remove(path.c_str())) {
        setError("Could not delete recording.");
        return;
    }
    syncNoteDeleted(filename);
    message_ = "Deleted " + filename;
    deleteConfirm_ = false;
    deleteConfirmName_ = "";
    scanFiles();
    forceRedraw_ = true;
}
void RecorderApp::clearAllRecordings()
{
    if (!storage_.isMounted()) {
        setError("E1: microSD mount failed.");
        return;
    }
    scanFiles();
    Serial.printf("[CLEAR] clearing %u files, %u locked\n",
                  static_cast<unsigned>(files_.size()), static_cast<unsigned>(lockedFiles_.size()));
    // Locked recordings stay, as with single delete. The sync index is
    // updated once at the end instead of once per file.
    const bool indexReady = ensureSyncIndex();
    bool indexChanged = false;
    unsigned deleted = 0;
    unsigned failed = 0;
    for (const String& filename : files_) {
        if (isLocked(filename)) {
            continue;
        }
        const String path = "/" + filename;
        if (!storage_.remove(path.c_str())) {
            Serial.printf("[CLEAR] remove FAILED %s\n", path.c_str());
            ++failed;
            continue;
        }
        Serial.printf("[CLEAR] removed %s\n", path.c_str());
        ++deleted;
        if (indexReady && syncIndex_.removeByName(std::string(filename.c_str()))) {
            indexChanged = true;
        }
    }
    if (indexChanged) {
        saveSyncIndex();
    }
    refreshUnsyncedCount();
    deleteConfirm_ = false;
    deleteConfirmName_ = "";
    scanFiles();
    selected_ = 0;
    message_ = "Cleared " + String(deleted) + " recording" +
               (deleted == 1 ? "" : "s");
    if (failed > 0) {
        message_ += ", " + String(failed) + " failed";
    }
    if (!lockedFiles_.empty()) {
        message_ += "; locked kept";
    }
    message_ += ".";
    forceRedraw_ = true;
}
void RecorderApp::toggleLockSelected()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return;
    }

    const String filename = files_[selected_];
    auto found = std::find(lockedFiles_.begin(), lockedFiles_.end(),
                           filename);
    if (found == lockedFiles_.end()) {
        lockedFiles_.push_back(filename);
        message_ = "Locked " + filename;
    } else {
        lockedFiles_.erase(found);
        message_ = "Unlocked " + filename;
    }
    deleteConfirm_ = false;
    if (!saveLocks()) {
        setError("Could not save locks.");
        return;
    }
    forceRedraw_ = true;
}
bool RecorderApp::isLocked(const String& filename) const
{
    return std::find(lockedFiles_.begin(), lockedFiles_.end(), filename) !=
           lockedFiles_.end();
}
void RecorderApp::loadLocks()
{
    lockedFiles_.clear();
    File file = storage_.open(kLockIndexPath, FILE_READ);
    if (!file) {
        return;
    }
    while (file.available()) {
        String name = file.readStringUntil('\n');
        name.trim();
        name.toUpperCase();
        if (name.endsWith(".WAV") &&
            std::find(lockedFiles_.begin(), lockedFiles_.end(), name) ==
                lockedFiles_.end()) {
            lockedFiles_.push_back(name);
        }
    }
    file.close();
}
bool RecorderApp::saveLocks()
{
    if (storage_.exists(kLockIndexPath)) {
        storage_.remove(kLockIndexPath);
    }
    if (lockedFiles_.empty()) {
        return true;
    }

    File file = storage_.open(kLockIndexPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    for (const String& name : lockedFiles_) {
        file.println(name);
    }
    file.close();
    return true;
}
void RecorderApp::scanFiles()
{
    files_.clear();
    File directory = storage_.open("/", FILE_READ);
    if (!directory || !directory.isDirectory()) {
        setError("Could not read SD directory.");
        return;
    }
    File entry = directory.openNextFile();
    while (entry) {
        String name = entry.name();
        String normalizedName = name;
        normalizedName.toLowerCase();
        if (!entry.isDirectory() &&
            normalizedName.endsWith(".wav") &&
            entry.size() >= kPcmWavHeaderSize) {
            if (name.startsWith("/")) {
                name.remove(0, 1);
            }
            files_.push_back(name);
        }
        entry.close();
        entry = directory.openNextFile();
    }
    directory.close();
    // Newest first: timestamped names by date, then RECnnnn by number.
    std::sort(files_.begin(), files_.end(),
              [](const String& left, const String& right) {
                  return recordingListsBefore(left.c_str(), right.c_str());
              });
    loadLocks();
    bool prunedLocks = false;
    for (auto item = lockedFiles_.begin(); item != lockedFiles_.end();) {
        if (std::find(files_.begin(), files_.end(), *item) ==
            files_.end()) {
            item = lockedFiles_.erase(item);
            prunedLocks = true;
        } else {
            ++item;
        }
    }
    if (prunedLocks) {
        saveLocks();
    }
    if (files_.empty()) {
        selected_ = 0;
    } else if (selected_ >= static_cast<int>(files_.size())) {
        selected_ = files_.size() - 1;
    }
    deleteConfirm_ = false;
    deleteConfirmName_ = "";
}
bool RecorderApp::chooseRecordingPath(char* path, std::size_t capacity)
{
    // RECDDMMYYYY_HHMM.WAV in local time once a sync has set the clock (the
    // ADV has no RTC, so it is lost at power-off); RECnnnn.WAV before that.
    Serial.printf("[NAME] clockSet=%d now=%ld tz=%d\n", clockIsSet(),
                  static_cast<long>(std::time(nullptr)), static_cast<int>(tzOffsetMin_.load()));
    if (clockIsSet()) {
        const LocalDateTime local =
            localDateTimeFrom(static_cast<std::int64_t>(std::time(nullptr)),
                              tzOffsetMin_.load());
        for (std::uint32_t suffix = 1; suffix <= 99; ++suffix) {
            if (!makeTimestampRecordingPath(local, suffix, path, capacity)) {
                break;
            }
            if (!storage_.exists(path)) {
                return true;
            }
        }
    }
    for (std::uint32_t index = 1; index <= 9999; ++index) {
        if (!makeRecordingPath(index, path, capacity)) {
            return false;
        }
        if (!storage_.exists(path)) {
            return true;
        }
    }
    return false;
}
String RecorderApp::storageUsageText() const
{
    if (!storage_.isMounted()) {
        return "SD unavailable";
    }
    const std::uint64_t capacity = storage_.capacityBytes();
    const std::uint64_t used = storage_.usedBytes();
    if (capacity == 0 || used > capacity) {
        return "SD size unknown";
    }
    return "FREE " + formatByteCount(capacity - used) + " / " +
           formatByteCount(capacity);
}
String RecorderApp::selectedRecordingDetail()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return storageUsageText();
    }

    const String path = "/" + files_[selected_];
    const std::uint32_t size = storage_.fileSize(path.c_str());
    String detail = formatByteCount(size);

    File file = storage_.open(path.c_str(), FILE_READ);
    WavReader detailReader;
    if (detailReader.begin(file)) {
        const WavInfo& wav = detailReader.info();
        const std::uint32_t bytesPerSecond =
            wav.spec.sampleRate * wav.spec.channels *
            (wav.spec.bitsPerSample / 8);
        if (bytesPerSecond > 0) {
            const unsigned long durationMs =
                static_cast<unsigned long>(
                    static_cast<std::uint64_t>(wav.dataSize) * 1000ULL /
                    bytesPerSecond);
            detail += "  " + formatTime(durationMs);
        }
        detailReader.end();
    } else if (file) {
        file.close();
    }
    return detail + "  " + storageUsageText();
}
void RecorderApp::beginRenameSelected()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return;
    }
    deleteConfirm_ = false;
    renameOriginalName_ = files_[selected_];
    renameText_ = basenameWithoutWav(renameOriginalName_);
    state_ = State::kRename;
    message_ = "Rename";
    forceRedraw_ = true;
}
void RecorderApp::handleRenameInput(const InputEvent& event)
{
    if (event.back) {
        state_ = State::kBrowsing;
        renameOriginalName_ = "";
        renameText_ = "";
        message_ = "Rename canceled.";
        forceRedraw_ = true;
        return;
    }
    if (event.deletePressed && renameText_.length() > 0) {
        renameText_.remove(renameText_.length() - 1);
        forceRedraw_ = true;
    }
    for (std::size_t index = 0; index < event.text.length(); ++index) {
        char character = event.text[index];
        if (!isSafeRenameCharacter(character) ||
            renameText_.length() >= kMaxRenameBaseLength) {
            continue;
        }
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
        renameText_ += character;
        forceRedraw_ = true;
    }
    if (event.confirm) {
        commitRename();
    }
}
void RecorderApp::commitRename()
{
    renameText_.trim();
    if (renameText_.length() == 0) {
        message_ = "Name required.";
        forceRedraw_ = true;
        return;
    }

    String targetName = renameText_;
    targetName.toUpperCase();
    targetName += ".WAV";
    if (targetName == renameOriginalName_) {
        state_ = State::kBrowsing;
        renameOriginalName_ = "";
        renameText_ = "";
        message_ = "Rename unchanged.";
        forceRedraw_ = true;
        return;
    }

    const String fromPath = "/" + renameOriginalName_;
    const String toPath = "/" + targetName;
    if (storage_.exists(toPath.c_str())) {
        message_ = "Name exists.";
        forceRedraw_ = true;
        return;
    }
    const bool wasLocked = isLocked(renameOriginalName_);
    if (!storage_.rename(fromPath.c_str(), toPath.c_str())) {
        setError("Could not rename recording.");
        return;
    }
    syncNoteRenamed(renameOriginalName_, targetName);
    if (wasLocked) {
        auto found = std::find(lockedFiles_.begin(), lockedFiles_.end(),
                               renameOriginalName_);
        if (found != lockedFiles_.end()) {
            *found = targetName;
            if (!saveLocks()) {
                setError("Could not save locks.");
                return;
            }
        }
    }
    state_ = State::kBrowsing;
    message_ = "Renamed " + targetName;
    renameOriginalName_ = "";
    renameText_ = "";
    scanFiles();
    for (std::size_t index = 0; index < files_.size(); ++index) {
        if (files_[index] == targetName) {
            selected_ = static_cast<int>(index);
            break;
        }
    }
    forceRedraw_ = true;
}

}  // namespace cardputer_recorder
