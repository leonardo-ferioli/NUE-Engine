#include "nue_legacy/win32/guest_threads.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace nue::legacy::win32 {
namespace {
std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%x", v); return b; }
}  // namespace

GuestThreads::GuestThreads(Sync& sync, GuestExecutor& executor, std::function<void(std::string_view)> log) : sync_(sync), executor_(executor), log_(std::move(log)) {}
GuestThreads::~GuestThreads() = default;

Ret<Handle> GuestThreads::create_thread(uint32_t start, uint32_t parameter, uint32_t flags, uint32_t stack_bytes, uint32_t* thread_id) {
    if (sync_.stopping) return fail<Handle>(err::operation_aborted);
    if (!executor_.is_code(start)) throw GuestFault("thread entry is not translated guest code: 0x" + hex(start));
    auto body = [this, start, parameter, stack_bytes]() -> uint32_t {
        std::unique_ptr<GuestContext> context = executor_.create_context(stack_bytes);
        if (!context) return 0xDEAD0002u;
        try {
            const uint32_t args[1] = {parameter};
            return context->call(start, args);
        } catch (const GuestThreadExit& e) {
            return e.code;
        } catch (const GuestFault& f) {
            {
                std::lock_guard lock(m_);
                if (fault_.empty()) fault_ = f.what();
            }
            if (log_) log_(std::string("guest thread fault: ") + f.what());
            sync_.stopping = true;
            return 0xDEAD0001u;
        }
    };
    auto created = sync_.create_thread(std::move(body), (flags & create_thread_flag::suspended) != 0, thread_id);
    if (created) {
        // Keep a handle of our own to wait for the thread at shutdown, even if the guest closes its handle.
        auto copy = sync_.duplicate_handle(created.value, false);
        if (copy.value) { std::lock_guard lock(m_); threads_.push_back(copy.value); }
    }
    return created;
}

size_t GuestThreads::created() const { std::lock_guard lock(m_); return threads_.size(); }
std::string GuestThreads::first_fault() const { std::lock_guard lock(m_); return fault_; }

size_t GuestThreads::shutdown(uint32_t milliseconds) {
    sync_.stopping = true;
    std::vector<Handle> handles;
    { std::lock_guard lock(m_); handles = threads_; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    size_t running = 0;
    for (;;) {
        running = 0;
        for (Handle h : handles) {
            auto code = sync_.get_exit_code_thread(h);
            if (code.value == kStillActive) ++running;
        }
        if (!running || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return running;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------ timers

GuestTimers::GuestTimers(Sync& sync, GuestExecutor& executor, std::function<void(std::string_view)> log) : sync_(sync), executor_(executor), log_(std::move(log)) {}
GuestTimers::~GuestTimers() = default;

uint64_t GuestTimers::callbacks_run() const { std::lock_guard lock(m_); return run_; }

uint32_t GuestTimers::set_event(uint32_t delay_ms, uint32_t, uint32_t callback, uint32_t user, uint32_t flags) {
    if (sync_.stopping) return 0;
    const bool periodic = (flags & timer_flag::periodic) != 0;
    const bool set = (flags & timer_flag::callback_event_set) != 0, pulse = (flags & timer_flag::callback_event_pulse) != 0;
    if (set || pulse) {
        return sync_.time_set_event(delay_ms, periodic, [this, callback, pulse](uint32_t) {
            if (pulse) sync_.pulse_event(callback); else sync_.set_event(callback);
        });
    }
    if (!executor_.is_code(callback)) throw GuestFault("timer callback is not translated guest code: 0x" + hex(callback));
    return sync_.time_set_event(delay_ms, periodic, [this, callback, user](uint32_t id) {
        if (sync_.stopping) return;
        // The callbacks never overlap (Sync serialises them), so the shared context is only ever used by one at a time; the lock guards its creation and the counter.
        GuestContext* context;
        {
            std::lock_guard lock(m_);
            if (!context_) context_ = executor_.create_context(0);
            context = context_.get();
            ++run_;
        }
        if (!context) return;
        try {
            const uint32_t args[5] = {id, 0, user, 0, 0};
            context->call(callback, args);
        } catch (const GuestThreadExit&) {
        } catch (const GuestFault& f) {
            if (log_) log_(std::string("guest timer fault: ") + f.what());
            sync_.stopping = true;
        }
    });
}

uint32_t GuestTimers::kill_event(uint32_t id) { return sync_.time_kill_event(id); }

}  // namespace nue::legacy::win32
