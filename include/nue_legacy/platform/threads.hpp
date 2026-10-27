// Platform API: threads, mutexes, condition variables, semaphores, thread-local storage and timers. Shaped like NUE's nue_thread_* /
// nue_mutex_* / nue_cond_* / nue_semaphore_* / nue_tls_* / nue_timer_* (include/nue/core.h). The Win32 synchronisation layer (events,
// mutexes, critical sections, waits, TLS, CreateThread, timeSetEvent) is built on this interface only: there is no call to the host OS in it.
#pragma once
#include "nue_legacy/platform/types.hpp"
#include <functional>
#include <string_view>

namespace nue::legacy::platform {

using ThreadHandle = Id<struct ThreadTag>;
using MutexHandle = Id<struct MutexTag>;
using CondHandle = Id<struct CondTag>;
using SemaphoreHandle = Id<struct SemaphoreTag>;
using TlsHandle = Id<struct TlsTag>;
using TimerHandle = Id<struct TimerTag>;

enum class ThreadPriority : int8_t { lowest = -2, below_normal = -1, normal = 0, above_normal = 1, highest = 2 };

struct ThreadDesc {
    std::string_view name;
    uint32_t stack_bytes = 0;                           // 0 = platform default
    ThreadPriority priority = ThreadPriority::normal;
    uint64_t affinity_mask = 0;                         // 0 = any core
};

class Threads {
public:
    virtual ~Threads() = default;

    // Threads. The function's return value is the exit code `join` reports.
    virtual Result<ThreadHandle> create_thread(const ThreadDesc&, std::function<int32_t()> body) = 0;
    virtual Status join(ThreadHandle, int32_t* exit_code) = 0;          // destroys the handle
    virtual Status detach(ThreadHandle) = 0;                            // destroys the handle
    virtual uint64_t current_thread_id() = 0;                           // stable per thread, never 0
    virtual void yield() = 0;
    virtual Status set_priority(ThreadHandle, ThreadPriority) = 0;      // best effort: unsupported where the OS has no such control
    virtual Status set_affinity(ThreadHandle, uint64_t mask) = 0;

    // Mutexes and condition variables (wait with a timeout in ns; negative = forever, 0 = try; timed_out when it expires).
    virtual Result<MutexHandle> mutex_create(bool recursive) = 0;
    virtual void mutex_destroy(MutexHandle) = 0;
    virtual void mutex_lock(MutexHandle) = 0;
    virtual bool mutex_trylock(MutexHandle) = 0;
    virtual void mutex_unlock(MutexHandle) = 0;
    virtual Result<CondHandle> cond_create() = 0;
    virtual void cond_destroy(CondHandle) = 0;
    virtual Status cond_wait(CondHandle, MutexHandle, int64_t timeout_ns) = 0;   // ok | timed_out
    virtual void cond_signal(CondHandle) = 0;
    virtual void cond_broadcast(CondHandle) = 0;

    // Counting semaphores.
    virtual Result<SemaphoreHandle> semaphore_create(uint32_t initial) = 0;
    virtual void semaphore_destroy(SemaphoreHandle) = 0;
    virtual Status semaphore_wait(SemaphoreHandle, int64_t timeout_ns) = 0;
    virtual void semaphore_post(SemaphoreHandle) = 0;

    // Thread-local storage: one value per (slot, thread), null until set.
    virtual Result<TlsHandle> tls_create() = 0;
    virtual void tls_destroy(TlsHandle) = 0;
    virtual void* tls_get(TlsHandle) = 0;
    virtual Status tls_set(TlsHandle, void* value) = 0;

    // Timers: the callback runs on a timer thread every `period_ns` (0 = once) after `delay_ns`. `cancel` stops future callbacks and returns
    // immediately; `destroy` cancels and waits for a callback in flight (never from the callback itself).
    virtual Result<TimerHandle> timer_create(int64_t delay_ns, int64_t period_ns, std::function<void()> callback) = 0;
    virtual void timer_cancel(TimerHandle) = 0;
    virtual void timer_destroy(TimerHandle) = 0;
};

}  // namespace nue::legacy::platform
