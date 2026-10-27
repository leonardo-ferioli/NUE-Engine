// Guest memory as a legacy layer needs it: the one door between a 32-bit guest address and the host. legacy_core will own this header; it is here
// because legacy_dsound (and the other layers) need the contract today. It is the same contract as `guest::Memory` of the NFS Underground 2 port
// (guest_memory.hpp, docs/MEMORY_MODEL_64.md): the port adapts with `namespace nfsu2::guest { using Memory = nue::legacy::guest::Memory; }` or a thin
// subclass.
//
// Rules for a library using it:
//   - a guest address is a 32-bit number and never a host pointer: it is turned into a host pointer only by host(), checked for the whole range;
//   - a host pointer is turned into a guest address only by address_of(), and only for memory this object handed out;
//   - on a 32-bit host the two are the same number; on a 64-bit host (x86-64, arm64; any OS) the guest lives in an arena, host = base + guest, and nothing
//     in a library may depend on which. No reinterpret_cast between a guest address and a pointer, anywhere.
#pragma once
#include <cstdint>

namespace nue::legacy::guest {

class Memory {
public:
    virtual ~Memory() = default;
    virtual uint32_t alloc(uint32_t bytes) = 0;                     // zero-filled, 16-byte aligned, inside the guest address range; 0 on failure
    virtual void free(uint32_t address) = 0;
    virtual uint8_t* host(uint32_t address, uint32_t bytes) = 0;    // host pointer valid for `bytes` bytes (at least 1), nullptr when not mapped
    virtual uint32_t address_of(const void* host_pointer) = 0;      // guest address of memory this object handed out (0 when foreign)
};

}  // namespace nue::legacy::guest
