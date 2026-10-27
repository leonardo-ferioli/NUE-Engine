// Guest threads and timers over a fake execution engine: the "guest functions" are C++ lambdas registered at addresses, a context runs them.
#include "nue_legacy/win32/guest_threads.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <string>

using namespace nue::legacy::win32;
namespace stdlib = nue::legacy::platform::stdlib;

namespace {
class FakeExecutor final : public GuestExecutor {
public:
    using Function = std::function<uint32_t(std::span<const uint32_t>)>;
    void define(uint32_t address, Function f) { functions_[address] = std::move(f); }
    std::atomic<int> contexts{0};
    std::unique_ptr<GuestContext> create_context(uint32_t) override { ++contexts; return std::make_unique<Context>(*this); }
    bool is_code(uint32_t address) override { return functions_.count(address) != 0; }
private:
    struct Context final : GuestContext {
        explicit Context(FakeExecutor& e) : executor(e) {}
        uint32_t call(uint32_t function, std::span<const uint32_t> args) override { return executor.functions_.at(function)(args); }
        FakeExecutor& executor;
    };
    std::map<uint32_t, Function> functions_;
};

struct Rig {
    HandleTable handles{0x00C00000u};
    std::unique_ptr<nue::legacy::platform::Threads> threads = stdlib::make_threads();
    std::unique_ptr<nue::legacy::platform::Clock> clock = stdlib::make_clock();
    Sync sync{*threads, *clock, handles};
    FakeExecutor executor;
    std::string log;
    GuestThreads guest_threads{sync, executor, [this](std::string_view s) { log += s; log += '\n'; }};
    GuestTimers timers{sync, executor, [this](std::string_view s) { log += s; log += '\n'; }};
};

bool wait_until(const std::function<bool()>& done, int milliseconds = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
}  // namespace

static void test_guest_threads() {
    // ---- a thread runs start(parameter) and its exit code is the function's result
    {
        Rig rig;
        std::atomic<uint32_t> seen{0};
        rig.executor.define(0x401000, [&](std::span<const uint32_t> a) { seen = a[0]; return a[0] + 1; });
        uint32_t id = 0;
        auto h = rig.guest_threads.create_thread(0x401000, 41, 0, 0, &id);
        CHECK(h && h.value >= 0x00C00000u && id != 0);
        CHECK(wait_until([&] { return rig.sync.get_exit_code_thread(h.value).value != kStillActive; }));
        CHECK_EQ(rig.sync.get_exit_code_thread(h.value).value, 42u);
        CHECK_EQ(seen.load(), 41u);
        CHECK_EQ(rig.guest_threads.created(), 1u);
    }
    // ---- ExitThread from guest code (the engine throws GuestThreadExit)
    {
        Rig rig;
        rig.executor.define(0x401000, [&](std::span<const uint32_t>) -> uint32_t { throw GuestThreadExit{7}; });
        auto h = rig.guest_threads.create_thread(0x401000, 0, 0, 0, nullptr);
        CHECK(wait_until([&] { return rig.sync.get_exit_code_thread(h.value).value != kStillActive; }));
        CHECK_EQ(rig.sync.get_exit_code_thread(h.value).value, 7u);
    }
    // ---- suspended start
    {
        Rig rig;
        std::atomic<int> ran{0};
        rig.executor.define(0x401000, [&](std::span<const uint32_t>) { ++ran; return 0u; });
        auto h = rig.guest_threads.create_thread(0x401000, 0, create_thread_flag::suspended, 0, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK_EQ(ran.load(), 0);
        CHECK_EQ(rig.sync.resume_thread(h.value).value, 1u);
        CHECK(wait_until([&] { return ran.load() == 1; }));
    }
    // ---- an entry that is not code is a fault of the guest (or of the port), not an error code
    {
        Rig rig;
        bool faulted = false;
        try { rig.guest_threads.create_thread(0x999999, 0, 0, 0, nullptr); } catch (const GuestFault&) { faulted = true; }
        CHECK(faulted);
    }
    // ---- a fault inside a thread stops the process and is reported
    {
        Rig rig;
        rig.executor.define(0x401000, [&](std::span<const uint32_t>) -> uint32_t { throw GuestFault("unhandled guest exception code=0xc0000005"); });
        auto h = rig.guest_threads.create_thread(0x401000, 0, 0, 0, nullptr);
        CHECK(wait_until([&] { return rig.sync.get_exit_code_thread(h.value).value != kStillActive; }));
        CHECK_EQ(rig.sync.get_exit_code_thread(h.value).value, 0xDEAD0001u);
        CHECK(rig.sync.stopping.load());
        CHECK(rig.guest_threads.first_fault().find("0xc0000005") != std::string::npos);
        CHECK(rig.log.find("guest thread fault") != std::string::npos);
        auto after = rig.guest_threads.create_thread(0x401000, 0, 0, 0, nullptr);       // the host is stopping now
        CHECK(!after && after.error == err::operation_aborted);
    }
    // ---- shutdown waits for the threads that were created; a stuck one is reported
    {
        Rig rig;
        std::atomic<bool> release{false};
        rig.executor.define(0x401000, [&](std::span<const uint32_t>) { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0u; });
        rig.guest_threads.create_thread(0x401000, 0, 0, 0, nullptr);
        CHECK_EQ(rig.guest_threads.shutdown(20), 1u);
        release = true;
        CHECK_EQ(rig.guest_threads.shutdown(2000), 0u);
    }
}
REGISTER_TEST("guest_threads", test_guest_threads);

static void test_guest_timers() {
    // ---- a periodic timer calls the guest function with (id, 0, user, 0, 0); a kill stops it; all callbacks share one context
    {
        Rig rig;
        std::atomic<int> calls{0};
        std::atomic<uint32_t> last_id{0}, last_user{0};
        std::atomic<bool> arguments_ok{true};
        rig.executor.define(0x402000, [&](std::span<const uint32_t> a) {
            arguments_ok = arguments_ok && a.size() == 5 && a[1] == 0 && a[3] == 0 && a[4] == 0;
            last_id = a[0]; last_user = a[2]; ++calls;
            return 0u;
        });
        const uint32_t id = rig.timers.set_event(5, 1, 0x402000, 0xABCD, timer_flag::periodic);
        CHECK(id != 0);
        CHECK(wait_until([&] { return calls.load() >= 3; }));
        CHECK_EQ(rig.timers.kill_event(id), 0u);
        const int after_kill = calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK(calls.load() <= after_kill + 1);
        CHECK(arguments_ok.load() && last_id.load() == id && last_user.load() == 0xABCDu);
        CHECK_EQ(rig.executor.contexts.load(), 1);                                       // one shared context for the callbacks
        CHECK_EQ(rig.timers.kill_event(0x7777), 97u);                                     // TIMERR_NOCANDO
        CHECK(rig.timers.callbacks_run() >= 3);
    }
    // ---- a one-shot fires once
    {
        Rig rig;
        std::atomic<int> calls{0};
        rig.executor.define(0x402000, [&](std::span<const uint32_t>) { ++calls; return 0u; });
        CHECK(rig.timers.set_event(5, 1, 0x402000, 0, timer_flag::oneshot) != 0);
        CHECK(wait_until([&] { return calls.load() == 1; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK_EQ(calls.load(), 1);
    }
    // ---- a timer kills itself from its own callback (a periodic timer that decides it is done)
    {
        Rig rig;
        std::atomic<int> calls{0};
        std::atomic<uint32_t> own_id{0};
        std::atomic<uint32_t> kill_result{99};
        rig.executor.define(0x402000, [&](std::span<const uint32_t> a) {
            if (++calls == 2) kill_result = rig.timers.kill_event(a[0]);
            return 0u;
        });
        own_id = rig.timers.set_event(4, 1, 0x402000, 0, timer_flag::periodic);
        CHECK(wait_until([&] { return kill_result.load() != 99; }));
        const int at_kill = calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK(calls.load() <= at_kill + 1);
        CHECK_EQ(kill_result.load(), 0u);
    }
    // ---- event callbacks: TIME_CALLBACK_EVENT_SET sets the event, _PULSE releases a waiter
    {
        Rig rig;
        auto ev = rig.sync.create_event(true, false);
        CHECK(rig.timers.set_event(5, 1, ev.value, 0, timer_flag::callback_event_set) != 0);
        CHECK_EQ(rig.sync.wait_for_single_object(ev.value, 2000).value, kWaitObject0);
        auto pulse = rig.sync.create_event(false, false);
        const uint32_t id = rig.timers.set_event(5, 1, pulse.value, 0, timer_flag::callback_event_pulse | timer_flag::periodic);
        CHECK(id != 0);
        CHECK_EQ(rig.sync.wait_for_single_object(pulse.value, 2000).value, kWaitObject0);
        rig.timers.kill_event(id);
    }
    // ---- PulseEvent: releases the threads blocked at that moment (all of them on a manual-reset event), and only those
    {
        Rig rig;
        auto manual = rig.sync.create_event(true, false);
        std::atomic<int> released{0};
        auto waiter = [&] { if (rig.sync.wait_for_single_object(manual.value, 3000).value == kWaitObject0) ++released; };
        std::thread a(waiter), b(waiter);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));                      // both are blocked now (a pulse reaches only threads already blocked: there is no way to observe that, so the pause is generous)
        rig.sync.pulse_event(manual.value);
        a.join(); b.join();
        CHECK_EQ(released.load(), 2);
        CHECK_EQ(rig.sync.wait_for_single_object(manual.value, 30).value, kWaitTimeout);  // left non-signalled; a late waiter is not released
        auto autoev = rig.sync.create_event(false, false);
        std::atomic<int> one{0};
        auto auto_waiter = [&] { if (rig.sync.wait_for_single_object(autoev.value, 1500).value == kWaitObject0) ++one; };
        std::thread c(auto_waiter), d(auto_waiter);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        rig.sync.pulse_event(autoev.value);
        c.join(); d.join();
        CHECK_EQ(one.load(), 1);                                                          // an auto-reset event releases one waiter per pulse
    }
    // ---- a callback that is not code is a fault; a fault in a callback stops the process; no timers once stopping
    {
        Rig rig;
        bool faulted = false;
        try { rig.timers.set_event(5, 1, 0x999999, 0, 0); } catch (const GuestFault&) { faulted = true; }
        CHECK(faulted);
        rig.executor.define(0x402000, [&](std::span<const uint32_t>) -> uint32_t { throw GuestFault("bad"); });
        rig.timers.set_event(3, 1, 0x402000, 0, timer_flag::oneshot);
        CHECK(wait_until([&] { return rig.sync.stopping.load(); }));
        CHECK_EQ(rig.timers.set_event(5, 1, 0x402000, 0, 0), 0u);
    }
}
REGISTER_TEST("guest_timers", test_guest_timers);
