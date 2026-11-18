#pragma once

#include <cstddef>
#include <cstdint>

namespace cardputer_recorder {

// "/REC0042.WAV": used while the clock is not set (the ADV has no RTC; the
// desktop sets the time on every sync).
bool makeRecordingPath(std::uint32_t index, char* destination,
                       std::size_t capacity);

// Local date and time of a recording's start.
struct LocalDateTime {
    int year = 0;    // e.g. 2026
    int month = 0;   // 1-12
    int day = 0;     // 1-31
    int hour = 0;    // 0-23
    int minute = 0;  // 0-59
};

// Local time from a Unix time (seconds) and the desktop's offset
// (local minus UTC, minutes).
LocalDateTime localDateTimeFrom(std::int64_t unixSeconds,
                                std::int32_t tzOffsetMinutes);

// "/RECDDMMYYYY_HHMM.WAV", or "/RECDDMMYYYY_HHMM_2.WAV" ... for `suffix` >= 2
// when that minute is taken. Suffix 0 or 1 means none; at most 99.
bool makeTimestampRecordingPath(const LocalDateTime& time,
                                std::uint32_t suffix, char* destination,
                                std::size_t capacity);

// For names of the form RECDDMMYYYY_HHMM[_N].WAV (any case, with or without a
// leading '/'): a key that sorts chronologically (YYYYMMDDHHMM * 100 + N).
bool timestampNameKey(const char* name, std::uint64_t* key);

// Library order: timestamped recordings newest first, then the others
// (RECnnnn, renamed files) by name, highest first.
bool recordingListsBefore(const char* left, const char* right);

}  // namespace cardputer_recorder
