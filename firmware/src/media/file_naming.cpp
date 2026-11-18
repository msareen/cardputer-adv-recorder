#include "recorder/media/file_naming.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace cardputer_recorder {

bool makeRecordingPath(std::uint32_t index, char* destination,
                       std::size_t capacity)
{
    if (destination == nullptr || capacity == 0 || index > 9999) {
        return false;
    }
    const int written = std::snprintf(
        destination, capacity, "/REC%04lu.WAV",
        static_cast<unsigned long>(index));
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

namespace {

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm), so the
// result does not depend on the C library's time zone handling.
void civilFromDays(std::int64_t days, int* year, int* month, int* day)
{
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    *year = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
    *month = static_cast<int>(m);
    *day = static_cast<int>(d);
}

bool readDigits(const char* text, int count, int* value)
{
    int result = 0;
    for (int i = 0; i < count; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
        result = result * 10 + (text[i] - '0');
    }
    *value = result;
    return true;
}

}  // namespace

LocalDateTime localDateTimeFrom(std::int64_t unixSeconds,
                                std::int32_t tzOffsetMinutes)
{
    const std::int64_t local =
        unixSeconds + static_cast<std::int64_t>(tzOffsetMinutes) * 60;
    std::int64_t days = local / 86400;
    std::int64_t secondsOfDay = local % 86400;
    if (secondsOfDay < 0) {
        secondsOfDay += 86400;
        --days;
    }
    LocalDateTime time;
    civilFromDays(days, &time.year, &time.month, &time.day);
    time.hour = static_cast<int>(secondsOfDay / 3600);
    time.minute = static_cast<int>((secondsOfDay % 3600) / 60);
    return time;
}

bool makeTimestampRecordingPath(const LocalDateTime& time,
                                std::uint32_t suffix, char* destination,
                                std::size_t capacity)
{
    if (destination == nullptr || capacity == 0 || suffix > 99 ||
        time.year < 2000 || time.year > 9999 || time.month < 1 ||
        time.month > 12 || time.day < 1 || time.day > 31 || time.hour < 0 ||
        time.hour > 23 || time.minute < 0 || time.minute > 59) {
        return false;
    }
    int written;
    if (suffix >= 2) {
        written = std::snprintf(destination, capacity,
                                "/REC%02d%02d%04d_%02d%02d_%lu.WAV", time.day,
                                time.month, time.year, time.hour, time.minute,
                                static_cast<unsigned long>(suffix));
    } else {
        written = std::snprintf(destination, capacity,
                                "/REC%02d%02d%04d_%02d%02d.WAV", time.day,
                                time.month, time.year, time.hour,
                                time.minute);
    }
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

bool timestampNameKey(const char* name, std::uint64_t* key)
{
    if (name == nullptr) {
        return false;
    }
    if (*name == '/') {
        ++name;
    }
    // REC, DDMMYYYY_HHMM, then optional _N or _NN, then .WAV
    for (const char* prefix = "REC"; *prefix != '\0'; ++prefix, ++name) {
        if (std::toupper(static_cast<unsigned char>(*name)) != *prefix) {
            return false;
        }
    }
    int day, month, year, hour, minute;
    if (std::strlen(name) < 17 || !readDigits(name, 2, &day) ||
        !readDigits(name + 2, 2, &month) || !readDigits(name + 4, 4, &year) ||
        name[8] != '_' || !readDigits(name + 9, 2, &hour) ||
        !readDigits(name + 11, 2, &minute)) {
        return false;
    }
    if (day < 1 || day > 31 || month < 1 || month > 12 || hour > 23 ||
        minute > 59) {
        return false;
    }
    const char* rest = name + 13;
    int suffix = 0;
    if (*rest == '_') {
        ++rest;
        int digits = 0;
        while (std::isdigit(static_cast<unsigned char>(rest[digits])) &&
               digits < 2) {
            suffix = suffix * 10 + (rest[digits] - '0');
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
        rest += digits;
    }
    if (std::strlen(rest) != 4 || rest[0] != '.' ||
        std::toupper(static_cast<unsigned char>(rest[1])) != 'W' ||
        std::toupper(static_cast<unsigned char>(rest[2])) != 'A' ||
        std::toupper(static_cast<unsigned char>(rest[3])) != 'V') {
        return false;
    }
    if (key != nullptr) {
        const std::uint64_t stamp =
            ((((static_cast<std::uint64_t>(year) * 100 + month) * 100 + day) *
                  100 +
              hour) *
                 100 +
             minute);
        *key = stamp * 100 + static_cast<std::uint64_t>(suffix);
    }
    return true;
}

bool recordingListsBefore(const char* left, const char* right)
{
    std::uint64_t leftKey = 0;
    std::uint64_t rightKey = 0;
    const bool leftStamped = timestampNameKey(left, &leftKey);
    const bool rightStamped = timestampNameKey(right, &rightKey);
    if (leftStamped != rightStamped) {
        return leftStamped;
    }
    if (leftStamped) {
        return leftKey > rightKey;
    }
    return std::strcmp(left, right) > 0;
}

}  // namespace cardputer_recorder
