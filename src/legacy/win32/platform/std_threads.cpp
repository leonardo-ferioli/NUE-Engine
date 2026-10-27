#include "nue_legacy/platform/std_platform.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nue::legacy::platform::stdlib {
namespace {

struct ThreadRecord {
    std::thread thread;
    std::atomic<int32_t> exit_code{0};
};

struct MutexRecord {
    std::recursive_mutex plain_recursive;
    std::mutex plain;
    bool recursive = false;
};

struct SemaphoreRecord {
    std::mutex m;
    std::condition_variable cv;
    uint32_t value = 0;
};

struct TimerRecord {
    std::mutex m;
    std::condition_variable cv;
    bool cancelled = false;
    std::thread thread;
    std::function<void()> callback;
};

std::atomic<uint64_t> g_next_thread_id{1};
uint64_t thread_id_of_this() {
    thread_local const uint64_t id = g_next_thread_id.fetch_add(1);
    return id;
}

class StdThreads final : public Threads {
public:
    ~StdThreads() override {   // a thread nobody joined must not terminate the process when the table goes away
        for (auto& entry : threads_) if (entry.second->thread.joinable()) entry.second->thread.detach();
        for (auto& entry : timers_) { { std::lock_guard lock(entry.second->m); entry.second->cancelled = true; } entry.second->cv.notify_all(); if (entry.second->thread.joinable()) entry.second->thread.join(); }
    }
    Result<ThreadHandle> create_thread(const ThreadDesc& desc, std::function<int32_t()> body) override {
        auto record = std::make_shared<ThreadRecord>();
        const uint32_t id = new_id();
        {
            std::lock_guard lock(m_);
            threads_[id] = record;
        }
        record->thread = std::thread([record, body = std::move(body)] { record->exit_code = body(); });
        apply_priority(*record, desc.priority);
        if (desc.affinity_mask) apply_affinity(*record, desc.affinity_mask);
        return {Status::ok, ThreadHandle{id}};
    }
    Status join(ThreadHandle handle, int32_t* exit_code) override {
        auto record = take_thread(handle.value);
        if (!record) return Status::invalid_argument;
        if (record->thread.joinable()) record->thread.join();
        if (exit_code) *exit_code = record->exit_code;
        return Status::ok;
    }
    Status detach(ThreadHandle handle) override {
        auto record = take_thread(handle.value);
        if (!record) return Status::invalid_argument;
        if (record->thread.joinable()) record->thread.detach();   // the thread keeps its record alive through the shared_ptr it captured
        return Status::ok;
    }
    uint64_t current_thread_id() override { return thread_id_of_this(); }
    void yield() override { std::this_thread::yield(); }
    Status set_priority(ThreadHandle handle, ThreadPriority priority) override {
        std::shared_ptr<ThreadRecord> record = find_thread(handle.value);
        if (!record) return Status::invalid_argument;
        return apply_priority(*record, priority);
    }
    Status set_affinity(ThreadHandle handle, uint64_t mask) override {
        std::shared_ptr<ThreadRecord> record = find_thread(handle.value);
        if (!record) return Status::invalid_argument;
        return apply_affinity(*record, mask);
    }

    Result<MutexHandle> mutex_create(bool recursive) override {
        auto record = std::make_shared<MutexRecord>();
        record->recursive = recursive;
        const uint32_t id = new_id();
        std::lock_guard lock(m_);
        mutexes_[id] = record;
        return {Status::ok, MutexHandle{id}};
    }
    void mutex_destroy(MutexHandle h) override { std::lock_guard lock(m_); mutexes_.erase(h.value); }
    void mutex_lock(MutexHandle h) override { if (auto r = mutex_of(h.value)) { if (r->recursive) r->plain_recursive.lock(); else r->plain.lock(); } }
    bool mutex_trylock(MutexHandle h) override { auto r = mutex_of(h.value); return r && (r->recursive ? r->plain_recursive.try_lock() : r->plain.try_lock()); }
    void mutex_unlock(MutexHandle h) override { if (auto r = mutex_of(h.value)) { if (r->recursive) r->plain_recursive.unlock(); else r->plain.unlock(); } }

    Result<CondHandle> cond_create() override {
        auto record = std::make_shared<std::condition_variable_any>();
        const uint32_t id = new_id();
        std::lock_guard lock(m_);
        conds_[id] = record;
        return {Status::ok, CondHandle{id}};
    }
    void cond_destroy(CondHandle h) override { std::lock_guard lock(m_); conds_.erase(h.value); }
    Status cond_wait(CondHandle c, MutexHandle mh, int64_t timeout_ns) override {
        std::shared_ptr<std::condition_variable_any> cv;
        std::shared_ptr<MutexRecord> mx;
        {
            std::lock_guard lock(m_);
            auto ci = conds_.find(c.value);
            auto mi = mutexes_.find(mh.value);
            if (ci == conds_.end() || mi == mutexes_.end()) return Status::invalid_argument;
            cv = ci->second;
            mx = mi->second;
        }
        auto wait = [&](auto& lockable) {
            if (timeout_ns < 0) { cv->wait(lockable); return Status::ok; }
            return cv->wait_for(lockable, std::chrono::nanoseconds(timeout_ns)) == std::cv_status::timeout ? Status::timed_out : Status::ok;
        };
        return mx->recursive ? wait(mx->plain_recursive) : wait(mx->plain);
    }
    void cond_signal(CondHandle h) override { if (auto c = cond_of(h.value)) c->notify_one(); }
    void cond_broadcast(CondHandle h) override { if (auto c = cond_of(h.value)) c->notify_all(); }

    Result<SemaphoreHandle> semaphore_create(uint32_t initial) override {
        auto record = std::make_shared<SemaphoreRecord>();
        record->value = initial;
        const uint32_t id = new_id();
        std::lock_guard lock(m_);
        semaphores_[id] = record;
        return {Status::ok, SemaphoreHandle{id}};
    }
    void semaphore_destroy(SemaphoreHandle h) override { std::lock_guard lock(m_); semaphores_.erase(h.value); }
    Status semaphore_wait(SemaphoreHandle h, int64_t timeout_ns) override {
        std::shared_ptr<SemaphoreRecord> s;
        {
            std::lock_guard lock(m_);
            auto it = semaphores_.find(h.value);
            if (it == semaphores_.end()) return Status::invalid_argument;
            s = it->second;
        }
        std::unique_lock lock(s->m);
        auto ready = [&] { return s->value > 0; };
        if (timeout_ns < 0) s->cv.wait(lock, ready);
        else if (!s->cv.wait_for(lock, std::chrono::nanoseconds(timeout_ns), ready)) return Status::timed_out;
        --s->value;
        return Status::ok;
    }
    void semaphore_post(SemaphoreHandle h) override {
        std::shared_ptr<SemaphoreRecord> s;
        {
            std::lock_guard lock(m_);
            auto it = semaphores_.find(h.value);
            if (it == semaphores_.end()) return;
            s = it->second;
        }
        { std::lock_guard lock(s->m); ++s->value; }
        s->cv.notify_one();
    }

    Result<TlsHandle> tls_create() override { return {Status::ok, TlsHandle{new_id()}}; }
    void tls_destroy(TlsHandle h) override { values().erase(h.value); }
    void* tls_get(TlsHandle h) override { auto& v = values(); auto it = v.find(h.value); return it == v.end() ? nullptr : it->second; }
    Status tls_set(TlsHandle h, void* value) override { values()[h.value] = value; return Status::ok; }

    Result<TimerHandle> timer_create(int64_t delay_ns, int64_t period_ns, std::function<void()> callback) override {
        auto record = std::make_shared<TimerRecord>();
        record->callback = std::move(callback);
        const uint32_t id = new_id();
        TimerRecord* raw = record.get();
        record->thread = std::thread([raw, delay_ns, period_ns] {
            std::unique_lock lock(raw->m);
            auto wait = [&](int64_t ns) { return !raw->cv.wait_for(lock, std::chrono::nanoseconds(ns), [&] { return raw->cancelled; }); };
            if (!wait(delay_ns)) return;
            for (;;) {
                lock.unlock();
                raw->callback();
                lock.lock();
                if (raw->cancelled || period_ns <= 0) return;
                if (!wait(period_ns)) return;
            }
        });
        std::lock_guard lock(m_);
        timers_[id] = record;
        return {Status::ok, TimerHandle{id}};
    }
    void timer_cancel(TimerHandle h) override {
        if (auto t = timer_of(h.value)) { { std::lock_guard lock(t->m); t->cancelled = true; } t->cv.notify_all(); }
    }
    void timer_destroy(TimerHandle h) override {
        std::shared_ptr<TimerRecord> t;
        {
            std::lock_guard lock(m_);
            auto it = timers_.find(h.value);
            if (it == timers_.end()) return;
            t = it->second;
            timers_.erase(it);
        }
        { std::lock_guard lock(t->m); t->cancelled = true; }
        t->cv.notify_all();
        if (t->thread.joinable() && t->thread.get_id() != std::this_thread::get_id()) t->thread.join();
    }

private:
    uint32_t new_id() { return next_.fetch_add(1); }
    std::shared_ptr<ThreadRecord> find_thread(uint32_t id) { std::lock_guard lock(m_); auto it = threads_.find(id); return it == threads_.end() ? nullptr : it->second; }
    std::shared_ptr<ThreadRecord> take_thread(uint32_t id) { std::lock_guard lock(m_); auto it = threads_.find(id); if (it == threads_.end()) return nullptr; auto r = it->second; threads_.erase(it); return r; }
    std::shared_ptr<MutexRecord> mutex_of(uint32_t id) { std::lock_guard lock(m_); auto it = mutexes_.find(id); return it == mutexes_.end() ? nullptr : it->second; }
    std::shared_ptr<std::condition_variable_any> cond_of(uint32_t id) { std::lock_guard lock(m_); auto it = conds_.find(id); return it == conds_.end() ? nullptr : it->second; }
    std::shared_ptr<TimerRecord> timer_of(uint32_t id) { std::lock_guard lock(m_); auto it = timers_.find(id); return it == timers_.end() ? nullptr : it->second; }
    static std::unordered_map<uint32_t, void*>& values() { thread_local std::unordered_map<uint32_t, void*> v; return v; }

    static Status apply_priority(ThreadRecord& record, ThreadPriority priority) {
#if defined(_WIN32)
        static const int levels[] = {THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST};
        return SetThreadPriority(HANDLE(record.thread.native_handle()), levels[int(priority) + 2]) ? Status::ok : Status::unsupported;
#else
        (void)record; (void)priority;
        return Status::unsupported;
#endif
    }
    static Status apply_affinity(ThreadRecord& record, uint64_t mask) {
#if defined(_WIN32)
        return SetThreadAffinityMask(HANDLE(record.thread.native_handle()), DWORD_PTR(mask)) ? Status::ok : Status::unsupported;
#else
        (void)record; (void)mask;
        return Status::unsupported;
#endif
    }

    std::mutex m_;
    std::atomic<uint32_t> next_{1};
    std::map<uint32_t, std::shared_ptr<ThreadRecord>> threads_;
    std::map<uint32_t, std::shared_ptr<MutexRecord>> mutexes_;
    std::map<uint32_t, std::shared_ptr<std::condition_variable_any>> conds_;
    std::map<uint32_t, std::shared_ptr<SemaphoreRecord>> semaphores_;
    std::map<uint32_t, std::shared_ptr<TimerRecord>> timers_;
};
}  // namespace

std::unique_ptr<Threads> make_threads() { return std::make_unique<StdThreads>(); }
}  // namespace nue::legacy::platform::stdlib
