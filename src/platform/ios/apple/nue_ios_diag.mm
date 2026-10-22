// Diagnostics: os_log sink, Instruments signposts, crash handler (signals) and an optional MetricKit subscriber.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <Foundation/Foundation.h>
#import <MetricKit/MetricKit.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <os/log.h>
#include <os/signpost.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ucontext.h>

#include "nue_ios_internal.hpp"

namespace {
os_log_t engine_log() {
    static os_log_t log = os_log_create("org.nue.engine", "engine");
    return log;
}
os_log_t poi_log() {
    static os_log_t log = os_log_create("org.nue.engine", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    return log;
}

// ---- crash handler ---------------------------------------------------------------------------------------------------------------------------
char g_crash_path[1024];
std::atomic<bool> g_in_crash{false};
stack_t g_alt_stack;
struct sigaction g_old[32];
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};

// Async-signal-safe helpers: only write(2) and manual integer formatting.
void put(int fd, const char* s) { (void)!write(fd, s, std::strlen(s)); }
void put_hex(int fd, uintptr_t v) {
    char buf[2 + sizeof(uintptr_t) * 2 + 1];
    buf[0] = '0'; buf[1] = 'x';
    int n = 2;
    bool started = false;
    for (int shift = static_cast<int>(sizeof(uintptr_t) * 8) - 4; shift >= 0; shift -= 4) {
        int d = static_cast<int>((v >> shift) & 0xF);
        if (d || started || shift == 0) { buf[n++] = "0123456789abcdef"[d]; started = true; }
    }
    (void)!write(fd, buf, static_cast<size_t>(n));
}

void crash_handler(int sig, siginfo_t* info, void* ucontext) {
    if (g_in_crash.exchange(true)) _exit(128 + sig);   // crash inside the handler: give up
    int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        put(fd, "NUE crash report\nsignal: "); put_hex(fd, static_cast<uintptr_t>(sig));
        put(fd, "\ncode: "); put_hex(fd, static_cast<uintptr_t>(info ? info->si_code : 0));
        put(fd, "\nfault address: "); put_hex(fd, reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
#if defined(__arm64__)
        if (ucontext) {
            auto* uc = static_cast<ucontext_t*>(ucontext);
            put(fd, "\npc: "); put_hex(fd, static_cast<uintptr_t>(__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss)));
            put(fd, "\nlr: "); put_hex(fd, static_cast<uintptr_t>(__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss)));
            put(fd, "\nsp: "); put_hex(fd, static_cast<uintptr_t>(__darwin_arm_thread_state64_get_sp(uc->uc_mcontext->__ss)));
        }
#endif
        put(fd, "\nbacktrace:\n");
        void* frames[64];
        int n = backtrace(frames, 64);
        backtrace_symbols_fd(frames, n, fd);   // not formally async-signal-safe but the standard practice for crash logs
        fsync(fd);
        close(fd);
    }
    // Restore the previous handler (the system's, which produces the Apple crash report) and re-raise.
    for (int s : kSignals) if (s == sig) sigaction(sig, &g_old[sig], nullptr);
    raise(sig);
}
}  // namespace

@interface NUEMetricSubscriber : NSObject <MXMetricManagerSubscriber>
@end
@implementation NUEMetricSubscriber
- (void)didReceiveDiagnosticPayloads:(NSArray<MXDiagnosticPayload*>*)payloads API_AVAILABLE(ios(14.0)) {
    char dir[1024];
    if (nue_ios_path(NUE_IOS_PATH_LOGS, dir, sizeof dir) < 0) return;
    for (MXDiagnosticPayload* p in payloads) {
        NSString* file = [NSString stringWithFormat:@"%s/metrickit-%lld.json", dir, static_cast<long long>([[NSDate date] timeIntervalSince1970])];
        [[p JSONRepresentation] writeToFile:file atomically:YES];
    }
}
@end

extern "C" {

void nue_ios_log(int level, const char* tag, const char* message) {
    os_log_type_t t = OS_LOG_TYPE_DEFAULT;
    switch (level) {
    case 0: t = OS_LOG_TYPE_DEBUG; break;
    case 1: t = OS_LOG_TYPE_INFO; break;
    case 2: t = OS_LOG_TYPE_DEFAULT; break;
    default: t = OS_LOG_TYPE_ERROR; break;
    }
    // %{public}s: dynamic strings are redacted by default in os_log; engine messages carry no user data.
    os_log_with_type(engine_log(), t, "[%{public}s] %{public}s", tag ? tag : "nue", message ? message : "");
}

uint64_t nue_ios_signpost_begin(const char* name) {
    os_signpost_id_t id = os_signpost_id_generate(poi_log());
    os_signpost_interval_begin(poi_log(), id, "engine", "%{public}s", name ? name : "interval");
    return id;
}
void nue_ios_signpost_end(const char* name, uint64_t id) {
    os_signpost_interval_end(poi_log(), static_cast<os_signpost_id_t>(id), "engine", "%{public}s", name ? name : "interval");
}
void nue_ios_signpost_event(const char* name) {
    os_signpost_event_emit(poi_log(), OS_SIGNPOST_ID_EXCLUSIVE, "event", "%{public}s", name ? name : "event");
}

int nue_ios_install_crash_handler(void) {
    static bool installed = false;
    if (installed) return 0;
    char dir[1024];
    if (nue_ios_path(NUE_IOS_PATH_LOGS, dir, sizeof dir) < 0) return -1;
    std::snprintf(g_crash_path, sizeof g_crash_path, "%s/last-crash.txt", dir);

    // Own stack so a stack overflow (SIGSEGV) can still be reported.
    g_alt_stack.ss_sp = std::malloc(64 * 1024);
    g_alt_stack.ss_size = 64 * 1024;
    g_alt_stack.ss_flags = 0;
    if (!g_alt_stack.ss_sp || sigaltstack(&g_alt_stack, nullptr) != 0) return -1;

    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : kSignals) sigaction(s, &sa, &g_old[s]);

    if (@available(iOS 14.0, *)) {
        static NUEMetricSubscriber* sub = [NUEMetricSubscriber new];
        [[MXMetricManager sharedManager] addSubscriber:sub];
    }
    installed = true;
    nue_ios_log(1, "diag", "crash handler installed");
    return 0;
}

}  // extern "C"
