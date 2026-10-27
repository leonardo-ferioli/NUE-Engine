#include "nue_legacy/win32/time.hpp"
#include <algorithm>

namespace nue::legacy::win32 {
namespace {
constexpr uint64_t kTicksPerSecond = 10000000ull, kTicksPerMs = 10000ull, kSecondsPerDay = 86400ull;
// 30827-12-31: the largest date SYSTEMTIME / FILETIME conversions accept (ticks that fit in 63 bits).
constexpr uint64_t kMaxFileTime = 0x7FFFFFFFFFFFFFFFull;

// Days since 1601-01-01 -> civil date (proleptic Gregorian), Howard Hinnant's algorithm shifted to the 1601 epoch.
void civil_from_days(int64_t days_since_1601, int64_t& y, uint32_t& m, uint32_t& d) {
    const int64_t shifted = days_since_1601 - 134774;   // 1601-01-01 is 134774 days before 1970-01-01
    const int64_t zz = shifted + 719468;
    const int64_t era = (zz >= 0 ? zz : zz - 146096) / 146097;
    const uint32_t doe = uint32_t(zz - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = int64_t(yoe) + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}
int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d) {   // days since 1601-01-01
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = uint32_t(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468 + 134774;
}
bool leap(uint32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
uint32_t month_days(uint32_t y, uint32_t m) { static const uint8_t t[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; return m == 2 && leap(y) ? 29 : t[m - 1]; }
}  // namespace

TimeApi::TimeApi(platform::Clock& clock) : clock_(clock), boot_offset_ms_(5 * 60 * 1000) {}

uint64_t TimeApi::query_performance_frequency() { return clock_.counter_frequency(); }
uint64_t TimeApi::query_performance_counter() { return clock_.counter(); }
uint32_t TimeApi::get_tick_count() { return uint32_t(uint64_t(clock_.time_ns() / 1000000 + boot_offset_ms_)); }
uint32_t TimeApi::time_get_time() { return get_tick_count(); }
uint64_t TimeApi::system_time_as_file_time() { return uint64_t(clock_.unix_ms()) * kTicksPerMs + kFileTimeEpochToUnixSeconds * kTicksPerSecond; }

void TimeApi::sleep(uint32_t ms) {
    if (ms == 0) { clock_.sleep_ns(0); return; }
    // Long sleeps are sliced so that a stopping host interrupts them (the bridge slices at 100 ms for the same reason).
    uint64_t remaining = ms == kInfinite ? ~0ull : ms;
    while (remaining) {
        if (stopping.load(std::memory_order_relaxed)) return;
        const uint64_t part = std::min<uint64_t>(remaining, 100);
        clock_.sleep_ns(int64_t(part) * 1000000);
        if (remaining != ~0ull) remaining -= part;
    }
}

bool TimeApi::file_time_to_system_time(uint64_t ft, SystemTime& out) {
    if (ft > kMaxFileTime) return false;
    const uint64_t seconds = ft / kTicksPerSecond;
    const uint32_t ms = uint32_t((ft % kTicksPerSecond) / kTicksPerMs);
    const int64_t days = int64_t(seconds / kSecondsPerDay);
    const uint32_t sod = uint32_t(seconds % kSecondsPerDay);
    int64_t y; uint32_t m, d;
    civil_from_days(days, y, m, d);
    if (y > 30827) return false;
    out.year = uint16_t(y); out.month = uint16_t(m); out.day = uint16_t(d);
    out.hour = uint16_t(sod / 3600); out.minute = uint16_t(sod % 3600 / 60); out.second = uint16_t(sod % 60); out.milliseconds = uint16_t(ms);
    out.day_of_week = uint16_t((days + 1) % 7);   // 1601-01-01 was a Monday; SYSTEMTIME counts Sunday as 0
    return true;
}

bool TimeApi::system_time_to_file_time(const SystemTime& s, uint64_t& out) {
    if (s.year < 1601 || s.year > 30827 || s.month < 1 || s.month > 12 || s.day < 1 || s.day > month_days(s.year, s.month) || s.hour > 23 || s.minute > 59 || s.second > 59 || s.milliseconds > 999) return false;
    const int64_t days = days_from_civil(s.year, s.month, s.day);
    const uint64_t seconds = uint64_t(days) * kSecondsPerDay + s.hour * 3600u + s.minute * 60u + s.second;
    out = seconds * kTicksPerSecond + uint64_t(s.milliseconds) * kTicksPerMs;
    return true;
}

uint64_t TimeApi::file_time_to_local_file_time(uint64_t ft) {
    const int64_t unix_ms = int64_t((ft - kFileTimeEpochToUnixSeconds * kTicksPerSecond) / kTicksPerMs);
    return ft + uint64_t(int64_t(clock_.local_utc_offset_minutes(unix_ms)) * 60 * int64_t(kTicksPerSecond));
}
uint64_t TimeApi::local_file_time_to_file_time(uint64_t local) {
    const int64_t unix_ms = int64_t((local - kFileTimeEpochToUnixSeconds * kTicksPerSecond) / kTicksPerMs);
    return local - uint64_t(int64_t(clock_.local_utc_offset_minutes(unix_ms)) * 60 * int64_t(kTicksPerSecond));
}

uint32_t TimeApi::get_time_zone_information(TimeZoneInfo& out) {
    out = TimeZoneInfo{};
    const int32_t offset = clock_.local_utc_offset_minutes(clock_.unix_ms());
    out.bias_minutes = -offset;
    out.standard_name = u"Local Standard Time";
    out.daylight_name = u"Local Daylight Time";
    // The platform reports the offset in effect now, not the transition rules: the zone is described without a daylight saving schedule
    // (documented deviation). The id says "no daylight saving information".
    return kTimeZoneIdUnknown;
}

uint32_t TimeApi::time_begin_period(uint32_t ms) {
    if (ms < 1 || ms > 1000000) return 97;
    periods_.fetch_add(1);
    return 0;
}
uint32_t TimeApi::time_end_period(uint32_t ms) {
    if (ms < 1 || ms > 1000000) return 97;
    if (periods_.load() <= 0) return 97;
    periods_.fetch_sub(1);
    return 0;
}

}  // namespace nue::legacy::win32
