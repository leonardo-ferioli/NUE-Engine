// nue::legacy::GuestCpu - what the Win32 library (legacy_win32) needs from the HOST that runs the translated game: executing guest code for callbacks,
// guest thread contexts, the synthetic TEB, SEH, FPU state and a clean stop. Abstract on purpose: the generated code and its runtime live in the
// game's project, which implements this class and hands it to legacy_core at start-up; NUE only defines the contract (docs/GUEST_MEMORY.md, section 3).
//
// Guest addresses are uint32_t (see guest_memory.hpp). A "context" is a guest CPU state (registers, stack, TEB) bound to one host thread at a time.
// NUE_LEGACY_CPU_ABI changes when a virtual function is added, removed or reordered (append only).
#pragma once

#include <cstdint>

#define NUE_LEGACY_CPU_ABI 1

namespace nue::legacy {

enum class CallConv : uint8_t { Stdcall = 0, Cdecl = 1, Thiscall = 2, Fastcall = 3 };

struct GuestCall {
    uint32_t entry = 0;              // guest address of the function
    const uint32_t* args = nullptr;  // stack words, first argument first (for Thiscall `this` goes in `this_pointer`)
    uint32_t arg_count = 0;
    CallConv convention = CallConv::Stdcall;
    uint32_t this_pointer = 0;       // ECX for Thiscall
    uint32_t edx = 0;                // EDX for Fastcall (ECX = this_pointer)
};

struct GuestResult {
    uint32_t eax = 0;
    uint32_t edx = 0;                // high half of a 64-bit result
    bool completed = true;           // false when the guest did not return (halt requested, thread exited, unwound past the call)
};

class GuestCpu {
public:
    virtual ~GuestCpu() = default;

    // Runs a guest function on a pooled context of the calling host thread (window procedures, timer callbacks, enumeration callbacks, DllMain-style
    // entries). Re-entrant: a guest function may call the host which calls the guest again.
    virtual GuestResult call(const GuestCall& call) = 0;

    // Guest thread contexts (CreateThread). `entry`/`parameter` start a new context with its own stack; returns a context id (0 on failure) and fills the TEB.
    virtual uint32_t create_context(uint32_t entry, uint32_t parameter, uint32_t stack_bytes, bool suspended, uint32_t* out_teb) = 0;
    virtual void destroy_context(uint32_t context) = 0;
    virtual uint32_t current_context() = 0;               // context bound to the calling host thread, 0 if none
    virtual uint32_t teb(uint32_t context) = 0;           // guest address of the synthetic TEB (LastError at +0x34, SEH chain at +0)
    virtual bool resume_context(uint32_t context) = 0;    // ResumeThread for a suspended start

    // Structured exceptions (RaiseException, RtlUnwind, UnhandledExceptionFilter): the runtime owns the frame walk.
    virtual bool raise_exception(uint32_t code, uint32_t flags, const uint32_t* parameters, uint32_t parameter_count) = 0;
    virtual bool unwind(uint32_t target_frame, uint32_t target_ip, uint32_t return_value) = 0;

    // x87 control word and MXCSR are mirrored to the host FPU on write and refreshed on read.
    virtual uint16_t fpu_control() = 0;
    virtual void set_fpu_control(uint16_t control_word) = 0;
    virtual uint32_t mxcsr() = 0;
    virtual void set_mxcsr(uint32_t value) = 0;

    // ExitProcess / host shutdown: every blocked or running guest call returns with completed = false as soon as possible.
    virtual void request_halt(int exit_code) = 0;
    virtual bool halt_requested() = 0;
};

}  // namespace nue::legacy
