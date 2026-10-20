// d3d9.dll entry points, configuration, logging, and IDirect3D9 (the factory).
// Capability questions (adapter identity, display modes, format and multisample support, device caps) are answered by the system's own Direct3D 9
// runtime, which is loaded from the Windows folder and only ever asked those questions (it never creates a device): the game then takes the same
// decisions it takes with the real driver. The device, and everything the game draws, is ours.
#include "facade.hpp"
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <mutex>
#include <thread>
#include <typeinfo>

namespace nue::facade {

HMODULE g_module = nullptr;
const char* volatile g_last_call = "(none yet)";
MethodStats g_methods[kMaxMethods];
DWORD g_main_thread = 0;
std::atomic<bool> g_multithreaded{false};
CRITICAL_SECTION g_call_lock;
std::atomic<unsigned> g_counter_present{0}, g_counter_draw{0}, g_counter_create{0};
volatile unsigned g_counter_calls = 0;
std::atomic<unsigned long long> g_frame_call_total_us{0}, g_present_total_us{0}, g_gpu_total_us{0}, g_render_total_us{0}, g_wait_render_total_us{0}, g_wait_submit_total_us{0}, g_thread_cpu_100ns{0}, g_thread_cycles{0};
std::atomic<unsigned long long> g_worker_busy_total_us{0};
std::atomic<unsigned> g_frame_sum_us{0}, g_frame_max_us{0}, g_frame_slow{0}, g_frame_count{0}, g_gpu_us{0}, g_render_cpu_us{0}, g_present_us{0};

namespace {

std::atomic<int> g_devices{0};
FILE* g_log = nullptr;
std::mutex g_log_mutex;
Config g_config;
bool g_config_loaded = false;

std::string module_directory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string p = path;
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::string ini_value(const std::string& ini, const char* key) {
    char buffer[512] = {};
    GetPrivateProfileStringA("graphics", key, "", buffer, sizeof buffer, ini.c_str());
    return buffer;
}

bool truthy(const std::string& v, bool fallback) {
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

void load_config() {
    if (g_config_loaded) return;
    g_config_loaded = true;
    g_config.dll_directory = module_directory();
    const std::string ini = g_config.dll_directory + "\\nue_d3d9.ini";
    // nue_d3d9.ini:  [graphics]  renderer=d3d11|vulkan   vsync=1   render_thread=1   worker_thread=0   log=1   shader_cache=<folder>
    if (auto v = ini_value(ini, "renderer"); !v.empty()) g_config.renderer = v;
    g_config.vsync = truthy(ini_value(ini, "vsync"), g_config.vsync);
    g_config.render_thread = truthy(ini_value(ini, "render_thread"), g_config.render_thread);
    g_config.log = truthy(ini_value(ini, "log"), g_config.log);
    g_config.log_unsupported = truthy(ini_value(ini, "log_unsupported"), g_config.log_unsupported);
    g_config.shader_cache = ini_value(ini, "shader_cache");
    if (auto v = ini_value(ini, "msaa"); !v.empty()) g_config.msaa = v == "off" ? 1u : v == "auto" ? 0u : uint32_t(std::atoi(v.c_str()));
    if (auto v = ini_value(ini, "texture_copy_mb"); !v.empty()) g_config.texture_copy_mb = uint32_t(std::atoi(v.c_str()));
    if (auto v = ini_value(ini, "texture_report_at"); !v.empty()) g_config.texture_report_at = uint32_t(std::atoi(v.c_str()));
    if (const char* e = std::getenv("NUE_TEXTURE_REPORT_AT")) g_config.texture_report_at = uint32_t(std::atoi(e));
    g_config.mark_missing = truthy(ini_value(ini, "mark_missing_textures"), g_config.mark_missing);
    if (auto v = ini_value(ini, "render_thread_priority"); !v.empty()) g_config.render_thread_priority = v;
    if (auto v = ini_value(ini, "render_thread_cpu"); !v.empty()) g_config.render_thread_cpu = std::atoi(v.c_str());
    if (const char* e = std::getenv("NUE_RENDER_THREAD_PRIORITY")) g_config.render_thread_priority = e;
    if (const char* e = std::getenv("NUE_RENDER_THREAD_CPU")) g_config.render_thread_cpu = std::atoi(e);
    if (const char* e = std::getenv("NUE_MARK_MISSING")) g_config.mark_missing = truthy(e, g_config.mark_missing);
    if (const char* e = std::getenv("NUE_RENDERER")) g_config.renderer = e;   // environment wins
    if (const char* e = std::getenv("NUE_VSYNC")) g_config.vsync = truthy(e, g_config.vsync);
    if (const char* e = std::getenv("NUE_RENDER_THREAD")) g_config.render_thread = truthy(e, g_config.render_thread);
    g_config.worker_thread = truthy(ini_value(ini, "worker_thread"), g_config.worker_thread);
    if (auto v = ini_value(ini, "worker_copy_mb"); !v.empty()) g_config.worker_copy_mb = uint32_t(std::atoi(v.c_str()));
    if (const char* e = std::getenv("NUE_WORKER_THREAD")) g_config.worker_thread = truthy(e, g_config.worker_thread);
    if (g_config.shader_cache.empty()) g_config.shader_cache = g_config.dll_directory + "\\nue_shader_cache";
}

void log_line(const char* message) { log("%s", message); }

}  // namespace

const Config& config() { load_config(); return g_config; }

namespace {
std::mutex g_status_mutex;
std::string g_window_status;
}
void set_window_status(const std::string& text) { std::lock_guard<std::mutex> lock(g_status_mutex); g_window_status = text; }
std::string window_status_copy() { std::lock_guard<std::mutex> lock(g_status_mutex); return g_window_status; }

void log(const char* format, ...) {
    load_config();
    if (!g_config.log) return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) {
        g_log = std::fopen((g_config.dll_directory + "\\nue_d3d9.log").c_str(), "a");
        if (!g_log) { g_config.log = false; return; }
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void device_destroyed() { --g_devices; }

void apply_d3d9_fpu_mode(DWORD behaviour_flags) {
#if defined(_M_IX86) || defined(__i386__)
    unsigned short control = 0x007F;   // single precision, round to nearest, everything masked
    if (behaviour_flags & D3DCREATE_FPU_PRESERVE) {   // the caller's precision is kept, but invalid-operation, zero-divide, overflow and underflow are masked anyway
        __asm__ volatile("fnstcw %0" : "=m"(control));
        control |= 0x001D;
    }
    __asm__ volatile("fldcw %0" : : "m"(control));
#else
    (void)behaviour_flags;   // x86-64: SSE math, the x87 control word is not used
#endif
}

// ---- diagnostics: what happened when the game dies or stops ----------------------------------------------------------------------------

unsigned register_method(const char* name) {
    static std::atomic<unsigned> next{0};
    const unsigned slot = next.fetch_add(1);
    if (slot >= kMaxMethods) return kMaxMethods - 1;
    g_methods[slot].name = name;
    return slot;
}

void note_other_thread(const char* method) {
    if (!g_multithreaded.exchange(true)) log("a thread other than the one that created the device calls Direct3D 9 (%s, thread %lu, device thread %lu): all calls are serialised from now on", method, static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(g_main_thread));
}

HRESULT trace_result(const char* method, HRESULT hr) {
    if (!FAILED(hr)) return hr;
    static std::mutex mutex;
    static std::map<std::string, int> seen;
    std::lock_guard<std::mutex> lock(mutex);
    char key[160];
    std::snprintf(key, sizeof key, "%s %08lx", method, static_cast<unsigned long>(hr));
    if (seen[key]++ < 3) log("call failed: %s returned 0x%08lx%s", method, static_cast<unsigned long>(hr), seen[key] == 3 ? " (not reported again)" : "");
    return hr;
}

HRESULT guard_failed(const char* method) {
    std::string what = "unknown";
    try { throw; } catch (const std::exception& e) { what = std::string(typeid(e).name()) + ": " + e.what(); } catch (...) {}
    log("C++ exception inside %s: %s", method, what.c_str());
    return D3DERR_INVALIDCALL;
}

namespace {

LONG CALLBACK exception_logger(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const void* address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module);
    const bool ours = module == g_module;
    const bool fatal = code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
    static std::atomic<int> cxx_logged{0};
    if (code == 0xE06D7363 && cxx_logged++ < 20) log("C++ exception thrown (first chance) while in %s", g_last_call);
    if (ours || fatal) {
        char name[MAX_PATH] = "?";
        if (module) GetModuleFileNameA(module, name, MAX_PATH);
        log("EXCEPTION %08lx at %p (%s+0x%llx) while in %s", static_cast<unsigned long>(code), address, name, (unsigned long long)(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)), g_last_call);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_terminate() {
    std::string what = "unknown";
    try { if (auto e = std::current_exception()) std::rethrow_exception(e); } catch (const std::exception& e) { what = std::string(typeid(e).name()) + ": " + e.what(); } catch (...) {}
    log("FATAL: std::terminate called (exception: %s) while in %s", what.c_str(), g_last_call);
    std::abort();
}

void on_abort(int) { log("FATAL: abort() called while in %s", g_last_call); }

void on_invalid_parameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned line, uintptr_t) {
    log("FATAL: invalid parameter passed to the C runtime (%ls in %ls, %ls:%u) while in %s", expression ? expression : L"?", function ? function : L"?", file ? file : L"?", line, g_last_call);
}

struct ProfileSnapshot { unsigned long long ticks[kMaxMethods] = {}; unsigned calls[kMaxMethods] = {}; unsigned long long tsc = 0, qpc = 0; };

void profile_report(ProfileSnapshot& previous, unsigned frames) {
    ProfileSnapshot now;
    LARGE_INTEGER qpc, frequency;
    QueryPerformanceCounter(&qpc); QueryPerformanceFrequency(&frequency);
    now.tsc = __rdtsc(); now.qpc = static_cast<unsigned long long>(qpc.QuadPart);
    for (unsigned i = 0; i < kMaxMethods; ++i) { now.ticks[i] = g_methods[i].ticks; now.calls[i] = g_methods[i].calls; }
    if (previous.qpc && now.qpc > previous.qpc && frames) {
        const double seconds = double(now.qpc - previous.qpc) / double(frequency.QuadPart);
        const double ms_per_tick = seconds * 1000.0 / double(now.tsc - previous.tsc);
        struct Row { unsigned slot; double ms; unsigned calls; };
        std::vector<Row> rows;
        double total_ms = 0;
        unsigned total_calls = 0;
        for (unsigned i = 0; i < kMaxMethods; ++i) {
            total_calls += now.calls[i] - previous.calls[i];
            const unsigned long long dt = now.ticks[i] - previous.ticks[i];
            if (!dt) continue;
            rows.push_back({i, double(dt) * ms_per_tick, now.calls[i] - previous.calls[i]});
            total_ms += rows.back().ms;
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
        std::string top;
        for (size_t k = 0; k < rows.size() && k < 6; ++k) {
            char item[160];
            std::snprintf(item, sizeof item, "%s%s %.1f ms/frame (%u calls/frame, %.2f us each)", k ? "; " : "", g_methods[rows[k].slot].name, rows[k].ms / frames, rows[k].calls / frames, rows[k].ms * 1000.0 / std::max(rows[k].calls, 1u));
            top += item;
        }
        log("profile: inside the Direct3D 9 dll %.2f ms per frame (%.0f%% of wall time), %u calls per frame; most expensive: %s", total_ms / frames, 100.0 * total_ms / (seconds * 1000.0), total_calls / frames, top.c_str());
    }
    previous = now;
}


// The frame as the game thread lives it: where the wall time of a frame goes, and whether the game thread is CPU bound (busy ~100 %) or waiting.
struct TimelineSnapshot {
    unsigned long long qpc = 0, tsc = 0, thread_cpu = 0, thread_cycles = 0, process_cpu = 0, present_us = 0, gpu_us = 0, render_us = 0, wait_render_us = 0, wait_submit_us = 0, dll_ticks = 0, frame_call_us = 0, worker_us = 0;
};

unsigned long long filetime_value(const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// How busy the GPU really is: the operating system's own "GPU Engine" performance counters for this process (what the Task Manager shows). bgfx's timestamp
// queries span from the start to the end of a frame on the GPU's timeline, so they include the time the GPU sits idle or slowed between commands; these counters are the share of
// the wall time an engine of the GPU ran work of this process. Returns -1 when the counters are not available.
double gpu_busy_percent() {
    struct Pdh {
        HMODULE module = nullptr;
        PDH_HQUERY query = nullptr;
        PDH_HCOUNTER counter = nullptr;
        bool ok = false;
        decltype(&PdhCollectQueryData) collect = nullptr;
        decltype(&PdhGetFormattedCounterArrayW) get_array = nullptr;
        Pdh() {
            module = LoadLibraryA("pdh.dll");
            if (!module) return;
            auto open = reinterpret_cast<decltype(&PdhOpenQueryW)>(GetProcAddress(module, "PdhOpenQueryW"));
            auto add = reinterpret_cast<decltype(&PdhAddEnglishCounterW)>(GetProcAddress(module, "PdhAddEnglishCounterW"));
            collect = reinterpret_cast<decltype(&PdhCollectQueryData)>(GetProcAddress(module, "PdhCollectQueryData"));
            get_array = reinterpret_cast<decltype(&PdhGetFormattedCounterArrayW)>(GetProcAddress(module, "PdhGetFormattedCounterArrayW"));
            if (!open || !add || !collect || !get_array) return;
            if (open(nullptr, 0, &query) != ERROR_SUCCESS) return;
            wchar_t path[128];
            std::swprintf(path, 128, L"\\GPU Engine(*)\\Utilization Percentage");
            if (add(query, path, 0, &counter) != ERROR_SUCCESS) return;
            collect(query);
            ok = true;
        }
    };
    static Pdh pdh;
    if (!pdh.ok) return -1;
    if (pdh.collect(pdh.query) != ERROR_SUCCESS) return -1;
    DWORD bytes = 0, items = 0;
    PDH_STATUS status = pdh.get_array(pdh.counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &items, nullptr);
    if (status != PDH_MORE_DATA || !bytes) return -1;
    std::vector<unsigned char> buffer(bytes);
    auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (pdh.get_array(pdh.counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &items, values) != ERROR_SUCCESS) return -1;
    double busiest = 0;
    wchar_t mine[32];
    std::swprintf(mine, 32, L"pid_%lu_", static_cast<unsigned long>(GetCurrentProcessId()));
    for (DWORD i = 0; i < items; ++i) if (values[i].szName && std::wcsstr(values[i].szName, mine) && std::wcsstr(values[i].szName, L"engtype_3D")) busiest = std::max(busiest, values[i].FmtValue.doubleValue);   // this process's 3D engines: the busiest one
    static int debug_left = std::getenv("NUE_PDH_DEBUG") ? 3 : 0;
    if (debug_left > 0) { --debug_left; int mine_count = 0; for (DWORD i = 0; i < items; ++i) if (values[i].szName && std::wcsstr(values[i].szName, mine)) ++mine_count; log("pdh: %lu GPU engine instances, %d of this process, busiest 3D %.1f%%", static_cast<unsigned long>(items), mine_count, busiest); for (DWORD i = 0; i < items; ++i) if (values[i].szName && std::wcsstr(values[i].szName, mine)) log("pdh:   %ls = %.2f (status %lx)", values[i].szName, values[i].FmtValue.doubleValue, static_cast<unsigned long>(values[i].FmtValue.CStatus)); }
    return busiest;
}

// How fast a core runs right now, measured: a short chain of dependent integer operations timed three times (the fastest counts, so a busy core does not skew it). The operating system
// does not report the real clock of this machine (it says the rated 2000 MHz all the time), but the speed of a core under a load that shares the chip with the GPU does change:
// the first measurement (in the menu) is 100 %, a later 80 % means the cores run 20 % slower than then.
double core_speed_probe() {
    static double best_rate = 0;
    double rate = 0;
    LARGE_INTEGER frequency, begin, end;
    QueryPerformanceFrequency(&frequency);
    for (int round = 0; round < 3; ++round) {
        volatile unsigned x = 12345;
        unsigned v = x;
        QueryPerformanceCounter(&begin);
        for (unsigned i = 0; i < 400000; ++i) v = v * 1664525u + 1013904223u;
        QueryPerformanceCounter(&end);
        x = v;
        const double seconds = double(end.QuadPart - begin.QuadPart) / double(frequency.QuadPart);
        if (seconds > 0) rate = std::max(rate, 400000.0 / seconds);
    }
    best_rate = std::max(best_rate, rate);
    return best_rate > 0 ? 100.0 * rate / best_rate : 0;
}

void timeline_report(TimelineSnapshot& previous, unsigned frames) {
    TimelineSnapshot now;
    LARGE_INTEGER qpc, frequency;
    QueryPerformanceCounter(&qpc); QueryPerformanceFrequency(&frequency);
    now.qpc = static_cast<unsigned long long>(qpc.QuadPart); now.tsc = __rdtsc();
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) now.process_cpu = filetime_value(k) + filetime_value(u);
    now.thread_cpu = g_thread_cpu_100ns.load(); now.thread_cycles = g_thread_cycles.load();
    now.frame_call_us = g_frame_call_total_us.load();
    now.worker_us = g_worker_busy_total_us.load();
    now.present_us = g_present_total_us.load(); now.gpu_us = g_gpu_total_us.load(); now.render_us = g_render_total_us.load(); now.wait_render_us = g_wait_render_total_us.load(); now.wait_submit_us = g_wait_submit_total_us.load();
    for (unsigned i = 0; i < kMaxMethods; ++i) now.dll_ticks += g_methods[i].ticks;
    if (previous.qpc && frames && now.qpc > previous.qpc) {
        const double seconds = double(now.qpc - previous.qpc) / double(frequency.QuadPart);
        const double ms_per_tick = seconds * 1000.0 / double(now.tsc - previous.tsc);
        const double f = double(frames);
        const double wall = seconds * 1000.0 / f;
        const double in_dll = double(now.dll_ticks - previous.dll_ticks) * ms_per_tick / f;
        const double waiting = double(now.present_us - previous.present_us) / 1000.0 / f;   // time inside Present: the game thread waits there for the render thread
        const double in_frame_call = double(now.frame_call_us - previous.frame_call_us) / 1000.0 / f;
        const double thread_s = double(now.thread_cpu - previous.thread_cpu) * 1e-7;
        const double busy = 100.0 * thread_s / seconds;
        const double process_cores = double(now.process_cpu - previous.process_cpu) * 1e-7 / seconds;
        const double render = double(now.render_us - previous.render_us) / 1000.0 / f;
        const double idle = double(now.wait_submit_us - previous.wait_submit_us) / 1000.0 / f;
        const double gpu = double(now.gpu_us - previous.gpu_us) / 1000.0 / f;
        const double core_speed = core_speed_probe();
        const double gpu_busy = gpu_busy_percent();
        const double worker = double(now.worker_us - previous.worker_us) / 1000.0 / f;
        char worker_text[160] = "";
        if (worker > 0) std::snprintf(worker_text, sizeof worker_text, "; worker thread (translator + bgfx calls) %.2f ms busy per frame, the game thread waits for it in Present", worker);
        char gpu_text[64];
        if (gpu_busy >= 0) std::snprintf(gpu_text, sizeof gpu_text, "%.0f%% of the time (%.2f ms per frame)", gpu_busy, gpu_busy * 0.01 * wall); else std::snprintf(gpu_text, sizeof gpu_text, "(counters not available)");
        log("timeline: %u frames in %.1f s, per frame: wall %.2f ms = game code outside the dll %.2f + inside the dll %.2f (of which in Present %.2f, bgfx::frame() %.2f: sorting the frame and waiting for the render thread if it is behind); game thread CPU busy %.0f%% (%s); "
            "render thread %.2f ms busy, %.2f ms waiting for the game thread; GPU frame span %.2f ms (first to last timestamp of a frame, idle gaps included), GPU 3D engine really busy %s; whole process %.2f cores busy; core speed %.0f%% of the best measured since the start%s",
            frames, seconds, wall, wall - in_dll, in_dll, waiting, in_frame_call, busy, busy > 85 ? "CPU bound: the game thread never waits" : busy > 60 ? "mostly CPU bound" : "the game thread waits for something", render, idle, gpu, gpu_text, process_cores, core_speed, worker_text);
    }
    previous = now;
}

void heartbeat() {
    unsigned last = 0;
    int quiet = 0;
    ProfileSnapshot profile_previous;
    TimelineSnapshot timeline_previous;
    for (;;) {
        Sleep(3000);
        const unsigned now = g_counter_calls;
        if (now != last) {
            const unsigned frames = g_frame_count.exchange(0), sum = g_frame_sum_us.exchange(0), worst = g_frame_max_us.exchange(0), slow = g_frame_slow.exchange(0);
            if (frames) profile_report(profile_previous, frames);
            if (frames) timeline_report(timeline_previous, frames);
            if (frames) log("frames: %.1f fps, mean %.2f ms, worst %.2f ms, %u of %u over 20 ms; last frame: GPU %.2f ms, backend CPU %.2f ms, Present %.2f ms", frames / 3.0, sum / 1000.0 / frames, worst / 1000.0, slow, frames, g_gpu_us.load() / 1000.0, g_render_cpu_us.load() / 1000.0, g_present_us.load() / 1000.0);
            log("alive: %u calls, %u presents, %u draws, %u resources created, in %s", now, g_counter_present.load(), g_counter_draw.load(), g_counter_create.load(), g_last_call);
            quiet = 0;
        } else if (++quiet == 1 || quiet % 10 == 0) log("no Direct3D 9 call for %d s (last: %s)", quiet * 3, g_last_call);
        last = now;
    }
}

void install_diagnostics() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::set_terminate(on_terminate);
        std::signal(SIGABRT, on_abort);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_invalid_parameter_handler(on_invalid_parameter);
        AddVectoredExceptionHandler(1, exception_logger);
        std::thread(heartbeat).detach();
    });
}

}  // namespace


// ---- threads --------------------------------------------------------------------------------------------------------------------------------

namespace {
std::vector<DWORD> process_threads() {
    std::vector<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return ids;
    THREADENTRY32 entry{sizeof entry};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) if (entry.th32OwnerProcessID == GetCurrentProcessId()) ids.push_back(entry.th32ThreadID);
    CloseHandle(snapshot);
    return ids;
}

int priority_from_name(const std::string& name, bool& known) {
    known = true;
    if (name == "idle") return THREAD_PRIORITY_IDLE;
    if (name == "lowest") return THREAD_PRIORITY_LOWEST;
    if (name == "below") return THREAD_PRIORITY_BELOW_NORMAL;
    if (name == "normal") return THREAD_PRIORITY_NORMAL;
    if (name == "above") return THREAD_PRIORITY_ABOVE_NORMAL;
    if (name == "highest") return THREAD_PRIORITY_HIGHEST;
    known = false;
    return THREAD_PRIORITY_NORMAL;
}

// The threads bgfx started (its render thread) are told apart from the game's by comparing the thread list before and after bgfx::init. They are logged with their priority,
// and optionally given a priority / ideal processor from nue_d3d9.ini (render_thread_priority, render_thread_cpu).
void tune_render_threads(const std::vector<DWORD>& before, const Config& cfg) {
    const std::vector<DWORD> after = process_threads();
    bool known = false;
    const int wanted = priority_from_name(cfg.render_thread_priority, known);
    SYSTEM_INFO info; GetSystemInfo(&info);
    log("threads: game thread %lu priority %d, process priority class 0x%lx, %lu logical processors", static_cast<unsigned long>(GetCurrentThreadId()), GetThreadPriority(GetCurrentThread()), static_cast<unsigned long>(GetPriorityClass(GetCurrentProcess())),
        static_cast<unsigned long>(info.dwNumberOfProcessors));
    for (DWORD id : after) {
        if (std::find(before.begin(), before.end(), id) != before.end()) continue;
        HANDLE thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!thread) continue;
        if (known) SetThreadPriority(thread, wanted);
        if (cfg.render_thread_cpu >= 0) SetThreadIdealProcessor(thread, DWORD(cfg.render_thread_cpu));
        log("threads: started while bgfx started (its render thread among them, and the driver's): thread %lu, priority %d%s%s", static_cast<unsigned long>(id), GetThreadPriority(thread), known ? (" (set from " + cfg.render_thread_priority + ")").c_str() : "", cfg.render_thread_cpu >= 0 ? " (ideal processor set)" : "");
        CloseHandle(thread);
    }
}
}  // namespace

// ---- IDirect3D9 -------------------------------------------------------------------------------------------------------------------------

Direct3D9::Direct3D9() {
    install_diagnostics();
    static std::once_flag lock_once;
    std::call_once(lock_once, [] { InitializeCriticalSection(&g_call_lock); });
    char system[MAX_PATH] = {};
    GetSystemDirectoryA(system, MAX_PATH);   // in a 32-bit process on 64-bit Windows this is SysWOW64
    const std::string path = std::string(system) + "\\d3d9.dll";
    if (HMODULE real = LoadLibraryA(path.c_str())) {
        using Create = IDirect3D9*(WINAPI*)(UINT);
        if (auto create = reinterpret_cast<Create>(GetProcAddress(real, "Direct3DCreate9"))) real_ = create(D3D_SDK_VERSION);
    }
    log("Direct3DCreate9: system runtime %s", real_ ? "available for capability queries" : "NOT available (using built-in defaults)");
}

Direct3D9::~Direct3D9() { if (real_) real_->Release(); }

UINT Direct3D9::GetAdapterCount() { return real_ ? real_->GetAdapterCount() : 1; }

HRESULT Direct3D9::GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) {
    if (real_) return real_->GetAdapterIdentifier(adapter, flags, id);
    if (!id || adapter != 0) return D3DERR_INVALIDCALL;
    std::memset(id, 0, sizeof *id);
    std::strcpy(id->Driver, "nue_d3d9"); std::strcpy(id->Description, "nue_d3d9 (Direct3D 9 on bgfx)"); id->VendorId = 0x10DE; id->DeviceId = 0x0001;
    return D3D_OK;
}

UINT Direct3D9::GetAdapterModeCount(UINT adapter, D3DFORMAT format) { return real_ ? real_->GetAdapterModeCount(adapter, format) : 0; }
HRESULT Direct3D9::EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) { return real_ ? real_->EnumAdapterModes(adapter, format, mode, out) : D3DERR_INVALIDCALL; }

HRESULT Direct3D9::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) {
    if (real_) return real_->GetAdapterDisplayMode(adapter, mode);
    if (!mode) return D3DERR_INVALIDCALL;
    mode->Width = UINT(GetSystemMetrics(SM_CXSCREEN)); mode->Height = UINT(GetSystemMetrics(SM_CYSCREEN)); mode->RefreshRate = 60; mode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HRESULT Direct3D9::CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back, BOOL windowed) { return real_ ? real_->CheckDeviceType(adapter, type, display, back, windowed) : D3D_OK; }
HRESULT Direct3D9::CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT check) { return real_ ? real_->CheckDeviceFormat(adapter, type, adapter_format, usage, resource, check) : D3D_OK; }
HRESULT Direct3D9::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE multisample, DWORD* quality) {
    if (real_) return real_->CheckDeviceMultiSampleType(adapter, type, format, windowed, multisample, quality);
    if (quality) *quality = 1;
    return multisample == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
}
HRESULT Direct3D9::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, D3DFORMAT render_target, D3DFORMAT depth) { return real_ ? real_->CheckDepthStencilMatch(adapter, type, adapter_format, render_target, depth) : D3D_OK; }
HRESULT Direct3D9::CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT source, D3DFORMAT target) { return real_ ? real_->CheckDeviceFormatConversion(adapter, type, source, target) : D3D_OK; }

HRESULT Direct3D9::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) {
    if (!caps) return D3DERR_INVALIDCALL;
    if (real_) return real_->GetDeviceCaps(adapter, type, caps);
    return D3DERR_NOTAVAILABLE;
}

HMONITOR Direct3D9::GetAdapterMonitor(UINT adapter) { return real_ ? real_->GetAdapterMonitor(adapter) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY); }

HRESULT Direct3D9::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (!pp || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    g_main_thread = GetCurrentThreadId();
    HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    if (!window || adapter != 0) return D3DERR_INVALIDCALL;
    if (g_devices.load() > 0) { log("CreateDevice refused: a device already exists (bgfx is a singleton)"); return D3DERR_NOTAVAILABLE; }

    RECT client{};
    GetClientRect(window, &client);
    const UINT window_w = UINT(std::max<LONG>(client.right - client.left, 1)), window_h = UINT(std::max<LONG>(client.bottom - client.top, 1));
    const Config& cfg = config();

    gpu::BgfxOptions options;
    options.window = window;
    options.window_width = window_w; options.window_height = window_h;
    options.renderer = cfg.renderer == "vulkan" ? gpu::Renderer::vulkan : gpu::Renderer::d3d11;
    options.vsync = cfg.vsync && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
    options.single_threaded = !cfg.render_thread;
    options.shader_cache_dir = cfg.shader_cache;
    options.log = log_line;
    d3d9::LayerOptions layer_options;
    layer_options.log = log_line;
    layer_options.log_unsupported = cfg.log_unsupported;
    layer_options.msaa_override = cfg.msaa;
    layer_options.texture_copy_mb = cfg.texture_copy_mb;
    layer_options.mark_missing_textures = cfg.mark_missing;
    auto core = std::make_shared<Core>();
    std::string error;
    // bgfx is used from the thread that started it: in worker mode the adapter and the layer are made (and later destroyed) on the worker
    if (cfg.worker_thread) core->threaded = std::make_unique<ThreadedLayer>(uint64_t(cfg.worker_copy_mb) << 20);
    const char* renderer_name = "";
    core->backend([&] {
        const std::vector<DWORD> threads_before = process_threads();
        core->gpu = gpu::create_bgfx_gpu(options, error);
        if (!core->gpu) return;
        tune_render_threads(threads_before, cfg);
        core->layer = d3d9::create_layer(*core->gpu, layer_options);
        renderer_name = core->gpu->stats().renderer_name;
    });
    if (!core->gpu) { log("CreateDevice failed: bgfx could not start (%s)", error.c_str()); return D3DERR_NOTAVAILABLE; }
    if (core->threaded) core->threaded->attach(core->layer.get(), core->gpu.get());

    // fill in what a game leaves to the runtime and make the layer's back buffer
    D3DPRESENT_PARAMETERS filled = *pp;
    if (!filled.BackBufferWidth) filled.BackBufferWidth = window_w;
    if (!filled.BackBufferHeight) filled.BackBufferHeight = window_h;
    if (filled.BackBufferFormat == D3DFMT_UNKNOWN) filled.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!filled.BackBufferCount) filled.BackBufferCount = 1;
    d3d9::DeviceParams params;
    params.back_width = filled.BackBufferWidth; params.back_height = filled.BackBufferHeight; params.back_format = uint32_t(filled.BackBufferFormat);
    params.depth_format = filled.EnableAutoDepthStencil ? uint32_t(filled.AutoDepthStencilFormat) : 0;
    params.multisample = uint32_t(filled.MultiSampleType); params.multisample_quality = filled.MultiSampleQuality; params.buffers = filled.BackBufferCount; params.windowed = filled.Windowed != FALSE;
    params.present_interval = filled.PresentationInterval;
    if (core->front()->reset(params) != Status::ok) {
        log("CreateDevice failed: the layer refused the back buffer parameters");
        return D3DERR_NOTAVAILABLE;   // ~Core releases them
    }

    D3DCAPS9 caps{};
    GetDeviceCaps(adapter, type, &caps);
    apply_d3d9_fpu_mode(behaviour);
    D3DDEVICE_CREATION_PARAMETERS creation{adapter, type, focus, behaviour};
    *pp = filled;
    auto* device = new Device9(this, core, creation, filled, caps, window);
    ++g_devices;
    *out = device;
    log("CreateDevice: behaviour flags 0x%lx, %ux%u, back buffer format %d, depth %d, %s, interval %u, window %ux%u, renderer %s%s%s", static_cast<unsigned long>(behaviour), filled.BackBufferWidth, filled.BackBufferHeight, int(filled.BackBufferFormat),
        filled.EnableAutoDepthStencil ? int(filled.AutoDepthStencilFormat) : 0, filled.Windowed ? "windowed" : "fullscreen", unsigned(filled.PresentationInterval), window_w, window_h,
        renderer_name, options.single_threaded ? "" : " (render thread)", core->threaded ? " (worker thread)" : "");
    return D3D_OK;
}

}  // namespace nue::facade

// ---- exports ----------------------------------------------------------------------------------------------------------------------------

#ifdef _WIN64
#define NUE_EXPORT(name, decorated) __pragma(comment(linker, "/EXPORT:" #name "=" #name))
#else
#define NUE_EXPORT(name, decorated) __pragma(comment(linker, "/EXPORT:" #name "=" decorated))
#endif

NUE_EXPORT(Direct3DCreate9, "_Direct3DCreate9@4")
NUE_EXPORT(Direct3DCreate9Ex, "_Direct3DCreate9Ex@8")
NUE_EXPORT(D3DPERF_BeginEvent, "_D3DPERF_BeginEvent@8")
NUE_EXPORT(D3DPERF_EndEvent, "_D3DPERF_EndEvent@0")
NUE_EXPORT(D3DPERF_SetMarker, "_D3DPERF_SetMarker@8")
NUE_EXPORT(D3DPERF_SetRegion, "_D3DPERF_SetRegion@8")
NUE_EXPORT(D3DPERF_QueryRepeatFrame, "_D3DPERF_QueryRepeatFrame@0")
NUE_EXPORT(D3DPERF_SetOptions, "_D3DPERF_SetOptions@4")
NUE_EXPORT(D3DPERF_GetStatus, "_D3DPERF_GetStatus@0")

extern "C" {
// Diagnostics for the window tests: how the last window size was taken (client area, visible part, swap chain size of bgfx). Returns the length written.
__declspec(dllexport) int NueD3D9_DescribeWindow(char* buffer, int size) {
    const std::string text = nue::facade::window_status_copy();
    if (buffer && size > 0) { std::snprintf(buffer, size_t(size), "%s", text.c_str()); }
    return int(text.size());
}
IDirect3D9* WINAPI Direct3DCreate9(UINT) { return new nue::facade::Direct3D9(); }
HRESULT WINAPI Direct3DCreate9Ex(UINT, IDirect3D9Ex**) { return D3DERR_NOTAVAILABLE; }
int WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
int WINAPI D3DPERF_EndEvent() { return 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
void WINAPI D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
BOOL WINAPI D3DPERF_QueryRepeatFrame() { return FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD) {}
DWORD WINAPI D3DPERF_GetStatus() { return 0; }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { nue::facade::g_module = module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
