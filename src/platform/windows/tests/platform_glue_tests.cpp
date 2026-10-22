// Tests of nue_platform_windows_get_ops(): the table the core uses. The core's own services (event queue, clock) are replaced by the small
// stubs below, so this test needs neither the core library nor SDL3, a window or an audio device.
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "internal/platform_ops.h"
#include "nue_win/diag.hpp"
#include "nue_win/power.hpp"
#include "nue_win/storage.hpp"
#include "test_framework.hpp"

// ---- stubs of the core services the platform layer calls ------------------------------------------------------------------------------------
static std::vector<nue_event> g_events;
extern "C" void NUE_CALL nue_internal_push_event(const nue_event* e) { g_events.push_back(*e); }
extern "C" int64_t NUE_CALL nue_time_ns(void) { return int64_t(nue::win::qpc_now()) * 1000000000ll / nue::win::qpc_frequency(); }

using namespace nue::win;

static const nue_platform_ops* ops() { return nue_platform_windows_get_ops(); }

static void test_table() {
    TEST_BEGIN("ops table");
    const nue_platform_ops* o = ops();
    CHECK(o != nullptr && ops() == o);
    CHECK(std::strcmp(o->name, "windows") == 0 && o->struct_size == sizeof(nue_platform_ops));
    CHECK(o->init && o->shutdown && o->pump && o->system_info && o->memory_info && o->storage_root && o->power_info && o->power_apply && o->crash_install && o->crash_release_input && o->log_write);
    CHECK(o->perf_hint_create == nullptr && o->haptics_available == nullptr && o->window_set_gpu_preference == nullptr && o->import == nullptr);   // nothing real behind them: left NULL
    auto has = [&](nue_capability c) { return (o->capabilities[unsigned(c) / 64] >> (unsigned(c) % 64)) & 1; };
    CHECK(has(NUE_CAP_GAME_MODE) && has(NUE_CAP_KEEP_AWAKE) && has(NUE_CAP_TIMER_RESOLUTION) && has(NUE_CAP_THREAD_PRIORITY) && has(NUE_CAP_CRASH_HANDLER) && has(NUE_CAP_MINIDUMP));
    CHECK(!has(NUE_CAP_PERF_HINT) && !has(NUE_CAP_GPU_PREFERENCE) && !has(NUE_CAP_HAPTICS) && !has(NUE_CAP_WINDOWS));   // SDL3 or other layers provide windows; no run-time GPU preference
    // every set capability has the entries it needs
    if (has(NUE_CAP_BATTERY) || has(NUE_CAP_LOW_POWER_MODE) || has(NUE_CAP_THERMAL_STATE)) CHECK(o->power_info != nullptr);
    if (has(NUE_CAP_THREAD_AFFINITY)) CHECK(o->thread_set_affinity != nullptr);
    if (has(NUE_CAP_CORE_CLASS)) CHECK(o->thread_set_core_class != nullptr);
    INFO("capabilities: 0x%016llX 0x%016llX", (unsigned long long)o->capabilities[0], (unsigned long long)o->capabilities[1]);
}

static nue_init_desc make_desc() {
    nue_init_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.app_id = "gluetest";
    d.org_name = "NUETests";
    d.app_name = "Glue Test";
    return d;
}

static void test_system() {
    TEST_BEGIN("system info, memory, locale");
    nue_system_info si;
    std::memset(&si, 0, sizeof si);
    si.struct_size = sizeof si;
    CHECK(ops()->system_info(&si) == NUE_OK);
    INFO("%s %s (build %u), %s, %s \"%s\", %u logical / %u physical cores, %u MHz, %llu MiB RAM, %u GPU(s), %u display(s), locale %s, dark mode %d, translation %d", si.os_name, si.os_version, si.os_build,
         si.arch == NUE_ARCH_KIND_X86 ? "x86" : (si.arch == NUE_ARCH_KIND_X64 ? "x64" : "other"), si.device_class == NUE_DEVICE_LAPTOP ? "laptop" : "desktop", si.device_model, si.cpu_logical_cores,
         si.cpu_physical_cores, si.cpu_max_mhz, (unsigned long long)(si.memory_total_bytes >> 20), si.gpu_count, si.display_count, si.locale, int(si.dark_mode), int(si.running_under_translation));
    CHECK(si.os == NUE_OS_WINDOWS && si.os_build >= 10240 && std::strncmp(si.os_name, "Windows", 7) == 0);
    CHECK(si.arch == (sizeof(void*) == 4 ? NUE_ARCH_KIND_X86 : NUE_ARCH_KIND_X64));
    CHECK(si.cpu_name[0] != 0 && si.cpu_logical_cores >= 1 && si.cpu_physical_cores >= 1 && si.cpu_physical_cores <= si.cpu_logical_cores);
    CHECK(si.memory_total_bytes > (512ull << 20) && si.memory_available_bytes > 0 && si.memory_available_bytes <= si.memory_total_bytes);
    CHECK(si.gpu_count >= 1 && si.gpus[0].name[0] != 0 && si.gpus[0].vendor_id != 0);
    CHECK(si.display_count >= 1 && si.locale[0] != 0);
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86_FP)
    CHECK(si.cpu_features & NUE_CPU_SSE2);
#endif
    uint64_t total = 0, avail = 0;
    CHECK(ops()->memory_info(&total, &avail) == NUE_OK && total == si.memory_total_bytes);
    char loc[32];
    CHECK(ops()->system_locale(loc, sizeof loc) == NUE_OK && loc[0] != 0);
    CHECK(ops()->system_info(nullptr) == NUE_ERR_INVALID_ARGUMENT);
}

static void test_storage() {
    TEST_BEGIN("storage roots");
    nue_init_desc d = make_desc();
    char buf[512];
    size_t need = 0;
    for (nue_root_kind k : {NUE_ROOT_DATA, NUE_ROOT_USER, NUE_ROOT_CACHE, NUE_ROOT_SETTINGS, NUE_ROOT_TEMP}) {
        CHECK(ops()->storage_root(k, &d, buf, sizeof buf, &need) == NUE_OK);
        INFO("root %d -> %s", int(k), buf);
        CHECK(need == std::strlen(buf) && need > 3);
    }
    CHECK(ops()->storage_root(NUE_ROOT_USER, &d, buf, sizeof buf, &need) == NUE_OK && std::string(buf).find("Saved Games") != std::string::npos);
    CHECK(ops()->storage_root(NUE_ROOT_SETTINGS, &d, buf, sizeof buf, &need) == NUE_OK && std::string(buf).find("NUETests\\gluetest") != std::string::npos);
    d.user_data_path = "D:/Portable/Saves";
    d.game_data_path = "D:/Game/Data";
    CHECK(ops()->storage_root(NUE_ROOT_USER, &d, buf, sizeof buf, &need) == NUE_OK && std::string(buf) == "D:\\Portable\\Saves");   // overrides win, separators normalised
    CHECK(ops()->storage_root(NUE_ROOT_DATA, &d, buf, sizeof buf, &need) == NUE_OK && std::string(buf) == "D:\\Game\\Data");
    char tiny[8];
    CHECK(ops()->storage_root(NUE_ROOT_USER, &d, tiny, sizeof tiny, &need) == NUE_OK_PARTIAL && need == 17 && std::strlen(tiny) == 7);   // truncated, full size reported
    CHECK(ops()->storage_root(nue_root_kind(77), &d, buf, sizeof buf, &need) == NUE_ERR_UNSUPPORTED);
}

static void test_power() {
    TEST_BEGIN("power info and apply");
    nue_power_info pi;
    CHECK(ops()->power_info(&pi) == NUE_OK && pi.struct_size == sizeof pi);
    INFO("state %d, battery %d%%, AC %d, low power %d, thermal %d, game mode %d, keep-awake %d", int(pi.state), pi.battery_percent, int(pi.on_ac_power), int(pi.low_power_mode), int(pi.thermal), int(pi.game_mode),
         int(pi.display_keep_awake_active));
    CHECK(pi.state != NUE_POWER_UNKNOWN);
    CHECK(pi.battery_percent >= -1 && pi.battery_percent <= 100);

    const PriorityClass original = process_get_priority();
    nue_power_request r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    r.profile = NUE_PERF_HIGH;
    r.game_hint = NUE_GAME_HINT_GAMEPLAY;
    r.keep_awake = NUE_KEEP_AWAKE_DISPLAY | NUE_KEEP_AWAKE_SYSTEM;
    r.timer_resolution_us = 1000;
    r.disable_power_throttling = NUE_TRUE;
    nue_power_applied applied = 0;
    CHECK(ops()->power_apply(&r, &applied) == NUE_OK);
    INFO("applied mask 0x%X", applied);
    CHECK((applied & NUE_POWER_APPLIED_PROFILE) && (applied & NUE_POWER_APPLIED_KEEP_AWAKE) && (applied & NUE_POWER_APPLIED_TIMER) && (applied & NUE_POWER_APPLIED_THROTTLING));
    CHECK(process_get_priority() == PriorityClass::AboveNormal);    // HIGH is capped at ABOVE_NORMAL: the project policy
    ThrottlingState ts;
    CHECK_OK(process_query_power_throttling(ts));
    CHECK(ts.execution_speed_forced && ts.execution_speed == Support::No);
    uint32_t granted_us = 0;
    CHECK(ops()->timer_resolution_get(&granted_us) == NUE_OK && granted_us <= 1000);
    nue_power_info during;
    ops()->power_info(&during);
    CHECK(during.display_keep_awake_active);

    // half-honoured request: GPU preference and frame time hints have no Windows mechanism
    r.gpu = NUE_GPU_HIGH_PERFORMANCE;
    r.target_frame_time_us = 16666;
    CHECK(ops()->power_apply(&r, &applied) == NUE_OK_PARTIAL && !(applied & NUE_POWER_APPLIED_GPU) && !(applied & NUE_POWER_APPLIED_FRAME_TIME));

    r.gpu = NUE_GPU_DEFAULT;
    r.target_frame_time_us = 0;
    r.timer_resolution_us = 500;
    CHECK(ops()->power_apply(&r, &applied) == NUE_OK);
    uint32_t us = 0;
    CHECK(ops()->timer_resolution_get(&us) == NUE_OK);
    INFO("timer resolution after asking for 500 us: %u us", us);
    CHECK(us > 0 && us <= 1000);

    // an all-zero request releases everything
    nue_power_request none;
    std::memset(&none, 0, sizeof none);
    none.struct_size = sizeof none;
    CHECK(ops()->power_apply(&none, &applied) == NUE_OK && applied == 0);
    CHECK(process_get_priority() == original);
    CHECK_OK(process_query_power_throttling(ts));
    CHECK(!ts.execution_speed_forced);
    ops()->power_info(&during);
    CHECK(!during.display_keep_awake_active);

    // power saver forces EcoQoS on and drops the priority class, and shutdown undoes it
    r = none;
    r.profile = NUE_PERF_POWER_SAVER;
    CHECK(ops()->power_apply(&r, &applied) == NUE_OK && process_get_priority() == PriorityClass::BelowNormal);
    CHECK_OK(process_query_power_throttling(ts));
    CHECK(ts.execution_speed_forced && ts.execution_speed == Support::Yes);
    CHECK(ops()->power_apply(&none, &applied) == NUE_OK);
    CHECK(process_get_priority() == original);
    CHECK_OK(process_query_power_throttling(ts));
    CHECK(!ts.execution_speed_forced);
    CHECK(ops()->power_apply(nullptr, &applied) == NUE_ERR_INVALID_ARGUMENT);
}

static void test_threads() {
    TEST_BEGIN("thread priority, affinity, core class, name");
    std::thread worker([] { Sleep(400); });
    nue_native_thread_ref ref;
    std::memset(&ref, 0, sizeof ref);
    ref.struct_size = sizeof ref;
    ref.handle = uint64_t(reinterpret_cast<uintptr_t>(worker.native_handle()));
    CHECK(ops()->thread_set_priority(&ref, NUE_THREAD_PRIORITY_HIGH) == NUE_OK);
    CHECK(GetThreadPriority(worker.native_handle()) == THREAD_PRIORITY_ABOVE_NORMAL);   // capped
    nue_thread_priority back;
    CHECK(ops()->thread_get_priority(&ref, &back) == NUE_OK && back == NUE_THREAD_PRIORITY_ABOVE_NORMAL);
    CHECK(ops()->thread_set_priority(&ref, NUE_THREAD_PRIORITY_REALTIME) == NUE_OK && GetThreadPriority(worker.native_handle()) == THREAD_PRIORITY_ABOVE_NORMAL);   // never TIME_CRITICAL
    CHECK(ops()->thread_set_priority(&ref, NUE_THREAD_PRIORITY_LOW) == NUE_OK && GetThreadPriority(worker.native_handle()) == THREAD_PRIORITY_BELOW_NORMAL);
    // by thread id (the form a recompiled game's CreateThread gives)
    nue_native_thread_ref by_id;
    std::memset(&by_id, 0, sizeof by_id);
    by_id.struct_size = sizeof by_id;
    by_id.id = GetThreadId(worker.native_handle());
    CHECK(ops()->thread_set_priority(&by_id, NUE_THREAD_PRIORITY_NORMAL) == NUE_OK && GetThreadPriority(worker.native_handle()) == THREAD_PRIORITY_NORMAL);
    nue_native_thread_ref bogus = by_id;
    bogus.id = 0x7FFFFFF0;
    CHECK(ops()->thread_set_priority(&bogus, NUE_THREAD_PRIORITY_HIGH) == NUE_ERR_NOT_FOUND);

    CHECK(ops()->thread_set_affinity(&ref, 1) == NUE_OK);
    CHECK(ops()->thread_set_affinity(&ref, 0) == NUE_OK);   // 0 = all CPUs of the process
    const nue_result cc = ops()->thread_set_core_class(&ref, NUE_CPU_PERFORMANCE_CORES);
    INFO("core class on this CPU: %s", cc == NUE_OK_NOOP ? "no-op (not a hybrid CPU)" : "applied");
    CHECK(cc == NUE_OK || cc == NUE_OK_NOOP);
    worker.join();

    // calling thread
    CHECK(ops()->thread_set_priority(nullptr, NUE_THREAD_PRIORITY_REALTIME) == NUE_OK);   // MMCSS "Games" on this thread
    const int rt = GetThreadPriority(GetCurrentThread());
    INFO("priority of this thread after REALTIME (MMCSS Games, base priority 9): %d", rt);
    CHECK(rt == THREAD_PRIORITY_ABOVE_NORMAL || rt == 9);   // MMCSS reports its absolute base priority; 9 is the normal class' ABOVE_NORMAL. Never 15 (TIME_CRITICAL).
    CHECK(ops()->thread_set_priority(nullptr, NUE_THREAD_PRIORITY_NORMAL) == NUE_OK && GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL);
    INFO("priority after NORMAL: %d", GetThreadPriority(GetCurrentThread()));
    CHECK(ops()->thread_set_name("nue glue test") == NUE_OK);

    // precise sleep in the core's clock domain
    const int64_t t0 = nue_time_ns();
    CHECK(ops()->sleep_until(t0 + 5000000) == NUE_OK);
    const int64_t dt = nue_time_ns() - t0;
    INFO("sleep_until(+5 ms) took %.3f ms", dt / 1e6);
    CHECK(dt >= 5000000 && dt < 15000000);
}

static void test_pump_and_logging() {
    TEST_BEGIN("pump and log sink");
    g_events.clear();
    ops()->pump();
    ops()->pump();
    INFO("events after two pumps: %zu (the first sample is only the baseline)", g_events.size());
    for (const auto& e : g_events) CHECK(e.type >= NUE_EVENT_POWER_CHANGED && e.type <= NUE_EVENT_GAME_MODE_CHANGED);
    ops()->log_write(NUE_LOG_INFO, "glue", "hello from the Windows platform layer test (OutputDebugString)");
    ops()->log_write(NUE_LOG_FATAL, nullptr, nullptr);
}

// ---- crash: a child process installs the handler through the ops table and crashes -------------------------------------------------------

static char g_report_dir[2 * MAX_PATH];
static const char* sink_begin(const nue_crash_info* info, void*) {
    // A real core writes report.txt here; the test leaves a marker with the reason.
    std::string marker = std::string(g_report_dir) + "\\begin.txt";
    HANDLE f = CreateFileA(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        WriteFile(f, info->reason, DWORD(std::strlen(info->reason)), &n, nullptr);
        CloseHandle(f);
    }
    return g_report_dir;
}
static nue_bool sink_end(const nue_crash_info* info, void*) {
    std::string marker = std::string(g_report_dir) + "\\end.txt";
    HANDLE f = CreateFileA(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const char* dir = info->report_dir ? info->report_dir : "(null)";
        DWORD n = 0;
        WriteFile(f, dir, DWORD(std::strlen(dir)), &n, nullptr);
        CloseHandle(f);
    }
    return NUE_TRUE;
}

static int child_main(const char* dir) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    std::snprintf(g_report_dir, sizeof g_report_dir, "%s", dir);
    nue_crash_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.flags = NUE_CRASH_DEFAULT;
    nue_crash_sink sink;
    std::memset(&sink, 0, sizeof sink);
    sink.struct_size = sizeof sink;
    sink.begin = sink_begin;
    sink.end = sink_end;
    if (ops()->crash_install(&d, &sink) != NUE_OK) return 90;
    *reinterpret_cast<volatile int*>(0x20) = 1;
    return 91;
}

static std::vector<uint8_t> read_all(const std::wstring& p) {
    std::vector<uint8_t> v;
    read_file(p, v);
    return v;
}

static void test_crash_through_ops() {
    TEST_BEGIN("crash handler through the ops table (child process)");
    std::wstring temp;
    known_folder_path(KnownFolder::Temp, temp);
    const std::wstring dir = temp + L"\\nue_glue_crash_" + std::to_wstring(GetCurrentProcessId());
    remove_tree(dir);
    create_directories(dir);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --crash-child \"" + dir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0);
    DWORD code = 0;
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 999);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    INFO("child exit code 0x%08lX", code);
    CHECK(code == 0xC0000005);
    DumpSummary s;
    CHECK(read_minidump_summary(dir + L"\\crash.dmp", s));
    CHECK(s.valid && s.has_exception && s.exception_code == 0xC0000005 && s.module_count >= 3);
    const auto begin = read_all(dir + L"\\begin.txt");
    const std::string reason(begin.begin(), begin.end());
    INFO("reason handed to the sink: \"%s\"", reason.c_str());
    CHECK(reason == "access violation writing 0x20");
    const auto end = read_all(dir + L"\\end.txt");
    CHECK(std::string(end.begin(), end.end()) == path_to_utf8(dir));   // report_dir is passed to end()

    // non-fatal dump of this live process
    const std::string d8 = path_to_utf8(dir);
    CHECK(ops()->crash_write_dump(d8.c_str(), 0) == NUE_OK);
    CHECK(read_minidump_summary(dir + L"\\crash.dmp", s) && s.valid && !s.has_exception);
    remove_tree(dir);
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--crash-child") == 0) return child_main(argv[2]);
    test_table();
    nue_init_desc d = make_desc();
    nue_backend_init bi;
    std::memset(&bi, 0, sizeof bi);
    bi.struct_size = sizeof bi;
    bi.desc = &d;
    CHECK(ops()->init(&bi) == NUE_OK);
    const PriorityClass original = process_get_priority();
    test_system();
    test_storage();
    test_power();
    test_threads();
    test_pump_and_logging();
    test_crash_through_ops();
    ops()->shutdown();
    CHECK(process_get_priority() == original);
    ops()->shutdown();   // idempotent
    return finish("platform_glue_tests");
}
