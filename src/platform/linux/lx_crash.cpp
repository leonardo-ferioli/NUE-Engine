#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "lx_crash.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>

#if defined(__GLIBC__)
#include <execinfo.h>
#define NUE_HAVE_BACKTRACE 1
#endif

#include "lx_diag.hpp"
#include "lx_sched.hpp"
#include "lx_signal_safe.hpp"

namespace nue::lx {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
constexpr size_t kAltStackSize = 64 * 1024;

char g_dir[400];
char g_app[64];
char g_version[64];
char g_report_path[512];
bool g_write_maps = true;
void (*g_hook)(int, void*, void*, int, void*) = nullptr;
void* g_hook_user = nullptr;
const char* (*g_begin)(int, void*, void*, void*) = nullptr;
bool (*g_end)(int, void*) = nullptr;
void* g_sink_user = nullptr;
bool g_installed = false;
struct sigaction g_previous[sizeof(kSignals) / sizeof(kSignals[0])];
volatile sig_atomic_t g_in_handler = 0;

void copy_str(char* dst, size_t cap, const std::string& s) {
    size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

void put(int fd, const char* s) {
    size_t n = std::strlen(s);
    while (n) {
        ssize_t w = ::write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= static_cast<size_t>(w);
    }
}

void put_w(int fd, const FixedWriter& w) {
    const char* p = w.data();
    size_t n = w.size();
    while (n) {
        ssize_t r = ::write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
}

void line_kv_hex(int fd, const char* key, uint64_t v) {
    char b[96];
    FixedWriter w(b, sizeof b);
    w.str(key);
    w.str(" = ");
    w.hex(v, 16);
    w.chr('\n');
    put_w(fd, w);
}

void dump_registers(int fd, void* ucv) {
    ucontext_t* uc = static_cast<ucontext_t*>(ucv);
    if (!uc) return;
    put(fd, "\nregisters:\n");
#if defined(__x86_64__)
    static const char* names[] = {"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rdi", "rsi", "rbp", "rbx", "rdx", "rax", "rcx", "rsp", "rip"};
    for (int i = 0; i < 17; ++i) line_kv_hex(fd, names[i], static_cast<uint64_t>(uc->uc_mcontext.gregs[i]));
#elif defined(__aarch64__)
    char name[8];
    for (int i = 0; i < 31; ++i) {
        FixedWriter w(name, sizeof name - 1);
        w.chr('x');
        w.dec(i);
        name[w.size()] = 0;
        line_kv_hex(fd, name, uc->uc_mcontext.regs[i]);
    }
    line_kv_hex(fd, "sp", uc->uc_mcontext.sp);
    line_kv_hex(fd, "pc", uc->uc_mcontext.pc);
#else
    (void)uc;
    put(fd, "(register dump not implemented for this architecture)\n");
#endif
}

void dump_maps(int fd) {
    int m = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (m < 0) return;
    put(fd, "\nmaps:\n");
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(m, buf, sizeof buf);
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::write(fd, buf + off, static_cast<size_t>(n - off));
            if (w < 0) break;
            off += w;
        }
    }
    ::close(m);
}

void handler(int sig, siginfo_t* si, void* uc) {
    const int saved_errno = errno;
    if (g_in_handler++ == 0) {
        char b[600];
        FixedWriter p(b, sizeof b - 1);
        const char* sink_dir = g_begin ? g_begin(sig, si, uc, g_sink_user) : nullptr;
        if (sink_dir && *sink_dir) {
            p.str(sink_dir);
            p.str("/stack.txt");
        } else {
            p.str(g_dir);
            p.str("/crash-");
            p.dec(::getpid());
            p.chr('-');
            p.dec(static_cast<int64_t>(::time(nullptr)));
            p.str(".txt");
        }
        b[p.size()] = 0;
        copy_str(g_report_path, sizeof g_report_path, b);
        int fd = ::open(g_report_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) fd = 2;
        char line[256];
        FixedWriter w(line, sizeof line);
        w.str(g_app);
        w.chr(' ');
        w.str(g_version);
        w.str(" crashed\nsignal = ");
        w.dec(sig);
        w.chr(' ');
        w.str(signal_name(sig));
        w.str("\nsi_code = ");
        w.dec(si ? si->si_code : 0);
        w.str("\nfault_address = ");
        w.hex(si ? reinterpret_cast<uint64_t>(si->si_addr) : 0, 16);
        w.str("\npid = ");
        w.dec(::getpid());
        w.str("\ntid = ");
        w.dec(current_tid());
        w.chr('\n');
        put_w(fd, w);
        dump_registers(fd, uc);
#if defined(NUE_HAVE_BACKTRACE)
        put(fd, "\nbacktrace:\n");
        void* frames[64];
        int n = backtrace(frames, 64);
        backtrace_symbols_fd(frames, n, fd);
#endif
        if (g_write_maps) dump_maps(fd);
        if (g_hook) g_hook(sig, si, uc, fd, g_hook_user);
        if (fd != 2) ::close(fd);
        if (g_end && g_end(sig, g_sink_user)) ::_exit(128 + sig);
    }
    // Let the default action (core dump / crash collector) happen.
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    ::sigaction(sig, &dfl, nullptr);
    errno = saved_errno;
    ::raise(sig);
}

thread_local bool t_stack_installed = false;

bool install_alt_stack_this_thread() {
    if (t_stack_installed) return true;
    void* mem = std::malloc(kAltStackSize);  // intentionally never freed: lives as long as the thread
    if (!mem) return false;
    stack_t ss;
    ss.ss_sp = mem;
    ss.ss_size = kAltStackSize;
    ss.ss_flags = 0;
    if (::sigaltstack(&ss, nullptr) != 0) {
        std::free(mem);
        return false;
    }
    t_stack_installed = true;
    return true;
}

}  // namespace

bool install_crash_handler(const CrashOptions& o) {
    copy_str(g_dir, sizeof g_dir, o.crash_dir.empty() ? "." : o.crash_dir);
    copy_str(g_app, sizeof g_app, o.app_name);
    copy_str(g_version, sizeof g_version, o.app_version);
    g_write_maps = o.write_maps;
    g_hook = o.hook;
    g_hook_user = o.hook_user;
    g_begin = o.begin;
    g_end = o.end;
    g_sink_user = o.sink_user;
    if (o.raise_core_limit) raise_core_limit();
#if defined(NUE_HAVE_BACKTRACE)
    {  // first use of backtrace() may allocate / dlopen libgcc: do it now, not inside the handler
        void* warm[2];
        backtrace(warm, 2);
    }
#endif
    if (g_installed) return true;
    if (!install_alt_stack_this_thread()) return false;
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        if (::sigaction(kSignals[i], &sa, &g_previous[i]) != 0) return false;
    }
    g_installed = true;
    return true;
}

void uninstall_crash_handler() {
    if (!g_installed) return;
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) ::sigaction(kSignals[i], &g_previous[i], nullptr);
    g_installed = false;
}

bool write_stack_report(const char* dir) {
    if (!dir || !*dir) return false;
    char path[600];
    FixedWriter p(path, sizeof path - 1);
    p.str(dir);
    p.str("/stack.txt");
    path[p.size()] = 0;
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    char line[160];
    FixedWriter w(line, sizeof line);
    w.str(g_app);
    w.chr(' ');
    w.str(g_version);
    w.str(" stack report (not a crash)\npid = ");
    w.dec(::getpid());
    w.str("\ntid = ");
    w.dec(current_tid());
    w.chr('\n');
    put_w(fd, w);
#if defined(NUE_HAVE_BACKTRACE)
    put(fd, "\nbacktrace (calling thread):\n");
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, fd);
#endif
    dump_maps(fd);
    ::close(fd);
    return true;
}

bool crash_handler_attach_thread() { return install_alt_stack_this_thread(); }

const char* last_crash_report_path() { return g_report_path; }

std::string crash_handler_selftest(const std::string& crash_dir) {
    pid_t child = ::fork();
    if (child < 0) return std::string();
    if (child == 0) {
        ::raise(SIGSEGV);
        ::_exit(0);  // not reached: the handler re-raises with the default action
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    std::string newest;
    time_t newest_mtime = 0;
    if (DIR* d = ::opendir(crash_dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (std::strncmp(e->d_name, "crash-", 6) != 0) continue;
            std::string path = crash_dir + "/" + e->d_name;
            struct stat st;
            if (::stat(path.c_str(), &st) == 0 && st.st_mtime >= newest_mtime) {
                newest_mtime = st.st_mtime;
                newest = path;
            }
        }
        ::closedir(d);
    }
    return newest;
}

}  // namespace nue::lx
