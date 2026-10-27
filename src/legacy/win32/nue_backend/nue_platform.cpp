#include "nue_legacy/platform/nue_platform.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "nue/core.h"
#include "nue/power.h"
#include "nue/system.h"
#include <cstring>
#include <map>
#include <mutex>

namespace nue::legacy::platform::nue_backend {
namespace {

// Handles of the legacy interfaces are 32-bit ids; the native ones are opaque pointers. One table per kind, ids never reused within a run.
template <class T> class Table {
public:
    uint32_t add(T value) { std::lock_guard lock(m_); const uint32_t id = next_++; items_[id] = std::move(value); return id; }
    bool get(uint32_t id, T& out) { std::lock_guard lock(m_); auto it = items_.find(id); if (it == items_.end()) return false; out = it->second; return true; }
    bool take(uint32_t id, T& out) { std::lock_guard lock(m_); auto it = items_.find(id); if (it == items_.end()) return false; out = std::move(it->second); items_.erase(it); return true; }
private:
    std::mutex m_;
    std::map<uint32_t, T> items_;
    uint32_t next_ = 1;
};

// ------------------------------------------------------------------------------------------------------------------------------------ clock
class NueClock final : public Clock {
public:
    NueClock() : zone_(stdlib::make_clock()) {}
    int64_t time_ns() override { return nue_time_ns(); }
    uint64_t counter() override { return nue_time_counter(); }
    uint64_t counter_frequency() override { return nue_time_counter_frequency(); }
    int64_t unix_ms() override { return nue_time_unix_ms(); }
    int32_t local_utc_offset_minutes(int64_t unix_ms) override { return zone_->local_utc_offset_minutes(unix_ms); }
    void sleep_ns(int64_t ns) override {
        if (ns <= 0) { nue_thread_yield(); return; }
        nue_sleep_ns(ns, NUE_SLEEP_PRECISE);
    }
private:
    std::unique_ptr<Clock> zone_;
};

// ------------------------------------------------------------------------------------------------------------------------------------ threads
struct ThreadStart { std::function<int32_t()> body; };
int32_t NUE_CALL run_thread(void* p) {
    std::unique_ptr<ThreadStart> start(static_cast<ThreadStart*>(p));
    return start->body();
}
struct TimerState { std::function<void()> callback; nue_timer timer = nullptr; };
void NUE_CALL run_timer(void* p, nue_timer, uint64_t) { static_cast<TimerState*>(p)->callback(); }

nue_thread_priority native_priority(ThreadPriority p) {
    switch (p) {
    case ThreadPriority::lowest: case ThreadPriority::below_normal: return NUE_THREAD_PRIORITY_LOW;
    case ThreadPriority::above_normal: return NUE_THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadPriority::highest: return NUE_THREAD_PRIORITY_HIGH;
    default: return NUE_THREAD_PRIORITY_NORMAL;
    }
}

class NueThreads final : public Threads {
public:
    Result<ThreadHandle> create_thread(const ThreadDesc& desc, std::function<int32_t()> body) override {
        nue_thread_desc d;
        NUE_STRUCT_INIT(&d);
        const std::string name(desc.name);
        d.name = name.empty() ? nullptr : name.c_str();
        d.stack_size = desc.stack_bytes;
        d.priority = native_priority(desc.priority);
        d.affinity_mask = desc.affinity_mask;
        auto* start = new ThreadStart{std::move(body)};
        nue_thread thread = nullptr;
        const nue_result r = nue_thread_create(&d, run_thread, start, &thread);
        if (r < 0) { delete start; return {status_from(r), {}}; }
        return {Status::ok, ThreadHandle{threads_.add(thread)}};
    }
    Status join(ThreadHandle h, int32_t* exit_code) override {
        nue_thread t;
        if (!threads_.take(h.value, t)) return Status::invalid_argument;
        return status_from(nue_thread_join(t, exit_code));
    }
    Status detach(ThreadHandle h) override {
        nue_thread t;
        if (!threads_.take(h.value, t)) return Status::invalid_argument;
        return status_from(nue_thread_detach(t));
    }
    uint64_t current_thread_id() override { return nue_thread_current_id(); }
    void yield() override { nue_thread_yield(); }
    Status set_priority(ThreadHandle h, ThreadPriority p) override {
        nue_thread t;
        if (!threads_.get(h.value, t)) return Status::invalid_argument;
        return status_from(nue_thread_set_priority(t, native_priority(p)));
    }
    Status set_affinity(ThreadHandle h, uint64_t mask) override {
        nue_thread t;
        if (!threads_.get(h.value, t)) return Status::invalid_argument;
        return status_from(nue_thread_set_affinity(t, mask));
    }

    Result<MutexHandle> mutex_create(bool recursive) override {
        nue_mutex m = nullptr;
        const nue_result r = nue_mutex_create(recursive ? NUE_TRUE : NUE_FALSE, &m);
        if (r < 0) return {status_from(r), {}};
        return {Status::ok, MutexHandle{mutexes_.add(m)}};
    }
    void mutex_destroy(MutexHandle h) override { nue_mutex m; if (mutexes_.take(h.value, m)) nue_mutex_destroy(m); }
    void mutex_lock(MutexHandle h) override { nue_mutex m; if (mutexes_.get(h.value, m)) nue_mutex_lock(m); }
    bool mutex_trylock(MutexHandle h) override { nue_mutex m; return mutexes_.get(h.value, m) && nue_mutex_trylock(m) != NUE_FALSE; }
    void mutex_unlock(MutexHandle h) override { nue_mutex m; if (mutexes_.get(h.value, m)) nue_mutex_unlock(m); }

    Result<CondHandle> cond_create() override {
        nue_cond c = nullptr;
        const nue_result r = nue_cond_create(&c);
        if (r < 0) return {status_from(r), {}};
        return {Status::ok, CondHandle{conds_.add(c)}};
    }
    void cond_destroy(CondHandle h) override { nue_cond c; if (conds_.take(h.value, c)) nue_cond_destroy(c); }
    Status cond_wait(CondHandle ch, MutexHandle mh, int64_t timeout_ns) override {
        nue_cond c; nue_mutex m;
        if (!conds_.get(ch.value, c) || !mutexes_.get(mh.value, m)) return Status::invalid_argument;
        return status_from(nue_cond_wait(c, m, timeout_ns));
    }
    void cond_signal(CondHandle h) override { nue_cond c; if (conds_.get(h.value, c)) nue_cond_signal(c); }
    void cond_broadcast(CondHandle h) override { nue_cond c; if (conds_.get(h.value, c)) nue_cond_broadcast(c); }

    Result<SemaphoreHandle> semaphore_create(uint32_t initial) override {
        nue_semaphore s = nullptr;
        const nue_result r = nue_semaphore_create(initial, &s);
        if (r < 0) return {status_from(r), {}};
        return {Status::ok, SemaphoreHandle{semaphores_.add(s)}};
    }
    void semaphore_destroy(SemaphoreHandle h) override { nue_semaphore s; if (semaphores_.take(h.value, s)) nue_semaphore_destroy(s); }
    Status semaphore_wait(SemaphoreHandle h, int64_t timeout_ns) override {
        nue_semaphore s;
        if (!semaphores_.get(h.value, s)) return Status::invalid_argument;
        return status_from(nue_semaphore_wait(s, timeout_ns));
    }
    void semaphore_post(SemaphoreHandle h) override { nue_semaphore s; if (semaphores_.get(h.value, s)) nue_semaphore_post(s); }

    Result<TlsHandle> tls_create() override {
        nue_tls_id id = 0;
        const nue_result r = nue_tls_create(nullptr, &id);
        if (r < 0) return {status_from(r), {}};
        return {Status::ok, TlsHandle{id}};
    }
    void tls_destroy(TlsHandle h) override { nue_tls_destroy(h.value); }
    void* tls_get(TlsHandle h) override { return nue_tls_get(h.value); }
    Status tls_set(TlsHandle h, void* value) override { return status_from(nue_tls_set(h.value, value)); }

    Result<TimerHandle> timer_create(int64_t delay_ns, int64_t period_ns, std::function<void()> callback) override {
        auto* state = new TimerState{std::move(callback), nullptr};
        nue_timer_desc d;
        NUE_STRUCT_INIT(&d);
        if (period_ns > 0) { d.period_ns = period_ns; d.first_delay_ns = delay_ns; d.one_shot = NUE_FALSE; }
        else { d.period_ns = delay_ns > 0 ? delay_ns : 1; d.first_delay_ns = 0; d.one_shot = NUE_TRUE; }
        d.callback = run_timer;
        d.userdata = state;
        const nue_result r = nue_timer_create(&d, &state->timer);
        if (r < 0) { delete state; return {status_from(r), {}}; }
        return {Status::ok, TimerHandle{timers_.add(state)}};
    }
    void timer_cancel(TimerHandle h) override { TimerState* s; if (timers_.get(h.value, s)) nue_timer_cancel(s->timer); }
    void timer_destroy(TimerHandle h) override {
        TimerState* s;
        if (!timers_.take(h.value, s)) return;
        nue_timer_destroy(s->timer);       // waits for a callback in flight, so the state can be freed
        delete s;
    }

private:
    Table<nue_thread> threads_;
    Table<nue_mutex> mutexes_;
    Table<nue_cond> conds_;
    Table<nue_semaphore> semaphores_;
    Table<TimerState*> timers_;
};

// ------------------------------------------------------------------------------------------------------------------------------------ system
class NueSystem final : public System {
public:
    NueSystem() : base_(stdlib::make_system()) {}
    SystemInfo info() override {
        SystemInfo out = base_->info();
        nue_system_info native;
        NUE_STRUCT_INIT(&native);
        if (nue_system_get_info(&native) >= 0) {
            if (native.cpu_logical_cores) out.logical_processors = native.cpu_logical_cores;
            if (native.memory_total_bytes) { out.total_memory = native.memory_total_bytes; out.available_memory = native.memory_available_bytes; }
            if (native.cpu_name[0]) out.cpu_name = native.cpu_name;
            if (native.os_name[0]) out.os_name = native.os_name;
        }
        return out;
    }
private:
    std::unique_ptr<System> base_;
};

}  // namespace

std::unique_ptr<Clock> make_clock() { return std::make_unique<NueClock>(); }
std::unique_ptr<Threads> make_threads() { return std::make_unique<NueThreads>(); }
std::unique_ptr<System> make_system() { return std::make_unique<NueSystem>(); }

}  // namespace nue::legacy::platform::nue_backend
