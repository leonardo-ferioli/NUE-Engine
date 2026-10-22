// Crash report text and fatal-signal names.  The signal handler itself (android_crash.cpp) only gathers raw
// numbers into a CrashInfo and calls format_crash_report; keeping the formatting here makes it testable.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nue::android {

struct CrashFrame {
    uint64_t pc = 0;
    uint64_t module_offset = 0;   // pc - load base of the module
    std::string module;           // path or soname, empty if unknown
    std::string symbol;           // empty if unknown (dladdr)
};

struct CrashInfo {
    int signal = 0;
    int si_code = 0;
    uint64_t fault_address = 0;
    int pid = 0, tid = 0;
    std::string thread_name;
    std::string app_version;
    std::string abi;
    std::string device;
    int sdk_int = 0;
    int64_t unix_time = 0;
    uint64_t rss_bytes = 0;
    std::vector<CrashFrame> frames;
};

// Linux / Android signal numbers (identical on arm64, arm, x86, x86_64).
const char* signal_name(int sig);
const char* signal_code_name(int sig, int si_code);

std::string format_crash_report(const CrashInfo& info);

// File name for a report: "crash-YYYYMMDD-HHMMSS-<pid>.txt" from a unix time (UTC).
std::string crash_file_name(int64_t unix_time, int pid);

} // namespace nue::android
