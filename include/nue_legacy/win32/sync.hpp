// Threads and synchronisation: CreateThread / ExitThread / ResumeThread / priorities, events, mutexes, critical sections, WaitForSingleObject and
// WaitForMultipleObjects, TLS, DuplicateHandle / CloseHandle, and the winmm multimedia timers (timeSetEvent / timeKillEvent).
//
// Everything is built on platform::Threads (mutex, condition variable, thread, timer): one global lock and condition variable guard all waitable
// objects, so a wait on several objects is exact (WaitAll acquires all or none) without polling. Guest code runs in callbacks given as std::function;
// this library never sees a guest address.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/win32/handles.hpp"
#include "nue_legacy/platform/clock.hpp"
#include "nue_legacy/platform/threads.hpp"
#include <atomic>
#include <functional>
#include <span>
#include <string_view>

namespace nue::legacy::win32 {

constexpr uint32_t kStillActive = 259;
constexpr uint32_t kTlsOutOfIndexes = 0xFFFFFFFFu;
namespace thread_priority { enum : int32_t { idle = -15, lowest = -2, below_normal = -1, normal = 0, above_normal = 1, highest = 2, time_critical = 15 }; }

struct SyncPolicy {
    // Platform policy, not a game patch: a request above this level is lowered to it (TIME_CRITICAL on translated workers starved the desktop).
    int32_t max_thread_priority = thread_priority::above_normal;
    uint64_t thread_affinity_mask = 0;      // 0 = no restriction
};

using CsId = uint32_t;

class Sync {
public:
    Sync(platform::Threads& threads, platform::Clock& clock, HandleTable& handles, SyncPolicy policy = {});
    ~Sync();

    // ---- events and mutexes. `name` empty = unnamed; a named object that exists is opened (ERROR_ALREADY_EXISTS in `error`, handle valid).
    Ret<Handle> create_event(bool manual_reset, bool initially_signaled, std::string_view name = {});
    Ret<bool> set_event(Handle);
    Ret<bool> reset_event(Handle);
    Ret<bool> pulse_event(Handle);
    Ret<Handle> create_mutex(bool initial_owner, std::string_view name = {});
    Ret<bool> release_mutex(Handle);

    // ---- waits. `milliseconds` kInfinite waits for ever (in slices, so `stopping` interrupts). Results: kWaitObject0 + index, kWaitAbandoned0 + index,
    // kWaitTimeout, or kWaitFailed with the error (ERROR_INVALID_HANDLE, ERROR_OPERATION_ABORTED when the host is stopping).
    Ret<uint32_t> wait_for_single_object(Handle, uint32_t milliseconds);
    Ret<uint32_t> wait_for_multiple_objects(std::span<const Handle>, bool wait_all, uint32_t milliseconds);

    // ---- critical sections: the facade maps a guest CRITICAL_SECTION address to an id.
    CsId cs_create();
    void cs_enter(CsId);
    bool cs_try_enter(CsId);
    void cs_leave(CsId);
    void cs_delete(CsId);

    // ---- threads. The body runs on the new thread and returns the exit code.
    Ret<Handle> create_thread(std::function<uint32_t()> body, bool suspended, uint32_t* thread_id_out);
    Ret<uint32_t> resume_thread(Handle);                  // the previous suspend count, or 0xFFFFFFFF
    Ret<uint32_t> suspend_thread(Handle);                 // not supported on every host: ERROR_CALL_NOT_IMPLEMENTED
    Ret<bool> terminate_thread(Handle, uint32_t exit_code);   // not supported: the thread cannot be stopped from outside (documented)
    Ret<uint32_t> get_exit_code_thread(Handle);           // kStillActive while running
    Ret<bool> set_thread_priority(Handle, int32_t win32_priority);   // the policy cap applies; the capped value is what get_thread_priority returns
    Ret<int32_t> get_thread_priority(Handle);
    uint32_t current_thread_id();
    Handle current_thread_handle();                       // a real handle to the calling thread (what DuplicateHandle(GetCurrentThread()) gives)

    // ---- handles
    bool close_handle(Handle);                            // false: ERROR_INVALID_HANDLE
    Ret<Handle> duplicate_handle(Handle source, bool close_source);   // pseudo handles (current thread) become real ones

    // ---- TLS (64 slots like the original)
    uint32_t tls_alloc();
    bool tls_free(uint32_t index);
    Ret<uint32_t> tls_get_value(uint32_t index);
    Ret<bool> tls_set_value(uint32_t index, uint32_t value);

    // ---- winmm multimedia timers. `callback(id)` runs on a timer thread; callbacks never run concurrently with each other.
    uint32_t time_set_event(uint32_t delay_ms, bool periodic, std::function<void(uint32_t id)> callback);   // 0 on failure
    uint32_t time_kill_event(uint32_t id);               // 0 = TIMERR_NOERROR, 97 = TIMERR_NOCANDO

    // Set by the host when the guest must stop: waits and sleeps come back at once.
    std::atomic<bool> stopping{false};

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nue::legacy::win32
