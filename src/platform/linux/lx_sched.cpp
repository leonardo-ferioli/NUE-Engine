#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "lx_sched.hpp"

#include <errno.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <cstring>

#include "lx_diag.hpp"
#include "lx_gamemode.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define NUE_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define NUE_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define NUE_CPU_PAUSE() ((void)0)
#endif

namespace nue::lx {

int64_t current_tid() { return static_cast<int64_t>(syscall(SYS_gettid)); }

bool set_thread_name(const std::string& name) {
    char buf[16] = {};
    std::strncpy(buf, name.c_str(), 15);
    return prctl(PR_SET_NAME, buf, 0, 0, 0) == 0;
}

PriorityLimits query_priority_limits() {
    PriorityLimits l;
    l.is_root = geteuid() == 0;
    struct rlimit r;
    if (getrlimit(RLIMIT_RTPRIO, &r) == 0) l.max_rt_priority = r.rlim_cur == RLIM_INFINITY ? 99 : static_cast<int>(r.rlim_cur);
    if (getrlimit(RLIMIT_NICE, &r) == 0) l.min_nice = r.rlim_cur == RLIM_INFINITY ? -20 : 20 - static_cast<int>(r.rlim_cur);
    if (l.is_root) {
        l.max_rt_priority = 99;
        l.min_nice = -20;
    }
    return l;
}

bool set_thread_nice(int nice) { return setpriority(PRIO_PROCESS, static_cast<id_t>(current_tid()), nice) == 0; }

namespace {
constexpr int kIoprioWhoProcess = 1;  // applies to a single thread (tid)
constexpr int kIoprioClassBe = 2, kIoprioClassIdle = 3, kIoprioShift = 13;
bool ioprio_set(int klass, int level) {
    return syscall(SYS_ioprio_set, kIoprioWhoProcess, static_cast<int>(current_tid()), (klass << kIoprioShift) | level) == 0;
}
}  // namespace

bool set_io_priority_best_effort(int level) { return ioprio_set(kIoprioClassBe, level < 0 ? 0 : level > 7 ? 7 : level); }
bool set_io_priority_idle() { return ioprio_set(kIoprioClassIdle, 0); }

bool set_thread_policy(SchedPolicy p, int rt_priority) {
    struct sched_param sp;
    std::memset(&sp, 0, sizeof sp);
    int policy = SCHED_OTHER;
    switch (p) {
        case SchedPolicy::Batch: policy = SCHED_BATCH; break;
        case SchedPolicy::Idle: policy = SCHED_IDLE; break;
        case SchedPolicy::Fifo: policy = SCHED_FIFO; sp.sched_priority = rt_priority; break;
        case SchedPolicy::RoundRobin: policy = SCHED_RR; sp.sched_priority = rt_priority; break;
        default: break;
    }
    if (p == SchedPolicy::Fifo || p == SchedPolicy::RoundRobin) policy |= SCHED_RESET_ON_FORK;  // children never inherit RT
    return sched_setscheduler(static_cast<pid_t>(current_tid()), policy, &sp) == 0;
}

bool set_timer_slack_ns(unsigned long ns) { return prctl(PR_SET_TIMERSLACK, ns, 0, 0, 0) == 0; }

std::string raise_thread_priority(bool allow_realtime, const std::string& label) {
    PriorityLimits lim = query_priority_limits();
    if (allow_realtime && lim.max_rt_priority > 0) {
        int prio = lim.max_rt_priority < 10 ? lim.max_rt_priority : 10;
        if (set_thread_policy(SchedPolicy::RoundRobin, prio)) return "SCHED_RR " + std::to_string(prio) + " (" + label + ")";
    }
    RtKit rk;  // reaches the system bus only when rtkit exists, otherwise fails fast
    const uint64_t tid = static_cast<uint64_t>(current_tid());
    if (allow_realtime && rk.make_realtime(tid, 10)) return "rtkit realtime 10 (" + label + ")";
    if (lim.min_nice < 0) {
        int nice = lim.min_nice > -5 ? lim.min_nice : -5;
        if (set_thread_nice(nice)) return "nice " + std::to_string(nice) + " (" + label + ")";
    }
    if (rk.make_high_priority(tid, -10)) return "rtkit high priority (" + label + ")";
    return "nothing applied (" + label + ")";
}

void drop_thread_priority_to_background() {
    set_thread_policy(SchedPolicy::Batch);
    set_thread_nice(10);
    set_io_priority_best_effort(6);
}

// ---------------------------------------------------------------------------- affinity

bool get_thread_affinity(std::vector<int>& cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(static_cast<pid_t>(current_tid()), sizeof set, &set) != 0) return false;
    cpus.clear();
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &set)) cpus.push_back(i);
    return true;
}

static bool apply_affinity(pid_t who, const std::vector<int>& cpus) {
    if (cpus.empty()) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus)
        if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
    return sched_setaffinity(who, sizeof set, &set) == 0;
}

bool set_thread_affinity(const std::vector<int>& cpus) { return apply_affinity(static_cast<pid_t>(current_tid()), cpus); }
bool set_process_affinity(const std::vector<int>& cpus) { return apply_affinity(0, cpus); }

// ---------------------------------------------------------------------------- process

uint64_t current_core_limit() {
    struct rlimit r;
    if (getrlimit(RLIMIT_CORE, &r) != 0) return 0;
    return r.rlim_cur == RLIM_INFINITY ? UINT64_MAX : static_cast<uint64_t>(r.rlim_cur);
}

uint64_t raise_core_limit() {
    struct rlimit r;
    if (getrlimit(RLIMIT_CORE, &r) == 0 && r.rlim_cur != r.rlim_max) {
        r.rlim_cur = r.rlim_max;
        setrlimit(RLIMIT_CORE, &r);
    }
    return current_core_limit();
}

bool set_process_dumpable(bool dumpable) { return prctl(PR_SET_DUMPABLE, dumpable ? 1 : 0, 0, 0, 0) == 0; }

bool can_lock_memory(size_t bytes) {
    struct rlimit r;
    if (getrlimit(RLIMIT_MEMLOCK, &r) != 0) return false;
    return r.rlim_cur == RLIM_INFINITY || r.rlim_cur >= bytes;
}

// ---------------------------------------------------------------------------- time

int64_t monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

int64_t realtime_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

bool sleep_until_ns(int64_t deadline_ns, int64_t spin_margin_ns) {
    SleepPlan plan = plan_sleep(monotonic_ns(), deadline_ns, spin_margin_ns);
    if (plan.needs_sleep) {
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(plan.sleep_until_ns / 1000000000ll);
        ts.tv_nsec = static_cast<long>(plan.sleep_until_ns % 1000000000ll);
        int r;
        do {
            r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        } while (r == EINTR);  // absolute sleeps can simply be resumed with the same deadline
    }
    while (monotonic_ns() < plan.spin_until_ns) NUE_CPU_PAUSE();
    return true;
}

bool PeriodicTimer::start(int64_t period_ns) {
    close();
    fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd_ < 0) return false;
    struct itimerspec its;
    its.it_interval.tv_sec = static_cast<time_t>(period_ns / 1000000000ll);
    its.it_interval.tv_nsec = static_cast<long>(period_ns % 1000000000ll);
    its.it_value = its.it_interval;
    if (timerfd_settime(fd_, 0, &its, nullptr) != 0) {
        close();
        return false;
    }
    return true;
}

void PeriodicTimer::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

uint64_t PeriodicTimer::wait() {
    if (fd_ < 0) return 0;
    uint64_t n = 0;
    ssize_t r;
    do {
        r = read(fd_, &n, sizeof n);
    } while (r < 0 && errno == EINTR);
    return r == static_cast<ssize_t>(sizeof n) ? n : 0;
}

}  // namespace nue::lx
