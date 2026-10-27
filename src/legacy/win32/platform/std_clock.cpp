#include "nue_legacy/platform/std_platform.hpp"
#include <chrono>
#include <ctime>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nue::legacy::platform::stdlib {
namespace {
class StdClock final : public Clock {
public:
    StdClock() : start_(std::chrono::steady_clock::now()) {}
    int64_t time_ns() override { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count(); }
    uint64_t counter() override {
#if defined(_WIN32)
        LARGE_INTEGER v; QueryPerformanceCounter(&v); return uint64_t(v.QuadPart);
#else
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }
    uint64_t counter_frequency() override {
#if defined(_WIN32)
        static const uint64_t f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return uint64_t(v.QuadPart); }();
        return f;
#else
        return 1000000000ull;
#endif
    }
    int64_t unix_ms() override { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    int32_t local_utc_offset_minutes(int64_t unix_ms_value) override {
        const std::time_t t = std::time_t(unix_ms_value / 1000);
        std::tm local{}, utc{};
#if defined(_WIN32)
        localtime_s(&local, &t);
        gmtime_s(&utc, &t);
#else
        localtime_r(&t, &local);
        gmtime_r(&t, &utc);
#endif
        // Difference of the two civil times, read as if both were UTC (days_from_civil: Howard Hinnant's algorithm).
        auto seconds = [](const std::tm& c) {
            const int64_t y = int64_t(c.tm_year) + 1900 - (c.tm_mon < 2), m = c.tm_mon + 1;
            const int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
            const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + c.tm_mday - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            const int64_t days = era * 146097 + doe - 719468;
            return days * 86400 + c.tm_hour * 3600 + c.tm_min * 60 + c.tm_sec;
        };
        return int32_t((seconds(local) - seconds(utc)) / 60);
    }
    void sleep_ns(int64_t ns) override {
        if (ns <= 0) { std::this_thread::yield(); return; }
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
private:
    std::chrono::steady_clock::time_point start_;
};
}  // namespace

std::unique_ptr<Clock> make_clock() { return std::make_unique<StdClock>(); }
}  // namespace nue::legacy::platform::stdlib
