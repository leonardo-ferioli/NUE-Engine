// Threads and synchronisation: scenarios run through the library (on the standard platform backend) and, on Windows, through the real kernel32 with
// every return value and error code compared.
#include "nue_legacy/win32/sync.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

using namespace nue::legacy::win32;
namespace stdlib = nue::legacy::platform::stdlib;

namespace {
// What a scenario can do; implemented once over the library and once over kernel32.
struct Ops {
    virtual ~Ops() = default;
    virtual Handle create_event(bool manual, bool initial, const char* name, uint32_t& error) = 0;
    virtual uint32_t set_event(Handle) = 0;       // returns the error (0 = ok)
    virtual uint32_t reset_event(Handle) = 0;
    virtual Handle create_mutex(bool owner, const char* name, uint32_t& error) = 0;
    virtual uint32_t release_mutex(Handle) = 0;
    virtual uint32_t wait1(Handle, uint32_t ms, uint32_t& error) = 0;
    virtual uint32_t waitn(std::vector<Handle>, bool all, uint32_t ms, uint32_t& error) = 0;
    virtual Handle create_thread(std::function<uint32_t()>, bool suspended, uint32_t& error) = 0;
    virtual uint32_t resume(Handle) = 0;
    virtual uint32_t exit_code(Handle, uint32_t& error) = 0;
    virtual bool close(Handle, uint32_t& error) = 0;
    virtual uint32_t tls_alloc() = 0;
    virtual uint32_t tls_get(uint32_t index, uint32_t& error) = 0;
    virtual bool tls_set(uint32_t index, uint32_t value, uint32_t& error) = 0;
    virtual bool tls_free(uint32_t index, uint32_t& error) = 0;
    virtual bool set_priority(Handle, int32_t, uint32_t& error) = 0;
    virtual int32_t get_priority(Handle, uint32_t& error) = 0;
    virtual Handle current_thread_duplicate(uint32_t& error) = 0;
    virtual Handle duplicate(Handle, bool close_source, uint32_t& error) = 0;
};

#if defined(_WIN32)
// kernel32 behind the same interface: the "handles" the scenario sees are indexes into a table of real HANDLEs.
struct WindowsOps : Ops {
    std::vector<HANDLE> table{nullptr};
    Handle put(HANDLE h) { if (!h) return 0; table.push_back(h); return Handle(table.size() - 1) * 4; }
    HANDLE get(Handle h) { return (h & 3) == 0 && h / 4 < table.size() && h / 4 != 0 ? table[h / 4] : reinterpret_cast<HANDLE>(uintptr_t(0x7ABC1230)); }   // a handle that cannot exist
    Handle create_event(bool manual, bool initial, const char* name, uint32_t& e) override { SetLastError(0); Handle h = put(CreateEventA(nullptr, manual, initial, name)); e = GetLastError(); return h; }
    uint32_t set_event(Handle h) override { return SetEvent(get(h)) ? 0 : GetLastError(); }
    uint32_t reset_event(Handle h) override { return ResetEvent(get(h)) ? 0 : GetLastError(); }
    Handle create_mutex(bool owner, const char* name, uint32_t& e) override { SetLastError(0); Handle h = put(CreateMutexA(nullptr, owner, name)); e = GetLastError(); return h; }
    uint32_t release_mutex(Handle h) override { return ReleaseMutex(get(h)) ? 0 : GetLastError(); }
    uint32_t wait1(Handle h, uint32_t ms, uint32_t& e) override { SetLastError(0); const DWORD r = WaitForSingleObject(get(h), ms); e = r == WAIT_FAILED ? GetLastError() : 0; return r; }
    uint32_t waitn(std::vector<Handle> hs, bool all, uint32_t ms, uint32_t& e) override { std::vector<HANDLE> real; for (Handle h : hs) real.push_back(get(h)); SetLastError(0); const DWORD r = WaitForMultipleObjects(DWORD(real.size()), real.data(), all, ms); e = r == WAIT_FAILED ? GetLastError() : 0; return r; }
    Handle create_thread(std::function<uint32_t()> body, bool suspended, uint32_t& e) override {
        auto* heap = new std::function<uint32_t()>(std::move(body));
        SetLastError(0);
        HANDLE h = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { std::unique_ptr<std::function<uint32_t()>> f(static_cast<std::function<uint32_t()>*>(p)); return (*f)(); }, heap, suspended ? CREATE_SUSPENDED : 0, nullptr);
        e = h ? 0 : GetLastError();
        return put(h);
    }
    uint32_t resume(Handle h) override { return ResumeThread(get(h)); }
    uint32_t exit_code(Handle h, uint32_t& e) override { DWORD c = 0; SetLastError(0); const BOOL ok = GetExitCodeThread(get(h), &c); e = ok ? 0 : GetLastError(); return c; }
    bool close(Handle h, uint32_t& e) override { SetLastError(0); const BOOL ok = CloseHandle(get(h)); e = ok ? 0 : GetLastError(); return ok; }
    uint32_t tls_alloc() override { return TlsAlloc(); }
    uint32_t tls_get(uint32_t i, uint32_t& e) override { SetLastError(77); const uint32_t v = uint32_t(uintptr_t(TlsGetValue(i))); e = GetLastError(); return v; }
    bool tls_set(uint32_t i, uint32_t v, uint32_t& e) override { SetLastError(0); const BOOL ok = TlsSetValue(i, reinterpret_cast<LPVOID>(uintptr_t(v))); e = ok ? 0 : GetLastError(); return ok; }
    bool tls_free(uint32_t i, uint32_t& e) override { SetLastError(0); const BOOL ok = TlsFree(i); e = ok ? 0 : GetLastError(); return ok; }
    bool set_priority(Handle h, int32_t p, uint32_t& e) override { SetLastError(0); const BOOL ok = SetThreadPriority(h == 0xFFFFFFFEu ? GetCurrentThread() : get(h), p); e = ok ? 0 : GetLastError(); return ok; }
    int32_t get_priority(Handle h, uint32_t& e) override { SetLastError(0); const int p = GetThreadPriority(h == 0xFFFFFFFEu ? GetCurrentThread() : get(h)); e = p == THREAD_PRIORITY_ERROR_RETURN ? GetLastError() : 0; return p; }
    Handle current_thread_duplicate(uint32_t& e) override { HANDLE d = nullptr; SetLastError(0); const BOOL ok = DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS); e = ok ? 0 : GetLastError(); return put(d); }
    Handle duplicate(Handle h, bool close_source, uint32_t& e) override {
        HANDLE d = nullptr; SetLastError(0);
        const BOOL ok = DuplicateHandle(GetCurrentProcess(), get(h), GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS | (close_source ? DUPLICATE_CLOSE_SOURCE : 0));
        e = ok ? 0 : GetLastError();
        return put(d);
    }
    ~WindowsOps() override { for (size_t i = 1; i < table.size(); ++i) CloseHandle(table[i]); }
};
#endif

struct LibraryOps : Ops {
    LibraryOps() : threads(stdlib::make_threads()), clock(stdlib::make_clock()), sync(*threads, *clock, handles, [] { SyncPolicy p; p.max_thread_priority = thread_priority::highest; return p; }()) {}
    std::unique_ptr<nue::legacy::platform::Threads> threads;
    std::unique_ptr<nue::legacy::platform::Clock> clock;
    HandleTable handles;
    Sync sync;
    // The scenario sees small stable tokens, like WindowsOps (index * 4), so the two logs can be compared.
    std::vector<Handle> table{0};
    Handle put(Handle h) { if (!h) return 0; table.push_back(h); return Handle(table.size() - 1) * 4; }
    Handle get(Handle h) { return (h & 3) == 0 && h / 4 < table.size() && h / 4 != 0 ? table[h / 4] : Handle(0x7ABC1230); }
    Handle create_event(bool manual, bool initial, const char* name, uint32_t& e) override { auto r = sync.create_event(manual, initial, name ? name : ""); e = r.error; return put(r.value); }
    uint32_t set_event(Handle h) override { return sync.set_event(get(h)).error; }
    uint32_t reset_event(Handle h) override { return sync.reset_event(get(h)).error; }
    Handle create_mutex(bool owner, const char* name, uint32_t& e) override { auto r = sync.create_mutex(owner, name ? name : ""); e = r.error; return put(r.value); }
    uint32_t release_mutex(Handle h) override { return sync.release_mutex(get(h)).error; }
    uint32_t wait1(Handle h, uint32_t ms, uint32_t& e) override { auto r = sync.wait_for_single_object(get(h), ms); e = r.error; return r.value; }
    uint32_t waitn(std::vector<Handle> hs, bool all, uint32_t ms, uint32_t& e) override { std::vector<Handle> real; for (Handle h : hs) real.push_back(get(h)); auto r = sync.wait_for_multiple_objects(real, all, ms); e = r.error; return r.value; }
    Handle create_thread(std::function<uint32_t()> body, bool suspended, uint32_t& e) override { auto r = sync.create_thread(std::move(body), suspended, nullptr); e = r.error; return put(r.value); }
    uint32_t resume(Handle h) override { return sync.resume_thread(get(h)).value; }
    uint32_t exit_code(Handle h, uint32_t& e) override { auto r = sync.get_exit_code_thread(get(h)); e = r.error; return r.value; }
    bool close(Handle h, uint32_t& e) override { const bool ok = sync.close_handle(get(h)); e = ok ? 0 : err::invalid_handle; return ok; }
    uint32_t tls_alloc() override { return sync.tls_alloc(); }
    uint32_t tls_get(uint32_t i, uint32_t& e) override { auto r = sync.tls_get_value(i); e = r.error; return r.value; }
    bool tls_set(uint32_t i, uint32_t v, uint32_t& e) override { auto r = sync.tls_set_value(i, v); e = r.error; return r.value; }
    bool tls_free(uint32_t i, uint32_t& e) override { const bool ok = sync.tls_free(i); e = ok ? 0 : err::invalid_parameter; return ok; }
    bool set_priority(Handle h, int32_t p, uint32_t& e) override { auto r = sync.set_thread_priority(h == 0xFFFFFFFEu ? kCurrentThreadPseudoHandle : get(h), p); e = r.error; return r.value; }
    int32_t get_priority(Handle h, uint32_t& e) override { auto r = sync.get_thread_priority(h == 0xFFFFFFFEu ? kCurrentThreadPseudoHandle : get(h)); e = r.error; return r.value; }
    Handle current_thread_duplicate(uint32_t& e) override { auto r = sync.duplicate_handle(kCurrentThreadPseudoHandle, false); e = r.error; return put(r.value); }
    Handle duplicate(Handle h, bool close_source, uint32_t& e) override { auto r = sync.duplicate_handle(get(h), close_source); e = r.error; return put(r.value); }
};

using Log = std::vector<std::string>;
std::string num(uint32_t v) { return std::to_string(v); }

// The scenarios: each appends the observable results to the log.
Log scenarios(Ops& ops) {
    Log log;
    auto L = [&](const std::string& what, const std::string& value) { log.push_back(what + " = " + value); };
    // Result and error of one call, read in a defined order (operands of + have none).
    auto with_err = [](auto&& call) { uint32_t error = 0; const auto result = call(error); const std::string text = num(uint32_t(result)); return text + " err " + num(error); };
    uint32_t e = 0;
    // 1. Events: auto-reset consumes, manual-reset does not.
    Handle a = ops.create_event(false, true, nullptr, e);
    L("auto create err", num(e));
    L("auto wait0 #1", num(ops.wait1(a, 0, e)));
    L("auto wait0 #2", num(ops.wait1(a, 0, e)));
    ops.set_event(a);
    L("auto set, wait0", num(ops.wait1(a, 0, e)));
    L("auto wait20", num(ops.wait1(a, 20, e)));
    Handle m = ops.create_event(true, false, nullptr, e);
    L("manual wait0 unsignalled", num(ops.wait1(m, 0, e)));
    ops.set_event(m);
    L("manual wait0 #1", num(ops.wait1(m, 0, e)));
    L("manual wait0 #2", num(ops.wait1(m, 0, e)));
    ops.reset_event(m);
    L("manual after reset", num(ops.wait1(m, 0, e)));
    L("set on bad handle err", num(ops.set_event(0x7ABC1230)));
    L("wait on bad handle", with_err([&](uint32_t& er) { return ops.wait1(0x7ABC1230, 0, er); }));
    // 2. Named events: the second create opens the first.
    uint32_t e1 = 0, e2 = 0;
    Handle n1 = ops.create_event(true, false, "nue_sync_test_event", e1);
    Handle n2 = ops.create_event(true, false, "NUE_SYNC_TEST_EVENT", e2);
    L("named first err", num(e1));
    L("named second err", num(e2));
    ops.set_event(n1);
    L("named visible through the other handle", num(ops.wait1(n2, 0, e)));
    // 3. Mutexes: ownership and recursion.
    Handle mx = ops.create_mutex(true, nullptr, e);
    L("mutex release #1", num(ops.release_mutex(mx)));
    L("mutex release #2 (not owner)", num(ops.release_mutex(mx)));
    L("mutex wait (free)", num(ops.wait1(mx, 0, e)));
    L("mutex wait again (recursive)", num(ops.wait1(mx, 0, e)));
    L("mutex release recursive #1", num(ops.release_mutex(mx)));
    L("mutex release recursive #2", num(ops.release_mutex(mx)));
    L("mutex release recursive #3", num(ops.release_mutex(mx)));
    // 4. Several objects.
    Handle e_a = ops.create_event(true, false, nullptr, e), e_b = ops.create_event(true, false, nullptr, e), e_c = ops.create_event(false, false, nullptr, e);
    L("any, none signalled", num(ops.waitn({e_a, e_b, e_c}, false, 0, e)));
    ops.set_event(e_b);
    L("any, second signalled", num(ops.waitn({e_a, e_b, e_c}, false, 0, e)));
    L("all, not yet", num(ops.waitn({e_a, e_b}, true, 0, e)));
    ops.set_event(e_a);
    L("all, both", num(ops.waitn({e_a, e_b}, true, 0, e)));
    ops.set_event(e_c);
    ops.reset_event(e_a);
    L("all with auto-reset, not satisfied leaves it set", num(ops.waitn({e_a, e_c}, true, 0, e)));
    L("auto-reset still set", num(ops.wait1(e_c, 0, e)));
    L("all, duplicate handle", with_err([&](uint32_t& er) { return ops.waitn({e_b, e_b}, true, 0, er); }));
    L("any, duplicate handle", num(ops.waitn({e_b, e_b}, false, 0, e)));
    L("empty list", with_err([&](uint32_t& er) { return ops.waitn({}, false, 0, er); }));
    L("bad handle in list", with_err([&](uint32_t& er) { return ops.waitn({e_a, 0x7ABC1230}, false, 0, er); }));
    // 5. Threads: exit codes, suspended start, waiting for the end.
    Handle t1 = ops.create_thread([] { return 42u; }, false, e);
    L("thread wait", num(ops.wait1(t1, 5000, e)));
    L("thread exit code", num(ops.exit_code(t1, e)));
    Handle gate = ops.create_event(true, false, nullptr, e);
    Handle t2 = ops.create_thread([&ops, gate] { uint32_t err = 0; ops.wait1(gate, 5000, err); return 7u; }, false, e);
    L("running thread exit code", num(ops.exit_code(t2, e)));
    L("running thread wait 10ms", num(ops.wait1(t2, 10, e)));
    ops.set_event(gate);
    L("released thread wait", num(ops.wait1(t2, 5000, e)));
    L("released thread exit code", num(ops.exit_code(t2, e)));
    std::atomic<int> ran{0};
    Handle t3 = ops.create_thread([&ran] { ran = 1; return 3u; }, true, e);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    L("suspended thread has not run", num(uint32_t(ran.load())));
    L("resume returns previous count", num(ops.resume(t3)));
    L("resume again", num(ops.resume(t3)));
    L("suspended thread wait", num(ops.wait1(t3, 5000, e)));
    L("suspended thread ran", num(uint32_t(ran.load())));
    // 6. A thread owning a mutex when it ends abandons it.
    Handle owned = ops.create_mutex(false, nullptr, e);
    Handle t4 = ops.create_thread([&ops, owned] { uint32_t err = 0; ops.wait1(owned, 1000, err); return 0u; }, false, e);
    ops.wait1(t4, 5000, e);
    L("abandoned mutex", num(ops.wait1(owned, 1000, e)));
    L("abandoned mutex taken, released", num(ops.release_mutex(owned)));
    // 7. Cross-thread signalling.
    Handle ping = ops.create_event(false, false, nullptr, e), pong = ops.create_event(false, false, nullptr, e);
    Handle t5 = ops.create_thread([&ops, ping, pong] { uint32_t err = 0; uint32_t sum = 0; for (int i = 0; i < 20; ++i) { sum += ops.wait1(ping, 5000, err); ops.set_event(pong); } return sum; }, false, e);
    uint32_t good = 0;
    for (int i = 0; i < 20; ++i) { ops.set_event(ping); good += ops.wait1(pong, 5000, e) == 0; }
    L("ping-pong rounds", num(good));
    ops.wait1(t5, 5000, e);
    L("ping-pong exit", num(ops.exit_code(t5, e)));
    // 8. TLS.
    const uint32_t slot = ops.tls_alloc();
    L("tls default", with_err([&](uint32_t& er) { return ops.tls_get(slot, er); }));
    L("tls set", num(ops.tls_set(slot, 1234, e)));
    L("tls get", num(ops.tls_get(slot, e)));
    std::atomic<uint32_t> seen_in_thread{99};
    Handle t6 = ops.create_thread([&ops, &seen_in_thread, slot] { uint32_t err = 0; seen_in_thread = ops.tls_get(slot, err); ops.tls_set(slot, 5, err); return 0u; }, false, e);
    ops.wait1(t6, 5000, e);
    L("tls in another thread", num(seen_in_thread));
    L("tls main thread unchanged", num(ops.tls_get(slot, e)));
    L("tls free", num(ops.tls_free(slot, e)));
    L("tls get after free", with_err([&](uint32_t& er) { return ops.tls_get(slot, er); }));
    L("tls free again", with_err([&](uint32_t& er) { return ops.tls_free(slot, er); }));
    L("tls get invalid index", with_err([&](uint32_t& er) { return ops.tls_get(5000, er); }));
    // 9. Handles.
    uint32_t dup_err = 0;
    Handle d = ops.duplicate(a, false, dup_err);
    L("duplicate err", num(dup_err));
    L("close original", num(ops.close(a, e)));
    ops.set_event(d);
    L("duplicate still works", num(ops.wait1(d, 0, e)));
    L("close duplicate", num(ops.close(d, e)));
    L("close duplicate again", with_err([&](uint32_t& er) { return ops.close(d, er); }));
    L("close bad handle", with_err([&](uint32_t& er) { return ops.close(0x7ABC1230, er); }));
    Handle moved = ops.duplicate(m, true, e);
    L("duplicate with close source", num(e));
    L("moved works", with_err([&](uint32_t& er) { return ops.wait1(moved, 0, er); }));
    // 10. Priorities (the library is given a policy that does not cap).
    L("set priority above normal", with_err([&](uint32_t& er) { return ops.set_priority(0xFFFFFFFEu, 1, er); }));
    L("get priority", num(uint32_t(ops.get_priority(0xFFFFFFFEu, e))));
    L("set priority invalid", with_err([&](uint32_t& er) { return ops.set_priority(0xFFFFFFFEu, 5, er); }));
    ops.set_priority(0xFFFFFFFEu, 0, e);
    uint32_t dup_thread_err = 0;
    Handle me = ops.current_thread_duplicate(dup_thread_err);
    L("duplicate current thread", num(dup_thread_err) + " nonzero " + num(me != 0));
    L("exit code of current thread", num(ops.exit_code(me, e)));
    return log;
}
}  // namespace

static void test_sync() {
    LibraryOps ops;
    // Sanity on the library alone (the values that the Windows comparison below also pins down).
    uint32_t e = 0;
    Handle ev = ops.create_event(false, false, nullptr, e);
    CHECK(ev != 0 && e == 0);
    CHECK_EQ(ops.wait1(ev, 0, e), uint32_t(kWaitTimeout));
    ops.set_event(ev);
    CHECK_EQ(ops.wait1(ev, 0, e), 0u);
    // A critical section protects a counter touched by several threads.
    CsId cs = ops.sync.cs_create();
    int counter = 0;
    std::vector<Handle> threads;
    for (int t = 0; t < 4; ++t)
        threads.push_back(ops.create_thread([&ops, cs, &counter] { for (int i = 0; i < 5000; ++i) { ops.sync.cs_enter(cs); ++counter; ops.sync.cs_leave(cs); } return 0u; }, false, e));
    for (Handle t : threads) ops.wait1(t, 10000, e);
    CHECK_EQ(counter, 20000);
    CHECK(ops.sync.cs_try_enter(cs));
    ops.sync.cs_leave(cs);
    ops.sync.cs_delete(cs);
    // The multimedia timer fires periodically until it is killed, and killing an unknown one is refused.
    std::atomic<int> ticks{0};
    const uint32_t timer = ops.sync.time_set_event(10, true, [&ticks](uint32_t) { ++ticks; });
    CHECK(timer != 0);
    for (int waited = 0; ticks.load() < 3 && waited < 3000; ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // a loaded machine is slow, not wrong
    CHECK_EQ(ops.sync.time_kill_event(timer), 0u);
    const int after_kill = ticks.load();
    std::printf("  periodic timer ticks in 120 ms: %d\n", after_kill);
    CHECK(after_kill >= 2);   // about 11 on an idle machine; the bound is low so a loaded one does not fail the test
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK_EQ(ticks.load(), after_kill);
    CHECK(ops.sync.time_kill_event(timer) != 0);
    // The stop flag interrupts a wait that would never end.
    Handle never = ops.create_event(true, false, nullptr, e);
    ops.sync.stopping = true;
    auto r = ops.sync.wait_for_single_object(ops.get(never), kInfinite);
    CHECK(r.value == kWaitFailed && r.error == err::operation_aborted);
    ops.sync.stopping = false;
    // The priority policy caps TIME_CRITICAL.
    nue::legacy::win32::HandleTable handles;
    auto t = stdlib::make_threads();
    auto c = stdlib::make_clock();
    Sync capped(*t, *c, handles);
    CHECK(capped.set_thread_priority(kCurrentThreadPseudoHandle, thread_priority::time_critical).value);
    CHECK_EQ(capped.get_thread_priority(kCurrentThreadPseudoHandle).value, int32_t(thread_priority::above_normal));
}
REGISTER_TEST("sync", test_sync);

#if defined(_WIN32)
static void test_sync_against_windows() {
    WindowsOps real;
    LibraryOps mine;
    const Log expected = scenarios(real), actual = scenarios(mine);
    CHECK_EQ(expected.size(), actual.size());
    int differences = 0;
    for (size_t i = 0; i < (std::min)(expected.size(), actual.size()); ++i) {
        if (expected[i] != actual[i]) { ++differences; std::fprintf(stderr, "windows: %s\nmine:    %s\n", expected[i].c_str(), actual[i].c_str()); }
    }
    CHECK_EQ(differences, 0);
    // winmm timers: periodic count, kill, kill of an unknown id.
    std::atomic<int> win_ticks{0};
    auto cb = [](UINT, UINT, DWORD_PTR user, DWORD_PTR, DWORD_PTR) { ++*reinterpret_cast<std::atomic<int>*>(user); };
    const UINT id = timeSetEvent(10, 1, cb, DWORD_PTR(&win_ticks), TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(timeKillEvent(id), 0u);
    CHECK(win_ticks.load() >= 3);
    const MMRESULT unknown = timeKillEvent(0x7777);
    HandleTable handles;
    auto threads = stdlib::make_threads();
    auto clock = stdlib::make_clock();
    Sync sync(*threads, *clock, handles);
    std::printf("  timeKillEvent(unknown): windows %u, mine %u\n", unknown, sync.time_kill_event(0x7777));
    CHECK_EQ(sync.time_kill_event(0x7777), uint32_t(unknown));
}
REGISTER_TEST("sync_windows", test_sync_against_windows);
#endif
