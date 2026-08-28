#pragma once

#include <cstdint>

namespace sol::platform {

// Monotonic, high-resolution seconds since an arbitrary fixed epoch.
[[nodiscard]] double timeSeconds();

void sleepMilliseconds(std::uint32_t milliseconds);

// A wall-clock instant, broken down into the fields a date is written from.
// `year` is the full year and `month`/`day` are 1-based, so the struct reads
// the way a date does rather than the way `struct tm` stores one.
struct CalendarTime
{
    std::int32_t year = 1970;
    std::int32_t month = 1;  // 1-12
    std::int32_t day = 1;    // 1-31
    std::int32_t hour = 0;   // 0-23
    std::int32_t minute = 0; // 0-59
    std::int32_t second = 0; // 0-60, because leap seconds exist
};

// Seconds since 1970-01-01T00:00:00Z, or 0 if the OS has no usable clock.
//
// ⚑⚑ THE ONLY WALL CLOCK IN THIS PROJECT, AND IT IS SEPARATE FROM
// `timeSeconds()` ON PURPOSE. `timeSeconds` is monotonic and the frame loop
// subtracts two of them; this one steps backwards over an NTP correction and
// must never be used for a delta. AGENTS.md 4 forbids it in sim code outright.
// It exists so a save can record WHEN it was written - a fact about the
// player's day, which no monotonic clock can express.
//
// ⚑ Not `fileModificationTime`: that value is opaque, platform-specific and
// only comparable against another of its own kind. This one has a defined
// meaning, which is what makes it printable.
[[nodiscard]] std::uint64_t wallClockSeconds();

// Breaks a `wallClockSeconds` value down in the machine's LOCAL time zone.
// False if the value cannot be represented, leaving `out` untouched.
//
// ⚑ Local rather than UTC because the only caller shows the result to the
// person sitting at the machine, and "yesterday evening" is what they need to
// recognise their own save. The stored value stays UTC, so a save keeps its
// meaning when the machine's zone changes.
[[nodiscard]] bool localCalendarTime(std::uint64_t unixSeconds, CalendarTime& out);

} // namespace sol::platform
