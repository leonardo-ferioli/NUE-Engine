// ArenaMemory: a portable guest::Memory over one block of host memory standing for the low part of the guest's 4 GiB address window.
//
// It is the memory a host with no real arena uses: tests, tools, and any port that wants the guest in a plain allocation. Layout (guest addresses):
//   [0, 0x10000)                          never mapped (null-pointer guard, like Windows)
//   [0x10000, 0x10000 + window_bytes)     the window the Win32 address space manages (VirtualAlloc, heaps): make_arena_address_space(memory, window_bytes)
//   [0x10000 + window_bytes, total)       allocated by guest::Memory::alloc for objects the host creates for the guest (COM objects, structures)
// Every host pointer comes from host(); no guest address is ever a host address.
#pragma once
#include "nue_legacy/guest_memory.hpp"
#include <map>
#include <memory>
#include <mutex>

namespace nue::legacy::win32 {

class ArenaMemory final : public guest::Memory {
public:
    static constexpr uint32_t kWindowBase = 0x10000;
    explicit ArenaMemory(uint32_t total_bytes = 512u << 20, uint32_t window_bytes = 256u << 20);
    ~ArenaMemory() override;
    uint32_t window_bytes() const { return window_bytes_; }

    uint32_t alloc(uint32_t bytes) override;
    void free(uint32_t address) override;
    uint8_t* host(uint32_t address, uint32_t bytes) override;
    uint32_t address_of(const void* host_pointer) override;

private:
    std::mutex m_;
    uint8_t* block_ = nullptr;
    uint32_t total_, window_bytes_, zone_base_;
    std::map<uint32_t, uint32_t> free_;     // free ranges of the allocation zone: address -> bytes
    std::map<uint32_t, uint32_t> live_;     // allocated blocks: address -> bytes
};

}  // namespace nue::legacy::win32
