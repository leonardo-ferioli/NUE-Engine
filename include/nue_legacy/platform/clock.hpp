// Platform API: clock and sleeping. Shaped like NUE's nue_time_* / nue_sleep_* (include/nue/core.h): a monotonic nanosecond clock, a raw
// high-resolution counter with its frequency, the wall clock and a sleep. The Win32 time layer (GetTickCount, QueryPerformanceCounter,
// GetSystemTimeAsFileTime, Sleep...) is written against this interface only, so it runs on any host and, with the null clock, deterministically.
#pragma once
#include "nue_legacy/platform/types.hpp"

namespace nue::legacy::platform {

class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t time_ns() = 0;                      // monotonic, starts near 0 at process start (nue_time_ns)
    virtual uint64_t counter() = 0;                     // raw high-resolution counter (nue_time_counter)
    virtual uint64_t counter_frequency() = 0;           // ticks per second (nue_time_counter_frequency)
    virtual int64_t unix_ms() = 0;                      // wall clock, ms since 1970-01-01 UTC (nue_time_unix_ms)
    virtual int32_t local_utc_offset_minutes(int64_t unix_ms) = 0;   // minutes east of UTC at that instant, daylight saving included
    virtual void sleep_ns(int64_t ns) = 0;              // returns after at least `ns` (nue_sleep_ns); <= 0 only yields
};

}  // namespace nue::legacy::platform
