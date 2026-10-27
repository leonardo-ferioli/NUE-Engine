// Time: FILETIME/SYSTEMTIME arithmetic (against the real Windows functions on Windows), the tick counters on a virtual clock, sleeping.
#include "nue_legacy/win32/handles.hpp"
#include "nue_legacy/win32/time.hpp"
#include "nue_legacy/platform/null_services.hpp"
#include "test_support.hpp"
#include <random>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace nue::legacy::win32;
namespace null = nue::legacy::platform::null;

static void test_handles() {
    HandleTable table;
    struct Thing : Object { Thing() : Object(ObjectKind::other) {} bool closed = false; void on_last_handle_closed() override { closed = true; } };
    auto a = std::make_shared<Thing>(), b = std::make_shared<Thing>();
    const Handle ha = table.add(a), hb = table.add(b);
    CHECK(ha != 0 && hb != 0 && ha != hb);
    CHECK((ha & 3) == 0 && ha < 0x40000);
    CHECK(table.get(ha) == a);
    CHECK(table.get(0) == nullptr);
    CHECK(table.get(kInvalidHandle) == nullptr);
    CHECK(table.get(ha + 1) == nullptr);
    const Handle dup = table.duplicate(ha);
    CHECK(dup != 0 && dup != ha && table.get(dup) == a);
    CHECK(table.close(ha));
    CHECK(!a->closed);                    // the duplicate keeps it open
    CHECK(table.get(ha) == nullptr);      // the closed value is dead
    CHECK(!table.close(ha));              // a second close fails
    CHECK(table.close(dup));
    CHECK(a->closed);
    // A new object may reuse the slot but never the handle value.
    auto c = std::make_shared<Thing>();
    const Handle hc = table.add(c);
    CHECK(hc != ha && table.get(ha) == nullptr);
    CHECK_EQ(table.live(), 2u);
    // Generations cycle through 1..7: the same slot reused 7 times comes back to the value it had.
    Handle last = 0;
    for (int i = 0; i < 20; ++i) { auto t = std::make_shared<Thing>(); Handle h = table.add(t); CHECK(h != last); CHECK(table.close(h)); last = h; }
}
REGISTER_TEST("handles", test_handles);

static void test_time() {
    null::NullClock clock;
    TimeApi time(clock);
    CHECK_EQ(time.query_performance_frequency(), 10000000ull);
    const uint64_t c0 = time.query_performance_counter();
    clock.advance_ns(1500000000);
    CHECK_EQ(time.query_performance_counter() - c0, 15000000ull);
    const uint32_t t0 = time.get_tick_count();
    clock.advance_ns(2500000000ll);
    CHECK_EQ(time.get_tick_count() - t0, 2500u);
    CHECK_EQ(time.time_get_time(), time.get_tick_count());
    // Sleep advances the virtual clock by the requested time without waiting.
    const uint32_t t1 = time.get_tick_count();
    time.sleep(1234);
    CHECK_EQ(time.get_tick_count() - t1, 1234u);
    CHECK_EQ(clock.total_slept_ns(), 1234000000ll);
    time.stopping = true;
    time.sleep(kInfinite);                // returns at once when the host is stopping
    time.stopping = false;
    // FILETIME of the clock's start (2000-01-01 00:00:00 UTC).
    SystemTime st;
    CHECK(TimeApi::file_time_to_system_time(time.system_time_as_file_time(), st));
    CHECK_EQ(st.year, 2000);
    CHECK_EQ(st.month, 1);
    CHECK_EQ(st.day, 1);
    // Known values.
    CHECK(TimeApi::file_time_to_system_time(0, st));
    CHECK(st == (SystemTime{1601, 1, 1, 1, 0, 0, 0, 0}));            // a Monday
    CHECK(TimeApi::file_time_to_system_time(116444736000000000ull, st));   // 1970-01-01, a Thursday
    CHECK(st == (SystemTime{1970, 1, 4, 1, 0, 0, 0, 0}));
    uint64_t ft;
    CHECK(TimeApi::system_time_to_file_time(SystemTime{2024, 2, 4, 29, 12, 30, 15, 250}, ft));   // leap day
    CHECK(TimeApi::file_time_to_system_time(ft, st));
    CHECK(st == (SystemTime{2024, 2, 4, 29, 12, 30, 15, 250}));
    CHECK(!TimeApi::system_time_to_file_time(SystemTime{2023, 2, 0, 29, 0, 0, 0, 0}, ft));      // 29 February 2023 does not exist
    CHECK(!TimeApi::system_time_to_file_time(SystemTime{1600, 12, 0, 31, 0, 0, 0, 0}, ft));
    // Local time follows the clock's zone.
    null::NullClock berlin(946684800000ll, 60);
    TimeApi t2(berlin);
    CHECK_EQ(t2.file_time_to_local_file_time(116444736000000000ull) - 116444736000000000ull, 36000000000ull);
    CHECK_EQ(t2.local_file_time_to_file_time(t2.file_time_to_local_file_time(116444736000000000ull)), 116444736000000000ull);
    TimeZoneInfo zone;
    t2.get_time_zone_information(zone);
    CHECK_EQ(zone.bias_minutes, -60);
    // timeBeginPeriod / timeEndPeriod pair up.
    CHECK_EQ(time.time_begin_period(1), 0u);
    CHECK_EQ(time.time_end_period(1), 0u);
    CHECK_EQ(time.time_end_period(1), 97u);                          // unbalanced
    CHECK_EQ(time.time_begin_period(0), 97u);
}
REGISTER_TEST("time", test_time);

#if defined(_WIN32)
// The same conversions through the real kernel32: every field of 20,000 random FILETIMEs and SYSTEMTIMEs must agree.
static void test_time_against_windows() {
    std::mt19937_64 rng(12345);
    int compared = 0;
    for (int i = 0; i < 20000; ++i) {
        const uint64_t ft = rng() % (uint64_t(30000 - 1601) * 365 * 86400 * 10000000ull);
        FILETIME f{DWORD(ft), DWORD(ft >> 32)};
        SYSTEMTIME s;
        SystemTime mine;
        const bool a = FileTimeToSystemTime(&f, &s) != 0, b = TimeApi::file_time_to_system_time(ft, mine);
        CHECK_EQ(a, b);
        if (a && b) {
            ++compared;
            CHECK(s.wYear == mine.year && s.wMonth == mine.month && s.wDay == mine.day && s.wDayOfWeek == mine.day_of_week && s.wHour == mine.hour && s.wMinute == mine.minute && s.wSecond == mine.second && s.wMilliseconds == mine.milliseconds);
            FILETIME back{};
            const bool c = SystemTimeToFileTime(&s, &back) != 0;
            uint64_t mine_back = 0;
            const bool d = TimeApi::system_time_to_file_time(mine, mine_back);
            CHECK_EQ(c, d);
            if (c && d) CHECK_EQ(mine_back, (uint64_t(back.dwHighDateTime) << 32) | back.dwLowDateTime);
        }
    }
    CHECK(compared > 19000);
    // Invalid system times are rejected by both.
    SYSTEMTIME bad{2023, 2, 0, 29, 0, 0, 0, 0};
    FILETIME out;
    uint64_t mine;
    CHECK_EQ(SystemTimeToFileTime(&bad, &out) != 0, TimeApi::system_time_to_file_time(SystemTime{2023, 2, 0, 29, 0, 0, 0, 0}, mine));
}
REGISTER_TEST("time_windows", test_time_against_windows);
#endif
