// Fatal-signal crash handler.
//
// Two stages so the part that runs inside the signal handler stays async-signal-safe:
//   1. handler: writes a raw binary record (signal, fault address, frame PCs from _Unwind_Backtrace, the executable
//      lines of /proc/self/maps) to <crash_dir>/last_crash.bin using only open/read/write, then restores the previous
//      handler (debuggerd: tombstone + the system crash dialog) and returns / re-raises so Android still sees the crash;
//   2. next start: crash_convert_pending() turns the record into <crash_dir>/crash-<utc time>-<pid>.txt with
//      format_crash_report() (module + offset per frame, symbolicate with ndk-stack / addr2line offline) and logs it.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "android_state.hpp"
#include "crash_report.hpp"

namespace fs = std::filesystem;
using namespace nue::android;

namespace {

constexpr int kMaxFrames = 48;
constexpr size_t kMapsCap = 96 * 1024;
constexpr const char kMagic[8] = {'N', 'U', 'E', 'C', 'R', 'S', 'H', '1'};

struct RawHeader {
    char magic[8];
    int32_t sig, code;
    uint64_t fault_addr;
    int32_t pid, tid;
    int64_t unix_time;
    uint64_t rss_pages;
    int32_t nframes;
    int32_t maps_len;
    char thread_name[16];
    char version[32];
    char device[64];
    char abi[16];
    int32_t sdk;
    uint64_t frames[kMaxFrames];
};

RawHeader g_rec;
char g_maps[kMapsCap];
char g_path[512];
char g_version[32];
char g_device[64];
char g_abi[16];
int g_sdk = 0;
bool g_installed = false;
struct sigaction g_old[32];
const int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, SIGSYS};
alignas(16) char g_altstack[64 * 1024];
volatile sig_atomic_t g_entered = 0;

struct UnwindState {
    int n;
};

_Unwind_Reason_Code unwind_cb(struct _Unwind_Context* ctx, void* arg) {
    UnwindState* s = static_cast<UnwindState*>(arg);
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && s->n < kMaxFrames) g_rec.frames[s->n++] = pc;
    return s->n >= kMaxFrames ? _URC_END_OF_STACK : _URC_NO_REASON;
}

// Copies the lines of /proc/self/maps whose permissions contain 'x' into g_maps.  No allocation, no stdio.
int snapshot_maps() {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    static char chunk[4096];
    static char line[640];
    size_t line_len = 0, out = 0;
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c != '\n') {
                if (line_len < sizeof line - 2) line[line_len++] = c;
                continue;
            }
            // line: "start-end perms ..."; perms start after the first space
            size_t sp = 0;
            while (sp < line_len && line[sp] != ' ') ++sp;
            bool exec = sp + 4 <= line_len && line[sp + 3] == 'x';
            if (exec && out + line_len + 1 <= kMapsCap) {
                std::memcpy(g_maps + out, line, line_len);
                out += line_len;
                g_maps[out++] = '\n';
            }
            line_len = 0;
        }
    }
    close(fd);
    return static_cast<int>(out);
}

void write_all(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
}

void handler(int sig, siginfo_t* info, void*) {
    if (g_entered++ == 0) {
        std::memset(&g_rec, 0, sizeof g_rec);
        std::memcpy(g_rec.magic, kMagic, 8);
        g_rec.sig = sig;
        g_rec.code = info ? info->si_code : 0;
        g_rec.fault_addr = info ? reinterpret_cast<uint64_t>(info->si_addr) : 0;
        g_rec.pid = getpid();
        g_rec.tid = static_cast<int32_t>(syscall(SYS_gettid));
        g_rec.unix_time = time(nullptr);
        prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(g_rec.thread_name), 0, 0, 0);
        std::memcpy(g_rec.version, g_version, sizeof g_rec.version);
        std::memcpy(g_rec.device, g_device, sizeof g_rec.device);
        std::memcpy(g_rec.abi, g_abi, sizeof g_rec.abi);
        g_rec.sdk = g_sdk;
        UnwindState us{0};
        _Unwind_Backtrace(unwind_cb, &us);
        g_rec.nframes = us.n;
        g_rec.maps_len = snapshot_maps();
        int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            write_all(fd, &g_rec, sizeof g_rec);
            write_all(fd, g_maps, static_cast<size_t>(g_rec.maps_len));
            fsync(fd);
            close(fd);
        }
    }
    // Give the system its chance (debuggerd tombstone): restore the previous handler.  A fault re-executes the
    // faulting instruction when we return; signals sent by kill / tkill / abort() must be raised again.
    if (sig > 0 && sig < 32) sigaction(sig, &g_old[sig], nullptr);
    if (info && info->si_code <= 0) syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
}

// ----------------------------------------------------------------------------------- stage 2

struct MapEntry {
    uint64_t start, end, file_offset;
    std::string path;
};

std::vector<MapEntry> parse_maps(const char* text, size_t len) {
    std::vector<MapEntry> v;
    std::string all(text, len);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) nl = all.size();
        std::string line = all.substr(pos, nl - pos);
        pos = nl + 1;
        unsigned long long s = 0, e = 0, off = 0;
        char perms[8] = {0};
        int consumed = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &s, &e, perms, &off, &consumed) < 4) continue;
        MapEntry m{s, e, off, consumed > 0 && size_t(consumed) <= line.size() ? line.substr(consumed) : std::string()};
        v.push_back(std::move(m));
    }
    return v;
}

} // namespace

namespace nue::android::native {

void crash_convert_pending(const char* crash_dir) {
    std::string bin = std::string(crash_dir) + "/last_crash.bin";
    FILE* f = std::fopen(bin.c_str(), "rb");
    if (!f) return;
    RawHeader h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, kMagic, 8) == 0 && h.maps_len >= 0 &&
              size_t(h.maps_len) <= kMapsCap && h.nframes >= 0 && h.nframes <= kMaxFrames;
    std::string maps_text;
    if (ok) {
        maps_text.resize(size_t(h.maps_len));
        ok = h.maps_len == 0 || std::fread(&maps_text[0], 1, size_t(h.maps_len), f) == size_t(h.maps_len);
    }
    std::fclose(f);
    std::error_code ec;
    if (!ok) {
        fs::remove(bin, ec);
        return;
    }
    std::vector<MapEntry> maps = parse_maps(maps_text.data(), maps_text.size());

    CrashInfo info;
    info.signal = h.sig;
    info.si_code = h.code;
    info.fault_address = h.fault_addr;
    info.pid = h.pid;
    info.tid = h.tid;
    info.thread_name = h.thread_name;
    info.app_version = h.version;
    info.abi = h.abi;
    info.device = h.device;
    info.sdk_int = h.sdk;
    info.unix_time = h.unix_time;
    for (int i = 0; i < h.nframes; ++i) {
        CrashFrame fr;
        fr.pc = h.frames[i];
        for (const MapEntry& m : maps) {
            if (fr.pc >= m.start && fr.pc < m.end) {
                fr.module = m.path;
                fr.module_offset = fr.pc - m.start + m.file_offset;
                break;
            }
        }
        if (fr.module.empty()) fr.module_offset = fr.pc;
        info.frames.push_back(std::move(fr));
    }
    std::string text = format_crash_report(info);
    std::string out = std::string(crash_dir) + "/" + crash_file_name(h.unix_time, h.pid);
    if (FILE* o = std::fopen(out.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), o);
        std::fclose(o);
    }
    fs::remove(bin, ec);

    NLOGE("previous run crashed (%s), report saved to %s", signal_name(h.sig), out.c_str());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        NLOGE("crash| %.*s", int(nl - pos), text.c_str() + pos);
        pos = nl + 1;
    }

    // keep the newest 10 reports
    std::vector<fs::path> reports;
    for (fs::directory_iterator it(crash_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.rfind("crash-", 0) == 0) reports.push_back(it->path());
    }
    std::sort(reports.begin(), reports.end());
    while (reports.size() > 10) {
        fs::remove(reports.front(), ec);
        reports.erase(reports.begin());
    }
}

} // namespace nue::android::native

extern "C" void nue_android_install_crash_handler(const char* crash_dir, const char* app_version) {
    using namespace nue::android::native;
    if (!crash_dir || !*crash_dir || g_installed) return;
    std::error_code ec;
    fs::create_directories(crash_dir, ec);
    std::snprintf(g_path, sizeof g_path, "%s/last_crash.bin", crash_dir);
    std::snprintf(g_version, sizeof g_version, "%s", app_version ? app_version : "");
    {
        State& st = state();
        std::lock_guard<std::mutex> lock(st.mu);
        std::snprintf(g_device, sizeof g_device, "%s %s", st.device.manufacturer, st.device.model);
        std::snprintf(g_abi, sizeof g_abi, "%s", st.device.abi);
        g_sdk = st.device.sdk_int;
    }
    crash_convert_pending(crash_dir);

    stack_t ss{};
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof g_altstack;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : kSignals) sigaction(sig, &sa, &g_old[sig]);
    g_installed = true;
    NLOGI("crash handler installed, reports in %s", crash_dir);
}
