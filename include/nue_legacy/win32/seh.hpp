// Structured exception handling of a 32-bit guest: RaiseException, RtlUnwind, UnhandledExceptionFilter and SetUnhandledExceptionFilter, as a state machine
// over the guest's registers and memory (the i386 exception registration chain at [TEB + 0], EXCEPTION_RECORD and CONTEXT structures in guest memory).
//
// Nothing here executes guest code. A guest handler is *entered*: the machine writes its arguments on the guest stack, points the guest at the handler and
// returns; the handler's return lands on one of three guest "token" addresses that the execution engine routes to handler_returned(). A handler that never
// returns (an __except block unwinding the stack) just leaves a state behind, which purge() drops once the guest stack has moved past it. So the same code
// serves a translated-code engine, an interpreter or a JIT: the engine only has to implement GuestCpuState for its thread context.
#pragma once
#include "nue_legacy/guest_memory.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace nue::legacy::win32 {

// A fault of the guest program that the host cannot continue from (an unhandled exception, a handler that unbalanced the stack).
struct GuestFault : std::runtime_error { using std::runtime_error::runtime_error; };

struct GuestRegs {                          // the i386 state a handler or a CONTEXT can see
    uint32_t eax = 0, ecx = 0, edx = 0, ebx = 0, esp = 0, ebp = 0, esi = 0, edi = 0, eip = 0;
    uint32_t eflags = 0x202;                // bits CF 0, PF 2, AF 4, ZF 6, SF 7, DF 10, OF 11 are the ones the model keeps
};

// One guest thread's CPU, as the execution engine exposes it.
class GuestCpuState {
public:
    virtual ~GuestCpuState() = default;
    virtual GuestRegs regs() const = 0;
    virtual void set_regs(const GuestRegs&) = 0;     // sets every register including eip: the next instruction the guest executes
    virtual uint32_t teb() const = 0;                // guest address of the thread's TEB; [teb + 0] is the head of the registration chain, [teb + 0x34] the last error
};

// Guest return addresses (not executable code) that the engine hands back to Seh::handler_returned().
struct SehTokens { uint32_t handler_return = 0, unwind_return = 0, filter_return = 0; };

class Seh {
public:
    static constexpr uint32_t kContextSize = 0x2CC, kExceptionRecordSize = 0x50;
    Seh(guest::Memory& memory, SehTokens tokens, std::function<void(std::string_view)> log = {});

    uint32_t set_unhandled_exception_filter(uint32_t filter);       // the previous filter (0 = none)
    uint32_t unhandled_exception_filter() const { return filter_; }

    // What an import did. `completed`: it is finished and returns `value` to its caller (the engine pops the arguments). Otherwise the machine has
    // set the CPU to continue in guest code (a handler, a filter, or the code after the unwind) and the import returns later.
    struct Outcome { bool completed = false; uint32_t value = 0; };
    // `guest_return` is where the import was called from (the address the import would return to); the stack pointer in the CPU is the import's, pointing at that return address.
    Outcome raise_exception(GuestCpuState&, uint32_t guest_return, uint32_t code, uint32_t flags, uint32_t argument_count, uint32_t arguments);
    Outcome rtl_unwind(GuestCpuState&, uint32_t guest_return, uint32_t target_frame, uint32_t target_ip, uint32_t exception_record, uint32_t return_value);
    Outcome unhandled_exception_filter_call(GuestCpuState&, uint32_t guest_return, uint32_t exception_pointers);

    // The CPU reached a token (cpu.regs().eip equals one of the three): eax holds the handler's disposition or the filter's result. Throws GuestFault.
    void handler_returned(GuestCpuState&);
    // To call before the engine runs each guest step of a thread that may have states: drops the states whose stack frame is gone.
    void purge(const GuestCpuState&);
    size_t pending(const GuestCpuState&) const;     // for tests

private:
    struct State {
        enum Kind { raise, unwind, filter } kind = raise;
        bool import_return = false;
        uint32_t pointers = 0, guest_return = 0, resume_sp = 0, call_base = 0, record = 0, context = 0, frame = 0;
        uint32_t target_frame = 0, target_ip = 0, return_value = 0, saved[4] = {};
    };
    uint32_t rd(uint32_t address) const;
    void wr(uint32_t address, uint32_t value) const;
    uint8_t* bytes(uint32_t address, uint32_t size) const;
    void build_context(GuestCpuState&, uint32_t context, uint32_t eip, uint32_t esp);
    void finish_with_context(GuestCpuState&, uint32_t context);
    void call_handler(GuestCpuState&, State&, uint32_t handler, uint32_t frame, uint32_t token);
    void continue_raise(GuestCpuState&);
    void continue_unwind(GuestCpuState&);
    void enter_filter(GuestCpuState&, State&, uint32_t pointers);
    State& top(const GuestCpuState&);
    void push(const GuestCpuState&, const State&);
    void note(std::string_view text) const { if (log_) log_(text); }

    guest::Memory& memory_;
    SehTokens tokens_;
    std::function<void(std::string_view)> log_;
    uint32_t filter_ = 0;
    mutable std::mutex m_;
    std::map<const GuestCpuState*, std::vector<State>> states_;
};

}  // namespace nue::legacy::win32
