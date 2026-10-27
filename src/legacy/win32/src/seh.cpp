#include "nue_legacy/win32/seh.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace nue::legacy::win32 {
namespace {
constexpr uint32_t kContextFlags = 0x10007;      // CONTEXT_i386 | CONTROL | INTEGER | SEGMENTS
constexpr uint32_t kStatusUnwind = 0xC0000027u;
constexpr uint32_t kUnwinding = 2, kExitUnwind = 4, kNonContinuable = 1;

std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%x", v); return b; }
}  // namespace

Seh::Seh(guest::Memory& memory, SehTokens tokens, std::function<void(std::string_view)> log) : memory_(memory), tokens_(tokens), log_(std::move(log)) {}

uint8_t* Seh::bytes(uint32_t address, uint32_t size) const {
    uint8_t* p = memory_.host(address, size);
    if (!p) throw GuestFault("guest exception state at an unmapped address " + hex(address));
    return p;
}
uint32_t Seh::rd(uint32_t address) const { uint32_t v; std::memcpy(&v, bytes(address, 4), 4); return v; }
void Seh::wr(uint32_t address, uint32_t value) const { std::memcpy(bytes(address, 4), &value, 4); }

uint32_t Seh::set_unhandled_exception_filter(uint32_t filter) { const uint32_t old = filter_; filter_ = filter; return old; }

Seh::State& Seh::top(const GuestCpuState& cpu) {
    std::lock_guard lock(m_);
    auto it = states_.find(&cpu);
    if (it == states_.end() || it->second.empty()) throw GuestFault("guest exception token reached without a pending exception");
    return it->second.back();
}
void Seh::push(const GuestCpuState& cpu, const State& s) { std::lock_guard lock(m_); states_[&cpu].push_back(s); }
size_t Seh::pending(const GuestCpuState& cpu) const { std::lock_guard lock(m_); auto it = states_.find(&cpu); return it == states_.end() ? 0 : it->second.size(); }

void Seh::purge(const GuestCpuState& cpu) {
    std::lock_guard lock(m_);
    auto found = states_.find(&cpu);
    if (found == states_.end()) return;
    const uint32_t esp = cpu.regs().esp;
    auto& list = found->second;
    while (!list.empty() && list.back().call_base < esp) list.pop_back();
    if (list.empty()) states_.erase(found);
}

void Seh::build_context(GuestCpuState& cpu, uint32_t context, uint32_t eip, uint32_t esp) {
    const GuestRegs r = cpu.regs();
    std::memset(bytes(context, kContextSize), 0, kContextSize);
    wr(context, kContextFlags);
    wr(context + 0x9C, r.edi); wr(context + 0xA0, r.esi); wr(context + 0xA4, r.ebx); wr(context + 0xA8, r.edx); wr(context + 0xAC, r.ecx); wr(context + 0xB0, r.eax); wr(context + 0xB4, r.ebp);
    const uint32_t kept = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 10) | (1u << 11);
    wr(context + 0xB8, eip); wr(context + 0xC0, 0x202u | (r.eflags & kept)); wr(context + 0xC4, esp);
    wr(context + 0xBC, 0x1B); wr(context + 0xC8, 0x23); wr(context + 0x98, 0x23); wr(context + 0x94, 0x23); wr(context + 0x90, 0x3B);
}

void Seh::finish_with_context(GuestCpuState& cpu, uint32_t context) {
    GuestRegs r = cpu.regs();
    r.edi = rd(context + 0x9C); r.esi = rd(context + 0xA0); r.ebx = rd(context + 0xA4); r.edx = rd(context + 0xA8); r.ecx = rd(context + 0xAC); r.eax = rd(context + 0xB0); r.ebp = rd(context + 0xB4);
    const uint32_t kept = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 10) | (1u << 11);
    r.eflags = (r.eflags & ~kept) | (rd(context + 0xC0) & kept);
    r.esp = rd(context + 0xC4);
    r.eip = rd(context + 0xB8);
    cpu.set_regs(r);
}

void Seh::call_handler(GuestCpuState& cpu, State& s, uint32_t handler, uint32_t frame, uint32_t token) {
    // stdcall: EXCEPTION_DISPOSITION handler(EXCEPTION_RECORD*, void* frame, CONTEXT*, void* dispatcher_context)
    const uint32_t sp = s.call_base;
    wr(sp, token); wr(sp + 4, s.record); wr(sp + 8, frame); wr(sp + 12, s.context); wr(sp + 16, sp + 24); wr(sp + 24, 0);
    GuestRegs r = cpu.regs();
    r.esp = sp; r.eip = handler;
    cpu.set_regs(r);
}

void Seh::enter_filter(GuestCpuState& cpu, State& s, uint32_t pointers) {
    wr(s.call_base, tokens_.filter_return); wr(s.call_base + 4, pointers);
    GuestRegs r = cpu.regs();
    r.esp = s.call_base; r.eip = filter_;
    cpu.set_regs(r);
}

Seh::Outcome Seh::unhandled_exception_filter_call(GuestCpuState& cpu, uint32_t guest_return, uint32_t pointers) {
    purge(cpu);
    const uint32_t esp = cpu.regs().esp;
    if (!filter_) { note("UnhandledExceptionFilter with no registered guest filter: EXCEPTION_EXECUTE_HANDLER"); return {true, 1}; }
    State s;
    s.kind = State::filter; s.import_return = true; s.pointers = pointers; s.guest_return = guest_return; s.resume_sp = esp + 8;
    s.call_base = (esp - 0x80) & ~15u;
    push(cpu, s);
    enter_filter(cpu, top(cpu), pointers);
    return {};
}

Seh::Outcome Seh::raise_exception(GuestCpuState& cpu, uint32_t guest_return, uint32_t code, uint32_t flags, uint32_t argument_count, uint32_t arguments) {
    purge(cpu);
    const uint32_t esp = cpu.regs().esp, teb = cpu.teb();
    State s;
    s.guest_return = guest_return; s.resume_sp = esp + 4 + 16;
    s.context = (esp - 0x40 - kContextSize) & ~15u; s.record = (s.context - kExceptionRecordSize) & ~15u;
    s.pointers = s.record - 8; s.call_base = (s.pointers - 0x80) & ~15u;
    const uint32_t count = std::min(argument_count, 15u);
    std::memset(bytes(s.record, kExceptionRecordSize), 0, kExceptionRecordSize);
    wr(s.record, code); wr(s.record + 4, flags & 1); wr(s.record + 12, guest_return); wr(s.record + 16, count);
    for (uint32_t i = 0; i < count; ++i) wr(s.record + 20 + 4 * i, rd(arguments + 4 * i));
    build_context(cpu, s.context, guest_return, s.resume_sp);
    wr(s.pointers, s.record); wr(s.pointers + 4, s.context);
    s.kind = State::raise; s.frame = rd(teb);
    push(cpu, s);
    note("RaiseException code=" + hex(code) + " flags=" + hex(flags) + " from guest " + hex(guest_return));
    continue_raise(cpu);
    return {};
}

Seh::Outcome Seh::rtl_unwind(GuestCpuState& cpu, uint32_t guest_return, uint32_t target_frame, uint32_t target_ip, uint32_t given, uint32_t value) {
    purge(cpu);
    const GuestRegs regs = cpu.regs();
    State s;
    s.guest_return = guest_return; s.resume_sp = regs.esp + 4 + 16;
    s.context = (regs.esp - 0x40 - kContextSize) & ~15u; s.record = (s.context - kExceptionRecordSize) & ~15u;
    s.pointers = s.record - 8; s.call_base = (s.pointers - 0x80) & ~15u;
    std::memset(bytes(s.record, kExceptionRecordSize), 0, kExceptionRecordSize);
    if (given) std::memcpy(bytes(s.record, kExceptionRecordSize), bytes(given, kExceptionRecordSize), kExceptionRecordSize);
    else wr(s.record, kStatusUnwind);
    wr(s.record + 4, rd(s.record + 4) | kUnwinding | (target_frame ? 0u : kExitUnwind));
    build_context(cpu, s.context, guest_return, s.resume_sp);
    wr(s.pointers, s.record); wr(s.pointers + 4, s.context);
    s.kind = State::unwind; s.target_frame = target_frame; s.target_ip = target_ip; s.return_value = value;
    s.saved[0] = regs.ebx; s.saved[1] = regs.ebp; s.saved[2] = regs.esi; s.saved[3] = regs.edi;
    push(cpu, s);
    continue_unwind(cpu);
    return {};
}

void Seh::continue_raise(GuestCpuState& cpu) {
    State& s = top(cpu);
    if (s.frame != 0xFFFFFFFFu && s.frame != 0) {
        call_handler(cpu, s, rd(s.frame + 4), s.frame, tokens_.handler_return);
        return;
    }
    // End of the chain: the registered top-level filter decides, otherwise the exception is fatal.
    if (filter_) { s.kind = State::filter; s.import_return = false; enter_filter(cpu, s, s.pointers); return; }
    throw GuestFault("unhandled guest exception code=" + hex(rd(s.record)) + " at guest " + hex(rd(s.record + 12)));
}

void Seh::continue_unwind(GuestCpuState& cpu) {
    State& s = top(cpu);
    const uint32_t frame = rd(cpu.teb());
    if (frame != s.target_frame && frame != 0xFFFFFFFFu && frame != 0) {
        s.frame = frame;
        call_handler(cpu, s, rd(frame + 4), frame, tokens_.unwind_return);
        return;
    }
    // Unwinding is complete: continue at TargetIp (or return to the caller) with the callee-saved registers intact.
    GuestRegs r = cpu.regs();
    r.ebx = s.saved[0]; r.ebp = s.saved[1]; r.esi = s.saved[2]; r.edi = s.saved[3];
    r.eax = s.return_value; r.esp = s.resume_sp;
    r.eip = s.target_ip ? s.target_ip : s.guest_return;
    cpu.set_regs(r);
    purge(cpu);
}

void Seh::handler_returned(GuestCpuState& cpu) {
    State& s = top(cpu);
    const GuestRegs r = cpu.regs();
    const uint32_t token = r.eip, value = r.eax;
    const uint32_t expected = s.call_base + (token == tokens_.filter_return ? 8 : 20);
    if (r.esp != expected) throw GuestFault("guest exception handler returned with an unbalanced stack");
    if (token == tokens_.handler_return) {
        if (value == 1) { s.frame = rd(s.frame); continue_raise(cpu); return; }                 // ExceptionContinueSearch
        if (value == 0) {                                                                       // ExceptionContinueExecution
            if (rd(s.record + 4) & kNonContinuable) throw GuestFault("guest handler continued a non-continuable exception");
            const uint32_t context = s.context;
            finish_with_context(cpu, context);
            purge(cpu);
            return;
        }
        throw GuestFault("guest exception handler returned an unsupported disposition");
    }
    if (token == tokens_.unwind_return) {
        // The handler has run its termination code; the frame is unlinked before the next one is visited.
        wr(cpu.teb(), rd(s.frame));
        continue_unwind(cpu);
        return;
    }
    // Top-level filter: -1 resumes; anything else ends the process in the original, here an explicit fault.
    if (s.import_return) {
        GuestRegs back = r;
        back.eax = value; back.esp = s.resume_sp; back.eip = s.guest_return;
        cpu.set_regs(back);
        purge(cpu);
        return;
    }
    if (int32_t(value) == -1) { const uint32_t context = s.context; finish_with_context(cpu, context); purge(cpu); return; }
    throw GuestFault("unhandled guest exception code=" + hex(rd(s.record)) + " (top-level filter returned " + std::to_string(int32_t(value)) + ")");
}

}  // namespace nue::legacy::win32
