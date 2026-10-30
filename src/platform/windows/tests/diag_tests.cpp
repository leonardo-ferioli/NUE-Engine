// Diagnostics tests. Crash handling is verified for real: the test re-launches itself as a child that installs the handler and then crashes in
// different ways; the parent checks the exit code and reads the minidump back.
#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "nue_win/diag.hpp"
#include "nue_win/power.hpp"
#include "nue_win/storage.hpp"
#include "nue_win/window.hpp"
#include "test_framework.hpp"
#include "test_window.hpp"

using namespace nue::win;

static volatile int g_zero = 0;

// Fault helpers live in seh_helpers.cpp (built with asynchronous exception tables).
void raise_handled(unsigned long code);
void write_bad_address(int address);
void throw_and_catch_cxx();

static int recurse(int n) {
    volatile char pad[256];
    pad[0] = char(n);
    return recurse(n + 1) + pad[0];
}

static DWORD WINAPI deep_thread(LPVOID) {
    reserve_crash_stack();   // every game thread that can run deep code does this once
    return DWORD(recurse(0));
}

static int child_main(const std::wstring& dir, const std::string& kind) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    CrashOptions o;
    o.dump_dir = dir;
    o.app_name = L"child";
    o.build_id = "diag_tests child " + kind;
    o.max_dumps_kept = 3;
    if (!install_crash_handler(o).ok) return 90;
    install_exception_log(64);
    if (kind == "av") { *reinterpret_cast<volatile int*>(0x10) = 1; }
    else if (kind == "stack") { return recurse(0); }
    else if (kind == "stack_thread") {   // overflow on a small worker stack: the thread reserves room for the handler, as every game thread should
        HANDLE t = CreateThread(nullptr, 256 * 1024, deep_thread, nullptr, 0, nullptr);
        WaitForSingleObject(t, 60000);
        return 93;
    }
    else if (kind == "raise") { RaiseException(0xE0000042, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
    else if (kind == "divzero") { return 100 / g_zero; }
    else if (kind == "veh_only") {
        raise_handled(0xE0000077);   // handled: logged, not a crash
        return exception_log_total() >= 1 ? 0 : 91;
    }
    return 92;
}

struct ChildResult { DWORD exit_code = 0; bool started = false; };
static ChildResult run_child(const std::wstring& dir, const char* kind) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --crash-child \"" + dir + L"\" " + to_wide(kind);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    ChildResult r;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return r;
    r.started = true;
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 999);
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

static std::vector<std::wstring> list_files(const std::wstring& dir, const wchar_t* pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { out.push_back(dir + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

static void test_crash_dumps() {
    TEST_BEGIN("crash handler: real crashes in child processes");
    std::wstring temp;
    known_folder_path(KnownFolder::Temp, temp);
    const std::wstring dir = temp + L"\\nue_win_diag_test_" + std::to_wstring(GetCurrentProcessId());
    remove_tree(dir);

    struct Case { const char* kind; DWORD expected; const char* text; };
    const Case cases[] = {
        {"av", 0xC0000005, "ACCESS_VIOLATION"},
        {"divzero", 0xC0000094, "INT_DIVIDE_BY_ZERO"},
        {"raise", 0xE0000042, "unknown"},
        {"stack", 0xC00000FD, "STACK_OVERFLOW"},
        {"stack_thread", 0xC00000FD, "STACK_OVERFLOW"},
    };
    for (const Case& c : cases) {
        const ChildResult r = run_child(dir, c.kind);
        CHECK(r.started);
        INFO("%s: exit code 0x%08lX", c.kind, r.exit_code);
        CHECK_MSG(r.exit_code == c.expected, "expected 0x%08lX", c.expected);
        const auto dumps = list_files(dir, L"child-*.dmp");
        CHECK(!dumps.empty());
        if (dumps.empty()) continue;
        DumpSummary s;
        CHECK(read_minidump_summary(dumps.back(), s));
        INFO("dump %s: exception 0x%08X at 0x%llX thread %u, %u modules, %u threads, handle data %d, thread info %d", path_to_utf8(dumps.back().substr(dumps.back().find_last_of(L'\\') + 1)).c_str(), s.exception_code,
             (unsigned long long)s.exception_address, s.crashing_thread_id, s.module_count, s.thread_count, int(s.has_handle_data), int(s.has_thread_info));
        CHECK(s.valid && s.has_exception && s.exception_code == c.expected);
        CHECK(s.module_count >= 3 && s.thread_count >= 1 && s.has_system_info);
        CHECK(s.has_handle_data && s.has_thread_info);
        const auto reports = list_files(dir, L"child-*.txt");
        CHECK(!reports.empty());
        if (!reports.empty()) {
            std::vector<uint8_t> text;
            CHECK_OK(read_file(reports.back(), text));
            const std::string t(text.begin(), text.end());
            CHECK(t.find(c.text) != std::string::npos);
            CHECK(t.find("diag_tests child") != std::string::npos);   // build id
            CHECK(t.find("modules:") != std::string::npos);
            if (std::string(c.kind) == "av") CHECK(t.find("access violation: writing address 0x10") != std::string::npos);
        }
    }
    // rotation: max_dumps_kept = 3 and we ran 4 children
    CHECK(list_files(dir, L"child-*.dmp").size() <= 3);

    const ChildResult v = run_child(dir, "veh_only");
    CHECK(v.started && v.exit_code == 0);   // a handled exception is logged and is not a crash
    remove_tree(dir);
}

static void test_exception_log() {
    TEST_BEGIN("first-chance exception log");
    CHECK_OK(install_exception_log(16));
    const uint64_t before = exception_log_total();
    for (int i = 0; i < 3; ++i) raise_handled(0xE0000100 + i);
    write_bad_address(8);
    throw_and_catch_cxx();                                                 // C++ exceptions are filtered out
    const auto log = exception_log_snapshot();
    CHECK(exception_log_total() == before + 4);
    CHECK(log.size() >= 4);
    const auto& av = log.back();
    CHECK(av.code == 0xC0000005 && av.info0 == 1 && av.info1 == 8);   // write access at address 8
    CHECK(log[log.size() - 4].code == 0xE0000100 && log[log.size() - 2].code == 0xE0000102);
    for (int i = 0; i < 40; ++i) raise_handled(0xE0000200);
    CHECK(exception_log_snapshot().size() == 16);                       // ring: only the newest 16 are kept
    remove_exception_log();
    const uint64_t t = exception_log_total();
    raise_handled(0xE0000300);
    CHECK(exception_log_total() == t);                                   // removed: nothing recorded
}

static void test_dump_without_exception() {
    TEST_BEGIN("on-demand minidump (hang report path)");
    std::wstring temp;
    known_folder_path(KnownFolder::Temp, temp);
    const std::wstring f = temp + L"\\nue_win_ondemand_" + std::to_wstring(GetCurrentProcessId()) + L".dmp";
    CHECK_OK(write_minidump(nullptr, f, false));
    DumpSummary s;
    CHECK(read_minidump_summary(f, s));
    CHECK(s.valid && !s.has_exception && s.module_count >= 3);
    DeleteFileW(f.c_str());
}

static void test_etw() {
    TEST_BEGIN("ETW provider");
    CHECK_OK(etw_register());
    CHECK_OK(etw_register());   // idempotent
    INFO("a session is listening: %d (expected 0 unless you started one)", int(etw_enabled()));
    etw_event("test", "detail", 7);
    etw_frame(1, 3.5, 16.6);
    etw_mark("mark");
    etw_unregister();
    etw_event("after-unregister");   // must be a no-op
    CHECK(!etw_enabled());
}

static void test_sampling() {
    TEST_BEGIN("cpu / memory / gpu engines / thermal");
    const ProcessSample a = sample_process();
    volatile double sink = 0;
    const int64_t t0 = qpc_now();
    while (qpc_to_ms(qpc_now() - t0) < 100.0) sink = sink + std::sqrt(double(qpc_now() & 1023));   // burn 100 ms of CPU on this thread
    const ProcessSample b = sample_process();
    const CpuUsage u = cpu_usage_between(a, b, 16);
    INFO("busy loop: process %.2f cores, thread %.2f cores, %.1f%% of a 16-thread machine; thread cycles %llu", u.cores, u.thread_cores, u.machine_fraction * 100, (unsigned long long)(b.thread_cycles - a.thread_cycles));
    CHECK(u.thread_cores > 0.5 && u.thread_cores < 1.3);
    CHECK(u.cores >= u.thread_cores * 0.9);
    CHECK(b.thread_cycles > a.thread_cycles);

    const MemorySample m = sample_memory();
    INFO("working set %llu MiB, private %llu MiB, handles %u, GDI %u, USER %u, threads %u", (unsigned long long)(m.working_set >> 20), (unsigned long long)(m.private_bytes >> 20), m.handle_count, m.gdi_objects,
         m.user_objects, m.threads);
    CHECK(m.valid && m.working_set > 0 && m.handle_count > 0 && m.threads >= 1);

    // the Quick sample is what win_pump uses every second: it must not do the system-wide thread walk (60 ms on a laptop = one stuttering frame per second)
    const MemorySample q = sample_memory(MemoryDetail::Quick);
    CHECK(q.valid && q.working_set > 0 && q.private_bytes > 0);
    CHECK(q.threads == 0 && q.handle_count == 0 && q.gdi_objects == 0 && q.user_objects == 0);

    GpuEngineSampler gpu;
    Status s = gpu.open();
    if (!s.ok) { SKIP("GPU engine counters unavailable (%s)", s.what); }
    else {
        Sleep(300);
        const auto g = gpu.sample();
        INFO("GPU engines for this process: valid %d, instances %u, 3D %.1f%%, any %.1f%%, dedicated %llu KiB, shared %llu KiB (this console test does not use the GPU)", int(g.valid), g.instances, g.engine_3d_percent,
             g.any_engine_percent, (unsigned long long)(g.dedicated_bytes >> 10), (unsigned long long)(g.shared_bytes >> 10));
        CHECK(g.valid);
    }
    const auto zones = read_thermal_zones();
    INFO("thermal zones: %zu", zones.size());
    for (const auto& z : zones) INFO("  %s: %.1f C, passive limit %.0f%%", z.name.c_str(), z.celsius, z.passive_limit_percent);
}

static void test_frame_stats() {
    TEST_BEGIN("frame statistics");
    FrameStats fs(100);
    CHECK(fs.compute().count == 0);
    for (int i = 0; i < 99; ++i) fs.add(16.0);
    fs.add(50.0);
    FrameStats::Result r = fs.compute();
    CHECK(r.count == 100 && std::fabs(r.avg - 16.34) < 1e-9 && r.min == 16.0 && r.max == 50.0 && r.p50 == 16.0);
    CHECK(std::fabs(r.fps - 1000.0 / 16.34) < 1e-6);
    CHECK(std::fabs(r.one_percent_low_fps - 20.0) < 1e-9);   // the single worst frame (50 ms) is the 1 % low
    for (int i = 0; i < 100; ++i) fs.add(10.0);              // the window slides: old frames are gone
    r = fs.compute();
    CHECK(r.count == 100 && r.max == 10.0 && std::fabs(r.fps - 100.0) < 1e-9);
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--crash-child") return child_main(to_wide(argv[2]), argv[3]);
    test_exception_log();
    test_dump_without_exception();
    test_crash_dumps();
    test_etw();
    test_sampling();
    test_frame_stats();
    return finish("diag_tests");
}
