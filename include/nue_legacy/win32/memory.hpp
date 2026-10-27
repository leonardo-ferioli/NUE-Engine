// Memory: VirtualAlloc / VirtualFree / VirtualProtect / VirtualQuery / IsBad*Ptr, heaps (HeapCreate, HeapAlloc, HeapReAlloc, HeapFree, HeapSize,
// HeapDestroy, GetProcessHeap) and GlobalAlloc / GlobalFree.
//
// The guest's memory is an `AddressSpace`: a 32-bit address space the library asks for pages (reserve, commit, protect...) and that tells it where a
// guest address lives in the host (`host()`), which is how the facade reads and writes guest data. Two implementations: the portable one
// (make_arena_address_space: a window of the guest's address range that a guest::Memory maps, the model of NUE's WINDOW64 / INDIRECT guests) and, on Windows,
// the real VirtualAlloc of a 32-bit process (identity mapping, legacy_win32/host). The Win32 rules (alignment, errors, state machine of a region) and the page
// protections (software table in 4 KiB guest pages, never the host MMU: hosts have 4 or 16 KiB pages) live in the portable one; the heap manager on top is
// independent of which address space it runs in. No guest address is ever taken for a host address here: host() is the only way to a pointer.
#pragma once
#include "nue_legacy/guest_memory.hpp"
#include "nue_legacy/win32/common.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace nue::legacy::win32 {

namespace mem { enum : uint32_t { commit = 0x1000, reserve = 0x2000, decommit = 0x4000, release = 0x8000, reset = 0x80000, top_down = 0x100000, free_ = 0x10000, private_ = 0x20000, image = 0x1000000, mapped = 0x40000 }; }
namespace page { enum : uint32_t { noaccess = 0x01, readonly = 0x02, readwrite = 0x04, writecopy = 0x08, execute = 0x10, execute_read = 0x20, execute_readwrite = 0x40, execute_writecopy = 0x80, guard = 0x100, nocache = 0x200, writecombine = 0x400 }; }
namespace heap_flag { enum : uint32_t { no_serialize = 1, generate_exceptions = 4, zero_memory = 8, realloc_in_place_only = 0x10, create_enable_execute = 0x40000 }; }
namespace gmem { enum : uint32_t { fixed = 0, moveable = 2, zeroinit = 0x40 }; }

struct RegionInfo {                         // MEMORY_BASIC_INFORMATION
    uint32_t base_address = 0, allocation_base = 0, allocation_protect = 0, region_size = 0, state = mem::free_, protect = 0, type = 0;
};

class AddressSpace {
public:
    virtual ~AddressSpace() = default;
    // Each returns a Win32 error code (0 = success). Sizes are in bytes; the implementation rounds to pages.
    virtual uint32_t virtual_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect, uint32_t& result) = 0;
    virtual uint32_t virtual_free(uint32_t address, uint32_t size, uint32_t type) = 0;
    virtual uint32_t virtual_protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t& old_protect) = 0;
    virtual uint32_t virtual_query(uint32_t address, RegionInfo& out) = 0;
    // The host memory behind [address, address + size), or null when any of it is not committed.
    virtual std::byte* host(uint32_t address, uint32_t size) = 0;
};

// The address space over the window [0x10000, 0x10000 + window_bytes) of `memory` (which must map it contiguously: host(0x10000, window_bytes) not null).
// `memory` must outlive the result. Throws std::runtime_error when the window is not mapped.
std::unique_ptr<AddressSpace> make_arena_address_space(guest::Memory& memory, uint32_t window_bytes = 256u << 20);

// IsBadReadPtr / IsBadWritePtr / IsBadCodePtr on any address space: true when some byte of the range cannot be accessed that way.
bool is_bad_read_ptr(AddressSpace&, uint32_t address, uint32_t size);
bool is_bad_write_ptr(AddressSpace&, uint32_t address, uint32_t size);
bool is_bad_code_ptr(AddressSpace&, uint32_t address);

// VirtualAlloc / VirtualProtect / VirtualFree / VirtualQuery as the guest calls them, over any address space, with one policy for the execution engine:
// memory that becomes executable outside the main image holds run-time generated code (a decompressed or relocated module, a thunk), so the engine is told
// where it is ("register only": what is done with it, an interpreter on a desktop or nothing on a host that forbids run-time code, is decided elsewhere).
class VirtualApi {
public:
    using RegionListener = std::function<void(uint32_t base, uint32_t size)>;
    struct Region { uint32_t base = 0, size = 0; };
    VirtualApi(AddressSpace& space, uint32_t image_base, uint32_t image_size, RegionListener on_executable = {}, RegionListener on_released = {});
    Ret<uint32_t> virtual_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect);   // the base, 0 on failure
    Ret<bool> virtual_protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t* old_protect);
    Ret<bool> virtual_free(uint32_t address, uint32_t size, uint32_t type);
    Ret<RegionInfo> virtual_query(uint32_t address);
    std::vector<Region> executable_regions() const;                  // the regions announced and not released since
    bool in_executable_region(uint32_t address) const;
private:
    bool outside_image(uint32_t base, uint32_t size) const { return uint64_t(base) + size <= image_base_ || base >= uint64_t(image_base_) + image_size_; }
    AddressSpace& space_;
    uint32_t image_base_, image_size_;
    RegionListener on_executable_, on_released_;
    mutable std::mutex m_;
    std::vector<Region> regions_;
};

using HeapId = Handle;

class HeapManager {
public:
    explicit HeapManager(AddressSpace& space);
    ~HeapManager();
    HeapId get_process_heap() const { return process_heap_; }
    Ret<HeapId> heap_create(uint32_t flags, uint32_t initial_size, uint32_t maximum_size);   // maximum 0 = growable
    Ret<bool> heap_destroy(HeapId);
    Ret<uint32_t> heap_alloc(HeapId, uint32_t flags, uint32_t size);                          // the address, 0 on failure (ERROR_NOT_ENOUGH_MEMORY)
    Ret<uint32_t> heap_realloc(HeapId, uint32_t flags, uint32_t address, uint32_t size);
    Ret<bool> heap_free(HeapId, uint32_t flags, uint32_t address);                            // FALSE for an address the heap does not own
    Ret<uint32_t> heap_size(HeapId, uint32_t flags, uint32_t address);                        // 0xFFFFFFFF for an invalid block
    // GlobalAlloc / GlobalFree (fixed memory out of the process heap; GMEM_MOVEABLE is served as fixed memory: documented deviation).
    Ret<uint32_t> global_alloc(uint32_t flags, uint32_t size);
    Ret<uint32_t> global_free(uint32_t address);                                              // 0 on success, the address on failure
    uint64_t live_blocks(HeapId) const;
    uint64_t live_bytes(HeapId) const;

private:
    struct Heap;
    Heap* find(HeapId) const;
    AddressSpace& space_;
    mutable std::mutex m_;
    std::map<HeapId, std::unique_ptr<Heap>> heaps_;
    HeapId process_heap_ = 0, next_ = 0x00150000;
};

}  // namespace nue::legacy::win32
