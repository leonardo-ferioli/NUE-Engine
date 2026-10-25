// Diagnostics: minidump on crash, first-chance exception log, ETW provider, CPU / memory / GPU engine / thermal sampling, frame statistics.
//
// The recompiled game has its own exception machinery (guest SEH, a vectored handler that reports the guest PC). This module is designed to
// sit beside it: the exception log is passive (it never handles anything), the crash filter only takes the final unhandled case, and
// write_minidump() can be called from the game's own fatal-error path with the EXCEPTION_POINTERS it already has.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

// ---- Crash dumps ---------------------------------------------------------------------------------------------------------------------------------
struct CrashOptions {
    std::wstring dump_dir;                 // created if missing (long-path aware)
    std::wstring app_name = L"nue";        // file name prefix: <app>-<yyyymmdd-hhmmss>-<pid>.dmp / .txt
    std::string build_id;                  // free text written to the report (version, commit)
    bool full_memory = false;              // false: small dump with data segments, indirect memory, handle/thread info (a few MB). true: all memory (hundreds of MB)
    bool write_text_report = true;
    bool restore_desktop = true;           // pointer clip / hidden pointer / display mode are undone first (emergency_restore_desktop)
    bool chain_previous_filter = true;     // call the filter that was installed before us after writing the dump
    bool skip_when_debugger_attached = true;
    uint32_t max_dumps_kept = 5;           // oldest dumps of this app are deleted before writing a new one
    std::function<void(const std::wstring& dump_path, const std::wstring& report_path)> on_crash;   // after the files exist; runs on the crashing process, keep it trivial
};
// Variant used by the NUE core: the report directory is chosen by the caller instead of by this module. `begin` receives the facts of the crash and
// returns a native UTF-8 directory path (static storage) or null; the dump goes to <dir>\crash.dmp; `end` decides whether the process terminates
// (true) or the default handling continues (false). Both run on the crash thread, without this module allocating anything on the way.
struct CrashInfo {
    char reason[160];             // "access violation reading 0x00000008"
    uint64_t fault_address = 0;   // address touched for access violations, else the instruction pointer
    uint64_t fault_pc = 0;
    uint32_t code = 0;            // NTSTATUS
    const char* report_dir = nullptr;
};
struct CrashHook {
    const char* (*begin)(const CrashInfo& info, void* user) = nullptr;
    bool (*end)(const CrashInfo& info, void* user) = nullptr;
    void* user = nullptr;
    bool full_memory = false;
    bool write_dump = true;
    bool release_input = true;    // emergency_restore_desktop() first (pointer clip, hidden pointer, display modes)
    bool skip_when_debugger_attached = true;
};
Status install_crash_hook(const CrashHook& hook);

// Installs SetUnhandledExceptionFilter. The dump is written by a fresh thread so that stack overflows can be reported too.
Status install_crash_handler(const CrashOptions& options);
void uninstall_crash_handler();
bool crash_handler_installed();
// Makes sure the CALLING thread keeps `bytes` of stack available while a stack overflow is being handled (SetThreadStackGuarantee). Without it the crash filter may find
// only a few KiB left, fault again (as an access violation, not a stack overflow) and the process dies without a report; seen on a GitHub Windows runner. The
// install functions do it for the installing thread; call it from every other thread that runs deep or untrusted code (game threads, workers). Returns the
// guarantee in force (0 on failure).
uint32_t reserve_crash_stack(uint32_t bytes = 128 * 1024);
// Writes a dump for `ex` (may be null: dump without exception info, e.g. for a watchdog "hang" report). Callable from any fatal path.
Status write_minidump(EXCEPTION_POINTERS* ex, const std::wstring& path, bool full_memory = false);
std::wstring last_crash_dump_path();

struct DumpSummary {
    bool valid = false;
    uint32_t exception_code = 0;
    uint64_t exception_address = 0;
    uint32_t crashing_thread_id = 0;
    uint32_t module_count = 0, thread_count = 0;
    uint32_t processor_architecture = 0;
    bool has_system_info = false;
    bool has_exception = false;
    bool has_handle_data = false, has_thread_info = false;
};
// Reads the streams of a dump back (dbghelp), used by the tests and by tooling that wants a one-line crash summary.
bool read_minidump_summary(const std::wstring& path, DumpSummary& out);
const char* exception_code_name(uint32_t code);

// ---- First-chance exception log ---------------------------------------------------------------------------------------------------------------
struct ExceptionLogEntry {
    uint32_t code = 0, flags = 0, thread_id = 0;
    uint64_t address = 0;
    uint64_t info0 = 0, info1 = 0;   // access violation: 0 read / 1 write / 8 DEP, and the address touched
    int64_t qpc = 0;
};
// A vectored handler that records every first-chance exception (except C++ exceptions, thread-name and debug-print pseudo exceptions unless asked)
// in a lock-free ring and always returns EXCEPTION_CONTINUE_SEARCH. Registered last so the game's own handlers see exceptions first.
Status install_exception_log(size_t capacity = 256, bool include_cxx = false);
void remove_exception_log();
std::vector<ExceptionLogEntry> exception_log_snapshot();   // oldest first
uint64_t exception_log_total();

// ---- ETW -----------------------------------------------------------------------------------------------------------------------------------------
// TraceLogging provider "NUE.Engine" {c8e1a4b2-3d57-4f6a-9b0e-71d2f5a8c3e4}. Zero cost while no session listens.
// Capture:  logman create trace nue -p {c8e1a4b2-3d57-4f6a-9b0e-71d2f5a8c3e4} -o nue.etl -ets   ...   logman stop nue -ets   (open the .etl in WPA / PerfView).
Status etw_register();
void etw_unregister();
bool etw_enabled();
void etw_event(const char* name, const char* detail = "", int64_t value = 0);
void etw_frame(uint64_t frame_index, double cpu_ms, double frame_ms);
void etw_mark(const char* label);   // instant marker, shows as a vertical line in WPA

// ---- Sampling --------------------------------------------------------------------------------------------------------------------------------------
struct ProcessSample {
    double wall_ms = 0;
    double process_cpu_ms = 0;          // user + kernel, all threads
    double thread_cpu_ms = 0;           // the calling thread
    uint64_t thread_cycles = 0;         // QueryThreadCycleTime (frequency independent, unlike time on hybrid/boosting CPUs)
};
ProcessSample sample_process();
// CPU use of the process between two samples as a fraction of ONE logical processor (1.0 = one core fully busy) and of the whole machine.
struct CpuUsage { double cores = 0, machine_fraction = 0, thread_cores = 0; };
CpuUsage cpu_usage_between(const ProcessSample& a, const ProcessSample& b, uint32_t logical_processors);

struct MemorySample {
    uint64_t working_set = 0, peak_working_set = 0, private_bytes = 0, pagefile_usage = 0;
    uint32_t handle_count = 0, gdi_objects = 0, user_objects = 0, threads = 0;
    bool valid = false;
};
MemorySample sample_memory();

// GPU engine utilisation of this process from the "GPU Engine" performance counters (PDH; Windows 10 1709+, no admin). Expensive (a few ms):
// call about once per second, never per frame.
class GpuEngineSampler {
public:
    GpuEngineSampler();
    ~GpuEngineSampler();
    GpuEngineSampler(const GpuEngineSampler&) = delete;
    GpuEngineSampler& operator=(const GpuEngineSampler&) = delete;
    struct Sample { bool valid = false; double engine_3d_percent = 0, engine_copy_percent = 0, engine_compute_percent = 0, engine_video_percent = 0, any_engine_percent = 0; uint64_t dedicated_bytes = 0, shared_bytes = 0; uint32_t instances = 0; };
    Status open();
    Sample sample();
    bool available() const;
private:
    struct Impl;
    Impl* impl_;
};

struct ThermalZone { std::string name; double celsius = 0; double passive_limit_percent = 0; };
// "Thermal Zone Information" counters (ACPI thermal zones; absent on many desktops and virtual machines). Empty vector = not available.
std::vector<ThermalZone> read_thermal_zones();

// Frame time statistics over a sliding window, for the frame pacing display and logs.
class FrameStats {
public:
    explicit FrameStats(size_t window = 600) : window_(window ? window : 1) { samples_.reserve(window_); }
    void add(double frame_ms);
    struct Result { size_t count = 0; double avg = 0, min = 0, max = 0, p50 = 0, p95 = 0, p99 = 0, fps = 0, one_percent_low_fps = 0; };
    Result compute() const;
    void clear() { samples_.clear(); next_ = 0; }
private:
    size_t window_;
    std::vector<double> samples_;
    size_t next_ = 0;
};

}  // namespace nue::win
