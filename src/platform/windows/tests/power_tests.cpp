#include <atomic>
#include <cmath>
#include <thread>

#include "nue_win/cpu.hpp"
#include "nue_win/power.hpp"
#include "test_framework.hpp"

using namespace nue::win;

static void test_throttling() {
    TEST_BEGIN("power throttling");
    ThrottlingState before;
    CHECK_OK(process_query_power_throttling(before));
    CHECK_OK(process_disable_power_throttling(true));
    ThrottlingState s;
    CHECK_OK(process_query_power_throttling(s));
    CHECK(s.execution_speed_forced);
    CHECK(s.execution_speed == Support::No);   // forced OFF
    INFO("windows 11: %d, timer-resolution opt-out forced: %d", int(is_windows11()), int(s.ignore_timer_resolution_forced));
    if (is_windows11()) CHECK(s.ignore_timer_resolution_forced);
    CHECK_OK(thread_disable_power_throttling());
    CHECK_OK(thread_reset_power_throttling());
    CHECK_OK(process_reset_power_throttling());
    CHECK_OK(process_query_power_throttling(s));
    CHECK(!s.execution_speed_forced);
}

static void test_priority() {
    TEST_BEGIN("priority policy");
    CHECK(clamp_thread_priority(THREAD_PRIORITY_TIME_CRITICAL) == THREAD_PRIORITY_ABOVE_NORMAL);   // the project's cap
    CHECK(clamp_thread_priority(THREAD_PRIORITY_HIGHEST) == THREAD_PRIORITY_ABOVE_NORMAL);   // HIGHEST (2) > ABOVE_NORMAL (1)
    CHECK(clamp_thread_priority(THREAD_PRIORITY_BELOW_NORMAL) == THREAD_PRIORITY_BELOW_NORMAL);
    CHECK(clamp_thread_priority(THREAD_PRIORITY_IDLE) == THREAD_PRIORITY_IDLE);
    CHECK(clamp_thread_priority(THREAD_PRIORITY_TIME_CRITICAL, THREAD_PRIORITY_HIGHEST) == THREAD_PRIORITY_HIGHEST);
    const PriorityClass original = process_get_priority();
    CHECK_OK(process_set_priority(PriorityClass::AboveNormal));
    CHECK(process_get_priority() == PriorityClass::AboveNormal);
    CHECK_OK(process_set_priority(original));
    std::thread t([] {
        CHECK_OK(thread_set_priority(nullptr, THREAD_PRIORITY_TIME_CRITICAL));
        CHECK(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_ABOVE_NORMAL);
    });
    t.join();
}

static void test_mmcss() {
    TEST_BEGIN("MMCSS");
    std::thread t([] {
        MmcssScope m;
        Status s = m.begin(L"Games");
        if (!s.ok) { SKIP("MMCSS unavailable: %s 0x%08X", s.what, s.code); return; }
        CHECK(m.active());
        CHECK(m.task_index() != 0);
        CHECK_OK(m.set_priority(1));
        m.end();
        CHECK(!m.active());
    });
    t.join();
}

static void test_timers() {
    TEST_BEGIN("timer resolution and precise sleep");
    TimerResolutionInfo info;
    if (query_timer_resolution(info)) INFO("timer: coarsest %.3f ms, finest %.3f ms, current %.3f ms", info.minimum_ms, info.maximum_ms, info.current_ms);
    {
        TimerResolutionScope scope;
        CHECK_OK(scope.begin(1));
        TimerResolutionInfo in_effect;
        if (query_timer_resolution(in_effect)) { INFO("with timeBeginPeriod(1): current %.3f ms", in_effect.current_ms); CHECK(in_effect.current_ms <= 1.0 + 1e-6); }
    }
    PreciseSleeper sleeper;
    INFO("high-resolution waitable timer: %s", sleeper.high_resolution() ? "yes" : "no (fallback)");
    double worst_ms = 0, sum_ms = 0;
    const int rounds = 40;
    for (int i = 0; i < rounds; ++i) {
        const int64_t t0 = qpc_now();
        sleeper.sleep_ns(1000000);   // 1 ms
        const double dt = qpc_to_ms(qpc_now() - t0);
        CHECK(dt >= 1.0 - 1e-3);      // never early
        sum_ms += dt;
        worst_ms = std::max(worst_ms, dt);
    }
    INFO("sleep 1 ms: mean %.4f ms, worst %.4f ms", sum_ms / rounds, worst_ms);
    CHECK(sum_ms / rounds < 2.5);     // loose bound: a loaded CI machine (a build running next to the test) may spike
    const int64_t t0 = qpc_now();
    sleeper.sleep_ns(20000000);
    const double dt = qpc_to_ms(qpc_now() - t0);
    CHECK(dt >= 20.0 - 1e-3 && dt < 100.0);
}

static void test_wakelock() {
    TEST_BEGIN("wake lock");
    WakeLock w;
    CHECK_OK(w.acquire(L"NUE test: no real effect expected beyond the request itself"));
    CHECK(w.active());
    INFO("PowerCreateRequest path: %s", w.using_power_request() ? "yes" : "no (SetThreadExecutionState fallback)");
    w.release();
    CHECK(!w.active());
}

static void test_status() {
    TEST_BEGIN("power status / mode / frequency");
    PowerStatus p = query_power_status();
    CHECK(p.valid);
    INFO("AC: %s, battery: %s %d%%, energy saver: %d", support_name(p.on_ac), p.has_battery ? "yes" : "no", p.battery_percent, int(p.energy_saver));
    EffectivePowerMode m = query_effective_power_mode();
    INFO("effective power mode: %s (%d)", power_mode_name(m), int(m));
    CpuFrequencyInfo f = query_cpu_frequency();
    CHECK(f.valid);
    INFO("cpu: max %u MHz, current %u MHz, limit %u MHz, limited: %d", f.max_mhz, f.current_mhz, f.limit_mhz, int(f.limited()));
}

static void test_notifier() {
    TEST_BEGIN("power notifier");
    PowerNotifier n;
    std::atomic<int> got{0};
    PowerEvent last{};
    Status s = n.start([&](const PowerEvent& e) { last = e; ++got; });
    CHECK_OK(s);
    INFO("registered sources mask: 0x%X (bit0 suspend/resume, bits1-4 settings, bit5 power mode)", n.registered_mask());
    CHECK(n.active());
    n.inject_for_test({PowerEvent::Suspend, 0});
    CHECK(got.load() >= 1);
    CHECK(last.kind == PowerEvent::Suspend || last.kind == PowerEvent::PowerMode || last.kind == PowerEvent::PowerSource || last.kind == PowerEvent::BatteryPercent || last.kind == PowerEvent::DisplayState || last.kind == PowerEvent::EnergySaver);
    n.stop();
    CHECK(!n.active());
    const int count = got.load();
    n.inject_for_test({PowerEvent::Suspend, 0});
    CHECK(got.load() == count);   // no callback after stop
}

static void test_cpu() {
    TEST_BEGIN("cpu topology");
    CpuTopology t;
    CHECK(query_cpu_topology(t));
    INFO("%s [%s]: %u logical, %u physical, groups %u, hybrid %d, smt %d, perf sets %zu, eff sets %zu, wow64 %d", t.brand.c_str(), t.vendor.c_str(),
         t.logical_processors, t.physical_cores, t.processor_groups, int(t.hybrid), int(t.smt), t.performance_set_ids.size(), t.efficiency_set_ids.size(), int(t.wow64));
    CHECK(t.logical_processors >= 1);
    CHECK(t.physical_cores >= 1 && t.physical_cores <= t.logical_processors);
    CHECK(t.performance_set_ids.size() + t.efficiency_set_ids.size() == t.logical_processors);
    CHECK(!t.performance_primary_set_ids.empty());

    // Synthetic hybrid CPU: 2 P-cores with SMT (4 threads, class 1) + 4 E-cores (class 0)
    CpuTopology h;
    uint32_t id = 256;
    for (uint8_t core = 0; core < 2; ++core) for (int smt = 0; smt < 2; ++smt) { CpuSetInfo s; s.id = id++; s.core_index = core; s.efficiency_class = 1; h.sets.push_back(s); }
    for (uint8_t core = 2; core < 6; ++core) { CpuSetInfo s; s.id = id++; s.core_index = core; s.efficiency_class = 0; h.sets.push_back(s); }
    classify_cpu_sets(h);
    CHECK(h.hybrid);
    CHECK(h.logical_processors == 8 && h.physical_cores == 6 && h.smt);
    CHECK(h.performance_set_ids.size() == 4 && h.efficiency_set_ids.size() == 4 && h.performance_primary_set_ids.size() == 2);
    CHECK(h.performance_primary_set_ids[0] == 256 && h.performance_primary_set_ids[1] == 258);
    CpuTopology flat;
    for (uint8_t core = 0; core < 4; ++core) { CpuSetInfo s; s.id = core; s.core_index = core; flat.sets.push_back(s); }
    classify_cpu_sets(flat);
    CHECK(!flat.hybrid && flat.performance_set_ids.size() == 4 && flat.efficiency_set_ids.empty() && !flat.smt);

    // Placement on the real machine: restrict this thread to its performance class, check where it runs, then lift the restriction.
    std::thread th([&] {
        CHECK_OK(thread_use_core_class(nullptr, t, CoreClass::Performance));
        for (int i = 0; i < 20; ++i) Sleep(1);
        INFO("running on processor %u after selecting performance class", current_processor_number());
        CHECK_OK(thread_use_core_class(nullptr, t, CoreClass::Any));
        CHECK_OK(thread_set_ideal_processor(nullptr, 0, 0));
    });
    th.join();
    // Affinity mask: pin to logical processor 0 of the thread's group, restore afterwards.
    std::thread th2([] {
        DWORD_PTR mask = 0, system_mask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &mask, &system_mask);
        CHECK_OK(thread_set_affinity_mask(nullptr, 1));
        for (int i = 0; i < 5; ++i) Sleep(1);
        CHECK(current_processor_number() == 0);
        CHECK_OK(thread_set_affinity_mask(nullptr, uint64_t(mask)));
    });
    th2.join();
}

int main() {
    test_throttling();
    test_priority();
    test_mmcss();
    test_timers();
    test_wakelock();
    test_status();
    test_notifier();
    test_cpu();
    return finish("power_tests");
}
