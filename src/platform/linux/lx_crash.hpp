// Crash reporting for Linux (signal handlers on an alternate stack).
//
// On SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT / SIGTRAP the handler writes a text report to
// <crash_dir>/crash-<pid>-<unixtime>.txt using only async-signal-safe calls (open, write, no
// allocation): signal, fault address, registers, a backtrace (glibc) and /proc/self/maps. It then
// gives an optional user hook the open file descriptor (a Breakpad/Crashpad-style minidump writer
// can plug in there) and re-raises the signal with the default action so a core dump or the
// system crash collector (systemd-coredump, apport) still gets the process.
#pragma once
#include <string>

namespace nue::lx {

struct CrashOptions {
    std::string crash_dir;          // must exist; created by the caller (app_dirs().crashes)
    std::string app_name = "nue";
    std::string app_version;
    // Called from the signal handler with the report fd still open. Must be async-signal-safe.
    void (*hook)(int signal, void* siginfo, void* ucontext, int report_fd, void* user) = nullptr;
    void* hook_user = nullptr;
    // Sink protocol (used by the engine's platform layer): `begin` returns the report directory the core
    // created (static storage, NUL terminated) and the stack report is written into it as stack.txt;
    // `end` returns true when the process should terminate right away (_exit) instead of re-raising
    // the signal with its default action. Both run in the signal context.
    const char* (*begin)(int signal, void* siginfo, void* ucontext, void* user) = nullptr;
    bool (*end)(int signal, void* user) = nullptr;
    void* sink_user = nullptr;
    bool write_maps = true;
    bool raise_core_limit = true;   // raise RLIMIT_CORE so the kernel can also write a core file
};

// Installs the handlers (idempotent: a second call replaces the options). Returns false if the
// alternate stack or any handler cannot be installed.
bool install_crash_handler(const CrashOptions& o);
void uninstall_crash_handler();
// The alternate signal stack is per thread: call this at the start of every engine thread so a stack
// overflow there can still be reported (the installing thread is covered automatically).
bool crash_handler_attach_thread();
// Non-fatal report: writes <dir>/stack.txt with the calling thread's backtrace and /proc/self/maps.
bool write_stack_report(const char* dir);
// Path of the report the handler wrote, valid only after a crash was handled (for a hook/atexit).
const char* last_crash_report_path();

// Test aid: raises the given signal in a forked child so the handler can be exercised without killing
// the test process. Returns the report path found in `crash_dir` (empty if none).
std::string crash_handler_selftest(const std::string& crash_dir);

}  // namespace nue::lx
