// Guest threads and multimedia timers: CreateThread / ExitThread and timeSetEvent / timeKillEvent for a guest whose code runs on an execution engine.
//
// `Sync` already owns the host side (threads with exit codes, suspended start, priority policy, timer thread with serialised callbacks). What it cannot
// know is how to *run guest code*: that is the execution engine's (translated code, interpreter, JIT), and it is reached through the two small interfaces
// below. A GuestContext is one guest thread's CPU and stack; call() runs a guest function to completion on it. With them the thread and timer rules
// (entry must be code, ExitThread, a fault in one thread stops the process, timer callbacks share one context and run one at a time, events as callbacks,
// a timer that kills itself from its own callback) are the same on every host and are tested without any guest code.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/win32/seh.hpp"
#include "nue_legacy/win32/sync.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

namespace nue::legacy::win32 {

// Thrown by the engine out of GuestContext::call() when the guest calls ExitThread (the code is the thread's exit code), and by exit_thread().
struct GuestThreadExit { uint32_t code; };

class GuestContext {
public:
    virtual ~GuestContext() = default;
    // Runs the guest function `function` with the stdcall arguments `args` to completion on this context and returns EAX. Throws GuestThreadExit when the
    // guest calls ExitThread, GuestFault when the guest does something the host cannot continue from.
    virtual uint32_t call(uint32_t function, std::span<const uint32_t> args) = 0;
};

class GuestExecutor {
public:
    virtual ~GuestExecutor() = default;
    // A context with its own stack (`stack_bytes` 0 = the engine's default). Called on the thread that will use it. Null when it cannot be made.
    virtual std::unique_ptr<GuestContext> create_context(uint32_t stack_bytes) = 0;
    // True when guest code can start at `address` (a thread entry or a timer callback outside translated code is a bug of the guest or of the port).
    virtual bool is_code(uint32_t address) = 0;
};

namespace create_thread_flag { enum : uint32_t { suspended = 4 }; }
namespace timer_flag { enum : uint32_t { oneshot = 0, periodic = 1, callback_function = 0, callback_event_set = 0x10, callback_event_pulse = 0x20, kill_synchronous = 0x100 }; }

class GuestThreads {
public:
    GuestThreads(Sync& sync, GuestExecutor& executor, std::function<void(std::string_view)> log = {});
    ~GuestThreads();

    // CreateThread(attributes, stack, start, parameter, flags, &id). The handle is a Level A handle; the thread runs start(parameter) on a context of its own.
    // A creation while the host is stopping fails with ERROR_OPERATION_ABORTED. Throws GuestFault when `start` is not code.
    Ret<Handle> create_thread(uint32_t start, uint32_t parameter, uint32_t flags, uint32_t stack_bytes, uint32_t* thread_id);
    [[noreturn]] void exit_thread(uint32_t code) { throw GuestThreadExit{code}; }

    // The host is stopping: waits and sleeps come back, creations fail. Then waits up to `milliseconds` for the threads that were created; returns how many still run.
    size_t shutdown(uint32_t milliseconds);
    size_t created() const;
    // A fault in a guest thread (GuestFault) is logged, stops the process and ends that thread with exit code 0xDEAD0001; the message is kept for the host to report.
    std::string first_fault() const;

private:
    Sync& sync_;
    GuestExecutor& executor_;
    std::function<void(std::string_view)> log_;
    mutable std::mutex m_;
    std::vector<Handle> threads_;
    std::string fault_;
};

class GuestTimers {
public:
    GuestTimers(Sync& sync, GuestExecutor& executor, std::function<void(std::string_view)> log = {});
    ~GuestTimers();

    // timeSetEvent(delay, resolution, callback, user, flags): `callback` is a guest function (TIME_CALLBACK_FUNCTION: callback(id, 0, user, 0, 0)) or, with
    // TIME_CALLBACK_EVENT_SET / _PULSE, an event handle that is set or pulsed. Returns the timer id, 0 on failure. Throws GuestFault when a function callback
    // is not code. All function callbacks run one at a time on one context.
    uint32_t set_event(uint32_t delay_ms, uint32_t resolution_ms, uint32_t callback, uint32_t user, uint32_t flags);
    // 0 = TIMERR_NOERROR, 97 = TIMERR_NOCANDO. May be called from a timer callback (a one-shot timer ends itself that way).
    uint32_t kill_event(uint32_t id);
    uint64_t callbacks_run() const;

private:
    Sync& sync_;
    GuestExecutor& executor_;
    std::function<void(std::string_view)> log_;
    mutable std::mutex m_;
    std::unique_ptr<GuestContext> context_;          // shared by every function callback: they never overlap
    uint64_t run_ = 0;
};

}  // namespace nue::legacy::win32
