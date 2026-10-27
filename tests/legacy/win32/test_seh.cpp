// Structured exception handling: the state machine driven by a fake guest CPU and a guest memory. The test plays the part of the execution engine: it reads
// where the machine sent the guest, checks the arguments of the handler on the guest stack, and "returns" from the handler to a token with a disposition.
#include "nue_legacy/win32/arena_memory.hpp"
#include "nue_legacy/win32/seh.hpp"
#include "test_support.hpp"
#include <cstring>

using namespace nue::legacy::win32;

namespace {
constexpr SehTokens kTokens{0xF0FFFF00u, 0xF0FFFF10u, 0xF0FFFF20u};

struct FakeCpu final : GuestCpuState {
    GuestRegs r;
    uint32_t teb_address = 0;
    GuestRegs regs() const override { return r; }
    void set_regs(const GuestRegs& v) override { r = v; }
    uint32_t teb() const override { return teb_address; }
};

struct World {
    ArenaMemory memory{64u << 20, 16u << 20};
    FakeCpu cpu;
    Seh seh{memory, kTokens};
    uint32_t stack_top = 0, teb = 0;
    World() {
        const uint32_t stack = memory.alloc(1u << 20);
        stack_top = stack + (1u << 20) - 0x100;
        teb = memory.alloc(0x1000);
        cpu.teb_address = teb;
        cpu.r.esp = stack_top;
        cpu.r.eip = 0x401000;
        w(teb, 0xFFFFFFFFu);
    }
    void w(uint32_t a, uint32_t v) { std::memcpy(memory.host(a, 4), &v, 4); }
    uint32_t r(uint32_t a) { uint32_t v; std::memcpy(&v, memory.host(a, 4), 4); return v; }
    // A registration frame in guest memory below the stack top: {next, handler}; it becomes the head of the chain.
    uint32_t frame(uint32_t slot, uint32_t handler) {
        const uint32_t address = stack_top - 0x2000 - slot * 0x20;
        w(address, r(teb)); w(address + 4, handler);
        w(teb, address);
        return address;
    }
    // The import is called: the return address is on the stack, then the arguments.
    void call_import(uint32_t return_address) { cpu.r.esp -= 4; w(cpu.r.esp, return_address); cpu.r.esp -= 0x20; cpu.r.esp += 0x20; }
    // The guest handler returns to the token it was entered with (stdcall: 4 arguments popped).
    void handler_returns(uint32_t disposition, unsigned popped_words = 4) {
        const uint32_t token = r(cpu.r.esp);
        cpu.r.esp += 4 + 4 * popped_words;
        cpu.r.eip = token;
        cpu.r.eax = disposition;
        seh.handler_returned(cpu);
    }
};

template <class F> bool faults(F&& f) { try { f(); } catch (const GuestFault&) { return true; } return false; }
}  // namespace

static void test_seh() {
    // ---- RaiseException walks the chain; ContinueSearch goes to the next frame, ContinueExecution resumes at the context
    {
        World w;
        const uint32_t outer = w.frame(1, 0x500000), inner = w.frame(0, 0x500100);
        w.cpu.r.ebx = 0x11; w.cpu.r.esi = 0x22; w.cpu.r.edi = 0x33; w.cpu.r.ebp = 0x44;
        w.call_import(0x402000);
        const uint32_t args = w.memory.alloc(16);
        w.w(args, 0xAAAA); w.w(args + 4, 0xBBBB);
        const uint32_t sp_in_import = w.cpu.r.esp;
        const auto out = w.seh.raise_exception(w.cpu, 0x402000, 0xE0000001u, 0, 2, args);
        CHECK(!out.completed);
        CHECK_EQ(w.cpu.r.eip, 0x500100u);                                              // the innermost frame's handler
        const uint32_t sp = w.cpu.r.esp, record = w.r(sp + 4), context = w.r(sp + 12);
        CHECK_EQ(w.r(sp), kTokens.handler_return);
        CHECK_EQ(w.r(sp + 8), inner);
        CHECK_EQ(w.r(record), 0xE0000001u);                                            // ExceptionCode
        CHECK_EQ(w.r(record + 4), 0u);                                                 // continuable
        CHECK_EQ(w.r(record + 12), 0x402000u);                                         // ExceptionAddress
        CHECK_EQ(w.r(record + 16), 2u);                                                // NumberParameters
        CHECK(w.r(record + 20) == 0xAAAA && w.r(record + 24) == 0xBBBB);
        CHECK(w.r(context) == 0x10007 && w.r(context + 0xB8) == 0x402000 && w.r(context + 0xA4) == 0x11 && w.r(context + 0xA0) == 0x22);
        CHECK_EQ(w.seh.pending(w.cpu), 1u);
        w.handler_returns(1);                                                          // ExceptionContinueSearch
        CHECK_EQ(w.cpu.r.eip, 0x500000u);                                              // the outer frame
        CHECK_EQ(w.r(w.cpu.r.esp + 8), outer);
        // The handler changes the context before resuming: that is how a handler "fixes" a register.
        const uint32_t ctx2 = w.r(w.cpu.r.esp + 12);
        w.w(ctx2 + 0xB0, 0xCAFE);                                                      // EAX
        w.handler_returns(0);                                                          // ExceptionContinueExecution
        CHECK_EQ(w.cpu.r.eip, 0x402000u);
        CHECK_EQ(w.cpu.r.eax, 0xCAFEu);
        CHECK_EQ(w.cpu.r.esp, sp_in_import + 4 + 16);                                  // resumes as if the import returned
        CHECK(w.cpu.r.ebx == 0x11 && w.cpu.r.esi == 0x22 && w.cpu.r.edi == 0x33 && w.cpu.r.ebp == 0x44);
        CHECK_EQ(w.seh.pending(w.cpu), 0u);
    }
    // ---- non-continuable, bad dispositions, an unbalanced stack, the end of the chain
    {
        World w;
        w.frame(0, 0x500100);
        w.call_import(0x402000);
        w.seh.raise_exception(w.cpu, 0x402000, 0xC0000005u, 1, 0, 0);                  // EXCEPTION_NONCONTINUABLE
        CHECK(faults([&] { w.handler_returns(0); }));
    }
    {
        World w;
        w.frame(0, 0x500100);
        w.call_import(0x402000);
        w.seh.raise_exception(w.cpu, 0x402000, 1, 0, 0, 0);
        CHECK(faults([&] { w.handler_returns(7); }));                                  // not a disposition
    }
    {
        World w;
        w.frame(0, 0x500100);
        w.call_import(0x402000);
        w.seh.raise_exception(w.cpu, 0x402000, 1, 0, 0, 0);
        w.cpu.r.esp += 4 + 8;                                                          // the handler popped too little
        w.cpu.r.eip = kTokens.handler_return;
        CHECK(faults([&] { w.seh.handler_returned(w.cpu); }));
    }
    {
        World w;                                                                       // no frames, no filter: fatal
        w.call_import(0x402000);
        CHECK(faults([&] { w.seh.raise_exception(w.cpu, 0x402000, 0xE0000002u, 0, 0, 0); }));
    }
    // ---- the top-level filter: a chain that ends decides through it; -1 resumes, anything else is a fault
    {
        World w;
        w.seh.set_unhandled_exception_filter(0x600000);
        w.call_import(0x402000);
        const uint32_t sp_in_import = w.cpu.r.esp;
        w.seh.raise_exception(w.cpu, 0x402000, 0xE0000003u, 0, 0, 0);
        CHECK_EQ(w.cpu.r.eip, 0x600000u);                                              // the filter is entered with EXCEPTION_POINTERS
        const uint32_t pointers = w.r(w.cpu.r.esp + 4);
        CHECK_EQ(w.r(w.r(pointers)), 0xE0000003u);
        w.handler_returns(0xFFFFFFFFu, 1);                                             // EXCEPTION_CONTINUE_EXECUTION
        CHECK_EQ(w.cpu.r.eip, 0x402000u);
        CHECK_EQ(w.cpu.r.esp, sp_in_import + 4 + 16);
    }
    {
        World w;
        w.seh.set_unhandled_exception_filter(0x600000);
        w.call_import(0x402000);
        w.seh.raise_exception(w.cpu, 0x402000, 1, 0, 0, 0);
        CHECK(faults([&] { w.handler_returns(1, 1); }));                               // EXCEPTION_EXECUTE_HANDLER: the process would end
    }
    {
        World w;
        CHECK_EQ(w.seh.set_unhandled_exception_filter(0x600000), 0u);
        CHECK_EQ(w.seh.set_unhandled_exception_filter(0x700000), 0x600000u);           // returns the previous one
        CHECK_EQ(w.seh.unhandled_exception_filter(), 0x700000u);
    }
    // ---- UnhandledExceptionFilter called as an import
    {
        World w;
        w.call_import(0x402000);
        const auto out = w.seh.unhandled_exception_filter_call(w.cpu, 0x402000, 0x1234);
        CHECK(out.completed && out.value == 1);                                        // no filter registered: EXCEPTION_EXECUTE_HANDLER, answered at once
        w.seh.set_unhandled_exception_filter(0x600000);
        const uint32_t sp_in_import = w.cpu.r.esp;
        const auto later = w.seh.unhandled_exception_filter_call(w.cpu, 0x402000, 0x1234);
        CHECK(!later.completed);
        CHECK_EQ(w.cpu.r.eip, 0x600000u);
        CHECK_EQ(w.r(w.cpu.r.esp + 4), 0x1234u);
        w.handler_returns(2, 1);
        CHECK_EQ(w.cpu.r.eip, 0x402000u);                                              // back to the caller of the import with the filter's value
        CHECK_EQ(w.cpu.r.eax, 2u);
        CHECK_EQ(w.cpu.r.esp, sp_in_import + 8);                                       // one argument popped
    }
    // ---- RtlUnwind: handlers of the frames up to the target run their termination code, the frames are unlinked, control continues at TargetIp
    {
        World w;
        const uint32_t target = w.frame(2, 0x500200), middle = w.frame(1, 0x500100), inner = w.frame(0, 0x500000);
        (void)target;
        w.cpu.r.ebx = 0x1; w.cpu.r.ebp = 0x2; w.cpu.r.esi = 0x3; w.cpu.r.edi = 0x4;
        w.call_import(0x403000);
        const uint32_t sp_in_import = w.cpu.r.esp;
        const auto out = w.seh.rtl_unwind(w.cpu, 0x403000, target, 0x404000, 0, 0x77);
        CHECK(!out.completed);
        CHECK_EQ(w.cpu.r.eip, 0x500000u);                                              // the innermost frame is unwound first
        const uint32_t record = w.r(w.cpu.r.esp + 4);
        CHECK_EQ(w.r(record), 0xC0000027u);                                            // STATUS_UNWIND
        CHECK(w.r(record + 4) & 2);                                                    // EXCEPTION_UNWINDING
        CHECK(!(w.r(record + 4) & 4));                                                 // not an exit unwind: a target frame is given
        w.cpu.r.ebx = 0xDEAD;                                                          // a handler may clobber callee-saved registers: the machine restores them
        w.handler_returns(1);
        CHECK_EQ(w.r(w.teb), middle);                                                  // the inner frame is unlinked
        CHECK_EQ(w.cpu.r.eip, 0x500100u);
        w.handler_returns(1);
        CHECK_EQ(w.r(w.teb), target);                                                  // the chain head is the target: done
        CHECK_EQ(w.cpu.r.eip, 0x404000u);                                              // TargetIp
        CHECK_EQ(w.cpu.r.eax, 0x77u);
        CHECK_EQ(w.cpu.r.esp, sp_in_import + 4 + 16);
        CHECK(w.cpu.r.ebx == 0x1 && w.cpu.r.ebp == 0x2 && w.cpu.r.esi == 0x3 && w.cpu.r.edi == 0x4);
        (void)inner;
    }
    {   // target 0 = unwind everything (an exit unwind): the flag is set and control returns to the caller of the import when TargetIp is 0
        World w;
        w.frame(0, 0x500000);
        w.call_import(0x403000);
        w.seh.rtl_unwind(w.cpu, 0x403000, 0, 0, 0, 5);
        const uint32_t record = w.r(w.cpu.r.esp + 4);
        CHECK(w.r(record + 4) & 4);
        w.handler_returns(1);
        CHECK_EQ(w.r(w.teb), 0xFFFFFFFFu);
        CHECK_EQ(w.cpu.r.eip, 0x403000u);
    }
    {   // a record given by the caller is copied and marked
        World w;
        w.frame(0, 0x500000);
        const uint32_t given = w.memory.alloc(Seh::kExceptionRecordSize);
        w.w(given, 0xE0001234u); w.w(given + 16, 1); w.w(given + 20, 0x99);
        w.call_import(0x403000);
        w.seh.rtl_unwind(w.cpu, 0x403000, 0, 0, given, 0);
        const uint32_t record = w.r(w.cpu.r.esp + 4);
        CHECK(record != given && w.r(record) == 0xE0001234u && w.r(record + 20) == 0x99 && (w.r(record + 4) & 2));
    }
    // ---- a handler that never returns (an __except block): its state is purged once the guest stack is above it
    {
        World w;
        w.frame(0, 0x500000);
        w.call_import(0x402000);
        w.seh.raise_exception(w.cpu, 0x402000, 1, 0, 0, 0);
        CHECK_EQ(w.seh.pending(w.cpu), 1u);
        w.cpu.r.esp = w.stack_top;                                                     // the guest longjmp'ed to code above the whole exception frame
        w.seh.purge(w.cpu);
        CHECK_EQ(w.seh.pending(w.cpu), 0u);
        CHECK(faults([&] { w.handler_returns(1); }));                                  // a token with no pending exception
    }
}
REGISTER_TEST("seh", test_seh);
