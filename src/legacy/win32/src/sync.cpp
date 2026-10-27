#include "nue_legacy/win32/sync.hpp"
#include "nue_legacy/platform/storage_path.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>

namespace nue::legacy::win32 {
namespace {

// The Win32 priorities a thread can have, and how they map to the platform's five levels.
platform::ThreadPriority platform_priority(int32_t win32) {
    if (win32 <= thread_priority::lowest) return platform::ThreadPriority::lowest;
    if (win32 < thread_priority::normal) return platform::ThreadPriority::below_normal;
    if (win32 == thread_priority::normal) return platform::ThreadPriority::normal;
    if (win32 == thread_priority::above_normal) return platform::ThreadPriority::above_normal;
    return platform::ThreadPriority::highest;
}
bool valid_priority(int32_t p) { return p == thread_priority::idle || (p >= -2 && p <= 2) || p == thread_priority::time_critical; }

struct Waitable : Object {
    explicit Waitable(ObjectKind k) : Object(k) {}
    // All members below are protected by the global lock of the Sync that owns the object.
    virtual bool can_acquire(uint64_t thread) const = 0;
    virtual bool acquire(uint64_t thread, bool& abandoned) = 0;   // consumes the signal where the object type does
    // PulseEvent: a pulse releases the threads that are blocked on the object when it happens, and only those. A waiter takes a mark when it starts to wait.
    virtual void enter_wait() {}
    virtual void leave_wait() {}
    virtual uint64_t pulse_mark() const { return 0; }
    virtual bool pulse_ready(uint64_t) const { return false; }
    virtual void pulse_take(uint64_t) {}
};

struct EventObject : Waitable {
    EventObject(bool manual_reset, bool initial) : Waitable(ObjectKind::event), manual(manual_reset), signaled(initial) {}
    bool manual, signaled;
    int waiters = 0;
    uint64_t pulses = 0;
    bool pulse_token = false;                   // an auto-reset event releases one waiter per pulse
    bool can_acquire(uint64_t) const override { return signaled; }
    bool acquire(uint64_t, bool&) override { if (!signaled) return false; if (!manual) signaled = false; return true; }
    void enter_wait() override { ++waiters; }
    void leave_wait() override { --waiters; }
    uint64_t pulse_mark() const override { return pulses; }
    bool pulse_ready(uint64_t mark) const override { return pulses != mark && (manual || pulse_token); }
    void pulse_take(uint64_t) override { if (!manual) pulse_token = false; }
};

struct MutexObject : Waitable {
    MutexObject() : Waitable(ObjectKind::mutex) {}
    uint64_t owner = 0;
    uint32_t count = 0;
    bool abandoned = false;
    bool can_acquire(uint64_t thread) const override { return owner == 0 || owner == thread; }
    bool acquire(uint64_t thread, bool& was_abandoned) override {
        if (owner == 0) { owner = thread; count = 1; was_abandoned = abandoned; abandoned = false; return true; }
        if (owner == thread) { ++count; return true; }
        return false;
    }
};

struct ThreadObject : Waitable {
    ThreadObject() : Waitable(ObjectKind::thread) {}
    bool exited = false;
    uint32_t exit_code = kStillActive;
    uint64_t thread_id = 0;
    int32_t priority = thread_priority::normal;
    uint32_t suspend_count = 0;
    platform::ThreadHandle platform_handle;
    platform::SemaphoreHandle gate;                 // a thread created suspended waits on it until ResumeThread
    bool can_acquire(uint64_t) const override { return exited; }
    bool acquire(uint64_t, bool&) override { return exited; }
};

thread_local std::shared_ptr<ThreadObject> t_current;       // the Sync thread object of the calling thread, if it has one
thread_local bool t_in_timer_callback = false;

}  // namespace

struct Sync::Impl {
    Impl(platform::Threads& t, platform::Clock& c, HandleTable& h, SyncPolicy p) : threads(t), clock(c), handles(h), policy(p) {
        global_mutex = threads.mutex_create(false).value;
        global_cond = threads.cond_create().value;
    }
    ~Impl() { threads.cond_destroy(global_cond); threads.mutex_destroy(global_mutex); }

    platform::Threads& threads;
    platform::Clock& clock;
    HandleTable& handles;
    SyncPolicy policy;
    platform::MutexHandle global_mutex;
    platform::CondHandle global_cond;
    std::atomic<bool>* stopping = nullptr;

    struct Guard { Impl& i; explicit Guard(Impl& impl) : i(impl) { i.threads.mutex_lock(i.global_mutex); } ~Guard() { i.threads.mutex_unlock(i.global_mutex); } };
    void broadcast() { threads.cond_broadcast(global_cond); }

    std::map<std::string, std::weak_ptr<Object>> named;       // folded name -> object (events and mutexes share a namespace)
    std::vector<std::weak_ptr<MutexObject>> mutexes;
    std::map<CsId, platform::MutexHandle> sections;
    CsId next_section = 1;
    std::mutex small;                                          // protects `sections`, TLS and timers (never held while waiting)

    // TLS
    platform::TlsHandle tls_slots[64];
    bool tls_used[64] = {};

    // Timers
    struct TimerRecord { platform::TimerHandle handle; bool periodic = false; bool expired = false; };
    std::map<uint32_t, TimerRecord> timers;
    uint32_t next_timer = 1;
    platform::MutexHandle timer_callback_mutex;
    bool timer_mutex_ready = false;

    platform::MutexHandle section(CsId id) {
        std::lock_guard lock(small);
        auto it = sections.find(id);
        return it == sections.end() ? platform::MutexHandle{} : it->second;
    }
    std::shared_ptr<Waitable> waitable(Handle h) {
        if (h == kCurrentThreadPseudoHandle) return current_thread_object();
        auto o = handles.get(h);
        if (!o) return nullptr;
        switch (o->kind) {
        case ObjectKind::event: case ObjectKind::mutex: case ObjectKind::thread: return std::static_pointer_cast<Waitable>(o);
        default: return nullptr;
        }
    }
    std::shared_ptr<ThreadObject> current_thread_object() {
        if (!t_current) {   // a thread this library did not start (the main thread): describe it so handles and priorities work
            auto o = std::make_shared<ThreadObject>();
            o->thread_id = next_thread_id();
            t_current = o;
        }
        return t_current;
    }
    uint64_t me() { return current_thread_object()->thread_id; }   // the id that owns mutexes and that GetCurrentThreadId reports
    static uint32_t next_thread_id() { static std::atomic<uint32_t> next{1000}; return next.fetch_add(4); }

    Ret<uint32_t> wait(const std::vector<std::shared_ptr<Waitable>>& objects, bool all, uint32_t ms) {
        const uint64_t me = this->me();
        Guard lock(*this);
        const int64_t start = clock.time_ns();
        std::vector<uint64_t> marks(objects.size());
        for (size_t i = 0; i < objects.size(); ++i) { marks[i] = objects[i]->pulse_mark(); objects[i]->enter_wait(); }
        struct Leave { const std::vector<std::shared_ptr<Waitable>>& objects; ~Leave() { for (auto& o : objects) o->leave_wait(); } } leave{objects};
        auto ready = [&](size_t i) { return objects[i]->can_acquire(me) || objects[i]->pulse_ready(marks[i]); };
        auto take = [&](size_t i, bool& ab) { if (objects[i]->acquire(me, ab)) return true; if (objects[i]->pulse_ready(marks[i])) { objects[i]->pulse_take(marks[i]); return true; } return false; };
        for (;;) {
            if (all) {
                bool every = true;
                for (size_t i = 0; i < objects.size(); ++i) if (!ready(i)) { every = false; break; }
                if (every) {
                    uint32_t result = kWaitObject0;
                    for (size_t i = 0; i < objects.size(); ++i) { bool ab = false; take(i, ab); if (ab && result == kWaitObject0) result = kWaitAbandoned0 + uint32_t(i); }
                    return ok(result);
                }
            } else {
                for (size_t i = 0; i < objects.size(); ++i) {
                    bool ab = false;
                    if (take(i, ab)) return ok((ab ? kWaitAbandoned0 : kWaitObject0) + uint32_t(i));
                }
            }
            if (ms == 0) return ok(kWaitTimeout);
            if (stopping && stopping->load()) return fail<uint32_t>(err::operation_aborted, kWaitFailed);
            int64_t slice_ns = 100000000;   // 100 ms: the longest the host goes without noticing that it is stopping
            bool last_slice = false;
            if (ms != kInfinite) {
                const int64_t elapsed = (clock.time_ns() - start);
                const int64_t remaining = int64_t(ms) * 1000000 - elapsed;
                if (remaining <= 0) return ok(kWaitTimeout);
                if (remaining <= slice_ns) { slice_ns = remaining; last_slice = true; }
            }
            const platform::Status s = threads.cond_wait(global_cond, global_mutex, slice_ns);
            if (s == platform::Status::timed_out && last_slice) {
                // One last look: the object may have been signalled just as the time ran out.
                if (all) { bool every = true; for (size_t i = 0; i < objects.size(); ++i) if (!ready(i)) { every = false; break; } if (!every) return ok(kWaitTimeout); }
                else { bool any = false; for (size_t i = 0; i < objects.size(); ++i) if (ready(i)) { any = true; break; } if (!any) return ok(kWaitTimeout); }
            }
        }
    }
};

Sync::Sync(platform::Threads& threads, platform::Clock& clock, HandleTable& handles, SyncPolicy policy) : impl_(std::make_unique<Impl>(threads, clock, handles, policy)) { impl_->stopping = &stopping; }
Sync::~Sync() {
    std::lock_guard lock(impl_->small);
    for (auto& t : impl_->timers) { impl_->threads.timer_cancel(t.second.handle); }
    for (auto& t : impl_->timers) { impl_->threads.timer_destroy(t.second.handle); }
    impl_->timers.clear();
    for (auto& s : impl_->sections) impl_->threads.mutex_destroy(s.second);
    for (int i = 0; i < 64; ++i) if (impl_->tls_used[i]) impl_->threads.tls_destroy(impl_->tls_slots[i]);
}

// ------------------------------------------------------------------------------------------------------------------------------ events, mutexes
Ret<Handle> Sync::create_event(bool manual_reset, bool initially_signaled, std::string_view name) {
    Impl::Guard lock(*impl_);
    const std::string key(name);   // object names are case-sensitive in the Win32 namespace
    if (!name.empty()) {
        auto it = impl_->named.find(key);
        if (it != impl_->named.end()) {
            if (auto existing = it->second.lock()) {
                if (existing->kind != ObjectKind::event) return fail<Handle>(err::invalid_handle);
                Handle h = impl_->handles.add(existing);
                return Ret<Handle>{h, err::already_exists};
            }
        }
    }
    auto e = std::make_shared<EventObject>(manual_reset, initially_signaled);
    const Handle h = impl_->handles.add(e);
    if (!h) return fail<Handle>(err::not_enough_memory);
    if (!name.empty()) impl_->named[key] = e;
    return ok(h);
}

Ret<bool> Sync::set_event(Handle h) {
    Impl::Guard lock(*impl_);
    auto e = impl_->handles.get_as<EventObject>(h, ObjectKind::event);
    if (!e) return fail<bool>(err::invalid_handle);
    e->signaled = true;
    impl_->broadcast();
    return ok(true);
}
Ret<bool> Sync::reset_event(Handle h) {
    Impl::Guard lock(*impl_);
    auto e = impl_->handles.get_as<EventObject>(h, ObjectKind::event);
    if (!e) return fail<bool>(err::invalid_handle);
    e->signaled = false;
    return ok(true);
}
Ret<bool> Sync::pulse_event(Handle h) {
    Impl::Guard lock(*impl_);
    auto e = impl_->handles.get_as<EventObject>(h, ObjectKind::event);
    if (!e) return fail<bool>(err::invalid_handle);
    // PulseEvent releases the threads blocked on the event right now (all of them for a manual-reset event, one for an auto-reset event) and leaves it
    // non-signalled. A thread that starts waiting after the call is not released: the wait loop compares the pulse count it saw when it started.
    ++e->pulses;
    e->pulse_token = e->waiters > 0;
    e->signaled = false;
    impl_->broadcast();
    return ok(true);
}

Ret<Handle> Sync::create_mutex(bool initial_owner, std::string_view name) {
    Impl::Guard lock(*impl_);
    const std::string key(name);   // object names are case-sensitive in the Win32 namespace
    if (!name.empty()) {
        auto it = impl_->named.find(key);
        if (it != impl_->named.end()) {
            if (auto existing = it->second.lock()) {
                if (existing->kind != ObjectKind::mutex) return fail<Handle>(err::invalid_handle);
                return Ret<Handle>{impl_->handles.add(existing), err::already_exists};
            }
        }
    }
    auto m = std::make_shared<MutexObject>();
    if (initial_owner) { m->owner = impl_->me(); m->count = 1; }
    const Handle h = impl_->handles.add(m);
    if (!h) return fail<Handle>(err::not_enough_memory);
    impl_->mutexes.push_back(m);
    if (!name.empty()) impl_->named[key] = m;
    return ok(h);
}

Ret<bool> Sync::release_mutex(Handle h) {
    Impl::Guard lock(*impl_);
    auto m = impl_->handles.get_as<MutexObject>(h, ObjectKind::mutex);
    if (!m) return fail<bool>(err::invalid_handle);
    if (m->owner != impl_->me() || m->count == 0) return fail<bool>(err::not_owner);
    if (--m->count == 0) { m->owner = 0; impl_->broadcast(); }
    return ok(true);
}

// ------------------------------------------------------------------------------------------------------------------------------ waits
Ret<uint32_t> Sync::wait_for_single_object(Handle h, uint32_t ms) {
    auto o = impl_->waitable(h);
    if (!o) return fail<uint32_t>(err::invalid_handle, kWaitFailed);
    return impl_->wait({o}, false, ms);
}

Ret<uint32_t> Sync::wait_for_multiple_objects(std::span<const Handle> handles, bool wait_all, uint32_t ms) {
    if (handles.empty() || handles.size() > 64) return fail<uint32_t>(err::invalid_parameter, kWaitFailed);
    std::vector<std::shared_ptr<Waitable>> objects;
    for (Handle h : handles) {
        auto o = impl_->waitable(h);
        if (!o) return fail<uint32_t>(err::invalid_handle, kWaitFailed);
        if (wait_all && std::find(objects.begin(), objects.end(), o) != objects.end()) return fail<uint32_t>(err::invalid_parameter, kWaitFailed);   // duplicates
        objects.push_back(std::move(o));
    }
    return impl_->wait(objects, wait_all, ms);
}

// ------------------------------------------------------------------------------------------------------------------------------ critical sections
CsId Sync::cs_create() {
    auto m = impl_->threads.mutex_create(true);
    std::lock_guard lock(impl_->small);
    const CsId id = impl_->next_section++;
    impl_->sections[id] = m.value;
    return id;
}
void Sync::cs_enter(CsId id) { if (auto m = impl_->section(id)) impl_->threads.mutex_lock(m); }
bool Sync::cs_try_enter(CsId id) { auto m = impl_->section(id); return m && impl_->threads.mutex_trylock(m); }
void Sync::cs_leave(CsId id) { if (auto m = impl_->section(id)) impl_->threads.mutex_unlock(m); }
void Sync::cs_delete(CsId id) {
    platform::MutexHandle m;
    { std::lock_guard lock(impl_->small); auto it = impl_->sections.find(id); if (it == impl_->sections.end()) return; m = it->second; impl_->sections.erase(it); }
    impl_->threads.mutex_destroy(m);
}

// ------------------------------------------------------------------------------------------------------------------------------ threads
Ret<Handle> Sync::create_thread(std::function<uint32_t()> body, bool suspended, uint32_t* thread_id_out) {
    auto object = std::make_shared<ThreadObject>();
    object->suspend_count = suspended ? 1 : 0;
    object->thread_id = Impl::next_thread_id();        // a stable, 4-aligned id of our own: the platform id of the new thread is only known inside it
    const Handle handle = impl_->handles.add(object);
    if (!handle) return fail<Handle>(err::not_enough_memory);
    if (suspended) object->gate = impl_->threads.semaphore_create(0).value;
    Impl* impl = impl_.get();
    platform::ThreadDesc desc;
    desc.name = "guest thread";
    desc.affinity_mask = impl_->policy.thread_affinity_mask;
    auto wrapper = [impl, object, body = std::move(body), suspended]() -> int32_t {
        { Impl::Guard lock(*impl); }                   // the creator holds the lock until the object is complete
        if (suspended) { impl->threads.semaphore_wait(object->gate, -1); impl->threads.semaphore_destroy(object->gate); }
        t_current = object;
        const uint32_t code = body();
        t_current.reset();
        platform::ThreadHandle platform_handle;
        {
            Impl::Guard lock(*impl);
            object->exited = true;
            object->exit_code = code;
            for (auto it = impl->mutexes.begin(); it != impl->mutexes.end();) {   // a thread that dies owning a mutex abandons it
                auto m = it->lock();
                if (!m) { it = impl->mutexes.erase(it); continue; }
                if (m->owner == object->thread_id && m->count) { m->owner = 0; m->count = 0; m->abandoned = true; }
                ++it;
            }
            platform_handle = object->platform_handle;
            impl->broadcast();
        }
        if (platform_handle) impl->threads.detach(platform_handle);
        return int32_t(code);
    };
    impl_->threads.mutex_lock(impl_->global_mutex);
    auto created = impl_->threads.create_thread(desc, std::move(wrapper));
    if (!created) {
        impl_->threads.mutex_unlock(impl_->global_mutex);
        impl_->handles.close(handle);
        return fail<Handle>(err::not_enough_memory);
    }
    object->platform_handle = created.value;
    impl_->threads.set_priority(created.value, platform_priority(object->priority));
    impl_->threads.mutex_unlock(impl_->global_mutex);
    if (thread_id_out) *thread_id_out = uint32_t(object->thread_id);
    return ok(handle);
}

Ret<uint32_t> Sync::resume_thread(Handle h) {
    auto t = impl_->handles.get_as<ThreadObject>(h, ObjectKind::thread);
    if (!t) return fail<uint32_t>(err::invalid_handle, 0xFFFFFFFFu);
    Impl::Guard lock(*impl_);
    const uint32_t previous = t->suspend_count;
    if (t->suspend_count != 0 && --t->suspend_count == 0 && t->gate) impl_->threads.semaphore_post(t->gate);
    return ok(previous);
}

Ret<uint32_t> Sync::suspend_thread(Handle h) {
    if (!impl_->handles.get_as<ThreadObject>(h, ObjectKind::thread)) return fail<uint32_t>(err::invalid_handle, 0xFFFFFFFFu);
    return fail<uint32_t>(err::call_not_implemented, 0xFFFFFFFFu);   // a running thread cannot be suspended through the platform interface
}
Ret<bool> Sync::terminate_thread(Handle h, uint32_t) {
    if (!impl_->handles.get_as<ThreadObject>(h, ObjectKind::thread)) return fail<bool>(err::invalid_handle);
    return fail<bool>(err::call_not_implemented);
}
Ret<uint32_t> Sync::get_exit_code_thread(Handle h) {
    Impl::Guard lock(*impl_);
    auto t = h == kCurrentThreadPseudoHandle ? impl_->current_thread_object() : impl_->handles.get_as<ThreadObject>(h, ObjectKind::thread);
    if (!t) return fail<uint32_t>(err::invalid_handle);
    return ok(t->exited ? t->exit_code : kStillActive);
}
Ret<bool> Sync::set_thread_priority(Handle h, int32_t priority) {
    if (!valid_priority(priority)) return fail<bool>(err::invalid_parameter);
    Impl::Guard lock(*impl_);
    auto t = h == kCurrentThreadPseudoHandle ? impl_->current_thread_object() : impl_->handles.get_as<ThreadObject>(h, ObjectKind::thread);
    if (!t) return fail<bool>(err::invalid_handle);
    const int32_t applied = priority > impl_->policy.max_thread_priority && priority != thread_priority::idle ? impl_->policy.max_thread_priority : priority;
    t->priority = applied;
    if (t->platform_handle && !t->exited) impl_->threads.set_priority(t->platform_handle, platform_priority(applied));
    return ok(true);
}
Ret<int32_t> Sync::get_thread_priority(Handle h) {
    Impl::Guard lock(*impl_);
    auto t = h == kCurrentThreadPseudoHandle ? impl_->current_thread_object() : impl_->handles.get_as<ThreadObject>(h, ObjectKind::thread);
    if (!t) return fail<int32_t>(err::invalid_handle, 0x7FFFFFFF);
    return ok(t->priority);
}
uint32_t Sync::current_thread_id() {
    auto o = impl_->current_thread_object();
    return uint32_t(o->thread_id);
}
Handle Sync::current_thread_handle() {
    auto o = impl_->current_thread_object();
    return impl_->handles.add(o);
}

// ------------------------------------------------------------------------------------------------------------------------------ handles
bool Sync::close_handle(Handle h) { return impl_->handles.close(h); }

Ret<Handle> Sync::duplicate_handle(Handle source, bool close_source) {
    Handle result;
    if (source == kCurrentThreadPseudoHandle) result = impl_->handles.add(impl_->current_thread_object());
    else result = impl_->handles.duplicate(source);
    if (!result) return fail<Handle>(err::invalid_handle);
    if (close_source) impl_->handles.close(source);
    return ok(result);
}

// ------------------------------------------------------------------------------------------------------------------------------ TLS
uint32_t Sync::tls_alloc() {
    std::lock_guard lock(impl_->small);
    for (uint32_t i = 0; i < 64; ++i) {
        if (impl_->tls_used[i]) continue;
        impl_->tls_slots[i] = impl_->threads.tls_create().value;
        impl_->tls_used[i] = true;
        return i;
    }
    return kTlsOutOfIndexes;
}
bool Sync::tls_free(uint32_t index) {
    std::lock_guard lock(impl_->small);
    if (index >= 64 || !impl_->tls_used[index]) return false;
    impl_->threads.tls_destroy(impl_->tls_slots[index]);
    impl_->tls_used[index] = false;
    return true;
}
Ret<uint32_t> Sync::tls_get_value(uint32_t index) {
    platform::TlsHandle slot;
    {
        std::lock_guard lock(impl_->small);
        if (index >= 64) return fail<uint32_t>(err::invalid_parameter);
        if (!impl_->tls_used[index]) return ok(0u);    // a slot that is not allocated reads as 0 with no error, like Windows
        slot = impl_->tls_slots[index];
    }
    return ok(uint32_t(reinterpret_cast<uintptr_t>(impl_->threads.tls_get(slot))));
}
Ret<bool> Sync::tls_set_value(uint32_t index, uint32_t value) {
    platform::TlsHandle slot;
    { std::lock_guard lock(impl_->small); if (index >= 64 || !impl_->tls_used[index]) return fail<bool>(err::invalid_parameter); slot = impl_->tls_slots[index]; }
    impl_->threads.tls_set(slot, reinterpret_cast<void*>(uintptr_t(value)));
    return ok(true);
}

// ------------------------------------------------------------------------------------------------------------------------------ multimedia timers
uint32_t Sync::time_set_event(uint32_t delay_ms, bool periodic, std::function<void(uint32_t)> callback) {
    {
        std::lock_guard lock(impl_->small);
        if (!impl_->timer_mutex_ready) { impl_->timer_callback_mutex = impl_->threads.mutex_create(false).value; impl_->timer_mutex_ready = true; }
    }
    uint32_t id;
    { std::lock_guard lock(impl_->small); id = impl_->next_timer++; }
    Impl* impl = impl_.get();
    auto fn = [impl, id, periodic, callback = std::move(callback)] {
        impl->threads.mutex_lock(impl->timer_callback_mutex);       // callbacks are serialised, as the single multimedia timer thread does
        t_in_timer_callback = true;
        callback(id);
        t_in_timer_callback = false;
        impl->threads.mutex_unlock(impl->timer_callback_mutex);
        if (!periodic) { std::lock_guard lock(impl->small); auto it = impl->timers.find(id); if (it != impl->timers.end()) it->second.expired = true; }
    };
    const int64_t delay_ns = int64_t(delay_ms) * 1000000;
    auto t = impl_->threads.timer_create(delay_ns, periodic ? std::max<int64_t>(delay_ns, 1000000) : 0, std::move(fn));
    if (!t) return 0;
    std::lock_guard lock(impl_->small);
    impl_->timers[id] = {t.value, periodic, false};
    return id;
}

uint32_t Sync::time_kill_event(uint32_t id) {
    platform::TimerHandle handle;
    bool expired;
    {
        std::lock_guard lock(impl_->small);
        auto it = impl_->timers.find(id);
        if (it == impl_->timers.end()) return 97;
        handle = it->second.handle;
        expired = it->second.expired;
        impl_->timers.erase(it);
    }
    if (t_in_timer_callback) impl_->threads.timer_cancel(handle);    // from inside a callback: do not wait for oneself (the handle is released with the Sync)
    else impl_->threads.timer_destroy(handle);                       // like TIME_KILL_SYNCHRONOUS: returns once no callback is running
    return expired ? 97 : 0;
}

}  // namespace nue::legacy::win32
