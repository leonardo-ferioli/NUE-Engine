// Thread and process scheduling helpers (Linux only): names, nice, I/O priority, CPU affinity,
// real-time policy (direct, falling back to RealtimeKit), timer slack and high-resolution
// timing. All calls report success/failure instead of aborting: unprivileged processes are
// expected to be refused many of these and the engine continues.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nue::lx {

// ---- identity ------------------------------------------------------------------------
int64_t current_tid();
bool set_thread_name(const std::string& name);  // at most 15 bytes are kept

// ---- priority ------------------------------------------------------------------------
struct PriorityLimits {
    int max_rt_priority = 0;   // RLIMIT_RTPRIO soft limit (0 = no real-time without rtkit/root)
    int min_nice = 0;          // lowest nice value allowed (RLIMIT_NICE: 20 - limit)
    bool is_root = false;
};
PriorityLimits query_priority_limits();

bool set_thread_nice(int nice);                    // calling thread (Linux nice is per thread)
bool set_io_priority_best_effort(int level);       // 0 (highest) .. 7, calling thread
bool set_io_priority_idle();
enum class SchedPolicy { Other, Batch, Idle, Fifo, RoundRobin };
bool set_thread_policy(SchedPolicy p, int rt_priority = 0);  // calling thread
bool set_timer_slack_ns(unsigned long ns);          // PR_SET_TIMERSLACK, calling thread (default 50 us)
// Best effort ladder: real-time if allowed (rlimit), else rtkit, else nice. Returns a description
// of what was applied ("SCHED_RR 10", "rtkit high priority nice -11", "nice -5", "nothing").
std::string raise_thread_priority(bool allow_realtime, const std::string& thread_label);
void drop_thread_priority_to_background();          // loaders, shader compilation, logging

// ---- affinity ------------------------------------------------------------------------
bool get_thread_affinity(std::vector<int>& cpus);
bool set_thread_affinity(const std::vector<int>& cpus);   // calling thread
bool set_process_affinity(const std::vector<int>& cpus);

// ---- process -------------------------------------------------------------------------
// Raises RLIMIT_CORE soft to the hard limit; returns the resulting soft limit (UINT64_MAX = unlimited).
uint64_t raise_core_limit();
uint64_t current_core_limit();
bool set_process_dumpable(bool dumpable);
bool can_lock_memory(size_t bytes);  // RLIMIT_MEMLOCK has room for an mlock of this size

// ---- time ----------------------------------------------------------------------------
int64_t monotonic_ns();
int64_t realtime_ns();
// Sleeps until the absolute CLOCK_MONOTONIC deadline: clock_nanosleep up to `spin_margin_ns` before it,
// then spins with a pause instruction.
bool sleep_until_ns(int64_t deadline_ns, int64_t spin_margin_ns = 150000);

// timerfd-based periodic timer for fixed-rate loops (fixed update, audio pump).
class PeriodicTimer {
public:
    ~PeriodicTimer() { close(); }
    bool start(int64_t period_ns);
    void close();
    // Blocks until the next tick; returns the number of expirations (>1 means ticks were missed), 0 on error.
    uint64_t wait();
    bool is_open() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

}  // namespace nue::lx
