// Time: GetTickCount, timeGetTime, QueryPerformanceCounter/Frequency, GetSystemTimeAsFileTime, FILETIME arithmetic, time zone, timeBeginPeriod.
// Written against platform::Clock. FILETIME is a count of 100 ns ticks since 1601-01-01 UTC.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/platform/clock.hpp"
#include <atomic>

namespace nue::legacy::win32 {

struct SystemTime {                       // SYSTEMTIME
    uint16_t year = 1601, month = 1, day_of_week = 1, day = 1, hour = 0, minute = 0, second = 0, milliseconds = 0;
    friend bool operator==(const SystemTime&, const SystemTime&) = default;
};

struct TimeZoneInfo {                     // TIME_ZONE_INFORMATION, names as UTF-16 code units
    int32_t bias_minutes = 0;             // UTC = local + bias
    std::u16string standard_name, daylight_name;
    SystemTime standard_date, daylight_date;
    int32_t standard_bias = 0, daylight_bias = -60;
};
constexpr uint32_t kTimeZoneIdUnknown = 0, kTimeZoneIdStandard = 1, kTimeZoneIdDaylight = 2;

struct TimeDevCaps { uint32_t period_min = 1, period_max = 1000000; };   // TIMECAPS

constexpr uint64_t kFileTimeEpochToUnixSeconds = 11644473600ull;

class TimeApi {
public:
    explicit TimeApi(platform::Clock& clock);

    uint64_t query_performance_frequency();
    uint64_t query_performance_counter();
    uint32_t get_tick_count();            // milliseconds since "boot" (wraps at 2^32 like the original after 49.7 days)
    uint32_t time_get_time();             // same clock, same wrap
    uint64_t system_time_as_file_time();  // 100 ns ticks since 1601
    void sleep(uint32_t milliseconds);    // 0 yields; kInfinite sleeps until the context stops

    // FILETIME <-> SYSTEMTIME. false (ERROR_INVALID_PARAMETER in Win32) for a time outside 1601..30827.
    static bool file_time_to_system_time(uint64_t file_time, SystemTime& out);
    static bool system_time_to_file_time(const SystemTime& in, uint64_t& out);
    // Shifts a UTC FILETIME to local time using the zone in effect at that instant.
    uint64_t file_time_to_local_file_time(uint64_t file_time);
    uint64_t local_file_time_to_file_time(uint64_t local_file_time);
    uint32_t get_time_zone_information(TimeZoneInfo& out);   // returns TIME_ZONE_ID_*

    // timeBeginPeriod / timeEndPeriod: a request for finer timer resolution. Counted so that the pairs balance; the value is advisory here.
    uint32_t time_begin_period(uint32_t ms);   // 0 = TIMERR_NOERROR, 97 = TIMERR_NOCANDO
    uint32_t time_end_period(uint32_t ms);
    TimeDevCaps time_get_dev_caps() const { return {}; }

    // Set by the host when the guest must stop: sleeps and waits come back at once.
    std::atomic<bool> stopping{false};

private:
    platform::Clock& clock_;
    int64_t boot_offset_ms_;              // makes GetTickCount start at a plausible uptime instead of 0
    std::atomic<int32_t> periods_{0};
};

}  // namespace nue::legacy::win32
