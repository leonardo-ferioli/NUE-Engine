#include "crash_report.hpp"

#include <cstdio>

namespace nue::android {

const char* signal_name(int sig) {
    switch (sig) {
    case 4: return "SIGILL";
    case 5: return "SIGTRAP";
    case 6: return "SIGABRT";
    case 7: return "SIGBUS";
    case 8: return "SIGFPE";
    case 11: return "SIGSEGV";
    case 31: return "SIGSYS";
    default: return "SIG?";
    }
}

const char* signal_code_name(int sig, int code) {
    if (sig == 11) {
        if (code == 1) return "SEGV_MAPERR";
        if (code == 2) return "SEGV_ACCERR";
        if (code == 8) return "SEGV_MTESTERR";
    } else if (sig == 7) {
        if (code == 1) return "BUS_ADRALN";
        if (code == 2) return "BUS_ADRERR";
    } else if (sig == 8) {
        if (code == 1) return "FPE_INTDIV";
        if (code == 3) return "FPE_FLTDIV";
    } else if (sig == 4) {
        if (code == 1) return "ILL_ILLOPC";
    }
    if (code == 0) return "SI_USER";
    if (code == -6) return "SI_TKILL";
    return "";
}

std::string crash_file_name(int64_t t, int pid) {
    // civil-from-days (Howard Hinnant), no <ctime> so the function is thread safe and signal friendly enough
    int64_t days = t / 86400;
    int64_t secs = t % 86400;
    if (secs < 0) { secs += 86400; --days; }
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[80];
    std::snprintf(buf, sizeof buf, "crash-%04d%02d%02d-%02d%02d%02d-%d.txt", int(y), int(m), int(d), int(secs / 3600),
                  int(secs % 3600 / 60), int(secs % 60), pid);
    return buf;
}

std::string format_crash_report(const CrashInfo& c) {
    std::string out;
    char line[512];
    auto add = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
    };
    out += "NUE crash report\n";
    add("version: %s\n", c.app_version.c_str());
    add("device: %s  sdk: %d  abi: %s\n", c.device.c_str(), c.sdk_int, c.abi.c_str());
    add("time: %lld (unix)  pid: %d  tid: %d (%s)\n", static_cast<long long>(c.unix_time), c.pid, c.tid,
        c.thread_name.c_str());
    add("signal: %d (%s)  code: %d (%s)  fault addr: 0x%llx\n", c.signal, signal_name(c.signal), c.si_code,
        signal_code_name(c.signal, c.si_code), static_cast<unsigned long long>(c.fault_address));
    add("rss: %llu KiB\n", static_cast<unsigned long long>(c.rss_bytes / 1024));
    out += "backtrace:\n";
    int n = 0;
    for (const CrashFrame& f : c.frames) {
        add("  #%02d pc %016llx  %s", n++, static_cast<unsigned long long>(f.module_offset),
            f.module.empty() ? "<unknown>" : f.module.c_str());
        if (!f.symbol.empty()) {
            out += " (";
            out += f.symbol;
            out += ")";
        }
        out += "\n";
    }
    return out;
}

} // namespace nue::android
