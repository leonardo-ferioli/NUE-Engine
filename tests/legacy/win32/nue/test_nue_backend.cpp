// Conformance suite of the legacy platform interfaces: the same checks run against the standard-library implementation and against the one that calls the
// native API (null backend, headless). A third implementation (a host's own) is conformant when this passes with it.
#include "nue_legacy/platform/nue_platform.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "nue/nue.h"
#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace nue::legacy::platform;

namespace {
bool wait_until(const std::function<bool()>& done, int milliseconds = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void conformance(Clock& clock, Threads& threads, System& system) {
    // ---- clock
    const int64_t t0 = clock.time_ns();
    clock.sleep_ns(20 * 1000 * 1000);
    const int64_t t1 = clock.time_ns();
    CHECK(t1 >= t0 + 15 * 1000 * 1000);                                      // at least (about) what was asked
    CHECK(clock.counter_frequency() > 0);
    const uint64_t c0 = clock.counter();
    clock.sleep_ns(2 * 1000 * 1000);
    CHECK(clock.counter() > c0);
    CHECK(clock.unix_ms() > 1600000000000ll);
    const int32_t offset = clock.local_utc_offset_minutes(clock.unix_ms());
    CHECK(offset >= -14 * 60 && offset <= 14 * 60);
    clock.sleep_ns(0);                                                       // only yields
    // ---- system
    CHECK(system.info().logical_processors >= 1);
    // ---- threads: exit code through join, distinct ids
    uint64_t main_id = threads.current_thread_id(), child_id = 0;
    CHECK(main_id != 0);
    auto thread = threads.create_thread(ThreadDesc{"conformance", 0, ThreadPriority::normal, 0}, [&] { child_id = threads.current_thread_id(); return 42; });
    CHECK(thread.status == Status::ok && thread.value);
    int32_t code = 0;
    CHECK(threads.join(thread.value, &code) == Status::ok);
    CHECK_EQ(code, 42);
    CHECK(child_id != 0 && child_id != main_id);
    CHECK(threads.join(thread.value, &code) != Status::ok);                  // the handle is gone
    // ---- mutexes: recursive, trylock from another thread, unlock
    auto recursive = threads.mutex_create(true);
    CHECK(recursive.status == Status::ok);
    threads.mutex_lock(recursive.value);
    threads.mutex_lock(recursive.value);
    std::atomic<int> got{-1};
    auto other = threads.create_thread({}, [&] { got = threads.mutex_trylock(recursive.value) ? 1 : 0; if (got == 1) threads.mutex_unlock(recursive.value); return 0; });
    threads.join(other.value, nullptr);
    CHECK_EQ(got.load(), 0);                                                 // held by this thread
    threads.mutex_unlock(recursive.value);
    threads.mutex_unlock(recursive.value);
    CHECK(threads.mutex_trylock(recursive.value));
    threads.mutex_unlock(recursive.value);
    threads.mutex_destroy(recursive.value);
    // ---- condition variable: timeout, signal
    auto mutex = threads.mutex_create(false);
    auto cond = threads.cond_create();
    threads.mutex_lock(mutex.value);
    {   // a wait may return early (a spurious wake is allowed: callers re-check their predicate), but it must end in timed_out, never in an error
        Status last = Status::ok;
        const auto begin = std::chrono::steady_clock::now();
        while (last == Status::ok && std::chrono::steady_clock::now() - begin < std::chrono::seconds(2)) last = threads.cond_wait(cond.value, mutex.value, 10 * 1000 * 1000);
        CHECK(last == Status::timed_out);
    }
    threads.mutex_unlock(mutex.value);
    std::atomic<bool> woke{false}, ready{false};
    auto waiter = threads.create_thread({}, [&] {
        threads.mutex_lock(mutex.value);
        while (!ready) { if (threads.cond_wait(cond.value, mutex.value, 3000ll * 1000 * 1000) == Status::timed_out) break; }
        woke = ready.load();
        threads.mutex_unlock(mutex.value);
        return 0;
    });
    clock.sleep_ns(30 * 1000 * 1000);
    threads.mutex_lock(mutex.value);
    ready = true;
    threads.cond_broadcast(cond.value);
    threads.mutex_unlock(mutex.value);
    threads.join(waiter.value, nullptr);
    CHECK(woke.load());
    threads.cond_destroy(cond.value);
    threads.mutex_destroy(mutex.value);
    // ---- semaphore: try, timeout, post
    auto sem = threads.semaphore_create(1);
    CHECK(threads.semaphore_wait(sem.value, 0) == Status::ok);
    CHECK(threads.semaphore_wait(sem.value, 0) == Status::timed_out);
    CHECK(threads.semaphore_wait(sem.value, 5 * 1000 * 1000) == Status::timed_out);
    threads.semaphore_post(sem.value);
    CHECK(threads.semaphore_wait(sem.value, -1) == Status::ok);
    threads.semaphore_destroy(sem.value);
    // ---- thread-local storage: one value per thread
    auto slot = threads.tls_create();
    CHECK(slot.status == Status::ok);
    CHECK(threads.tls_get(slot.value) == nullptr);
    int main_value = 1, child_value = 2;
    threads.tls_set(slot.value, &main_value);
    void* seen_in_child = &main_value;
    auto tls_thread = threads.create_thread({}, [&] { seen_in_child = threads.tls_get(slot.value); threads.tls_set(slot.value, &child_value); return 0; });
    threads.join(tls_thread.value, nullptr);
    CHECK(seen_in_child == nullptr);                                         // a new thread starts with null
    CHECK(threads.tls_get(slot.value) == &main_value);                       // and does not disturb this one
    threads.tls_destroy(slot.value);
    // ---- timers: periodic until cancelled, one-shot once, destroy waits
    std::atomic<int> ticks{0};
    auto periodic = threads.timer_create(5 * 1000 * 1000, 5 * 1000 * 1000, [&] { ++ticks; });
    CHECK(periodic.status == Status::ok);
    CHECK(wait_until([&] { return ticks.load() >= 3; }));
    threads.timer_cancel(periodic.value);
    clock.sleep_ns(20 * 1000 * 1000);
    const int after_cancel = ticks.load();
    clock.sleep_ns(40 * 1000 * 1000);
    CHECK_EQ(ticks.load(), after_cancel);
    threads.timer_destroy(periodic.value);
    std::atomic<int> shots{0};
    auto once = threads.timer_create(5 * 1000 * 1000, 0, [&] { ++shots; });
    CHECK(wait_until([&] { return shots.load() >= 1; }));
    clock.sleep_ns(40 * 1000 * 1000);
    CHECK_EQ(shots.load(), 1);
    threads.timer_destroy(once.value);
}

void test_std() {
    auto clock = stdlib::make_clock();
    auto threads = stdlib::make_threads();
    auto system = stdlib::make_system();
    conformance(*clock, *threads, *system);
}

void test_nue() {
    nue_init_desc d;
    NUE_STRUCT_INIT(&d);
    d.api_version = NUE_API_VERSION;
    d.backend = NUE_BACKEND_NULL;
    d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD;
    const std::string dir = (std::filesystem::temp_directory_path() / ("nue_legacy_win32_backend_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    std::filesystem::create_directories(dir);
    d.user_data_path = dir.c_str();
    CHECK_EQ(nue_init(&d), NUE_OK);
    {
        auto clock = nue_backend::make_clock();
        auto threads = nue_backend::make_threads();
        auto system = nue_backend::make_system();
        conformance(*clock, *threads, *system);
    }
    nue_shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
}  // namespace

REGISTER_TEST("platform_conformance_std", test_std);
REGISTER_TEST("platform_conformance_nue", test_nue);
