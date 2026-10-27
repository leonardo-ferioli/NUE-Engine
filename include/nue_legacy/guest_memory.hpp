// nue::legacy::GuestMemory - the ONE interface through which every legacy layer (legacy_win32, legacy_dsound, the DirectInput model, the Direct3D 9
// layer) reaches the memory of a 32-bit guest. A library never assumes that a guest address is a host address: it asks this object.
//
// Spec and rationale: docs/GUEST_MEMORY.md. Legacy-side C++17 (header only, no exceptions thrown); it is NOT part of the native C ABI, which never
// sees a guest address. Guest addresses are uint32_t. Protections use the Win32 PAGE_* values because the guest asks in those terms (VirtualProtect).
//
//   IdentityMemory  32-bit host: guest address == host address (the process heap and the image the loader mapped). Test/reference implementation.
//   ArenaMemory     64-bit host: a reserved arena (nue/vm.h), host = base + guest, allocator, 4 KiB software page table.
//
// Guest page protections live in a SOFTWARE table checked by readable()/writable()/protection(); they are not enforced by the host MMU (the translated
// code reads and writes through plain pointers), so a guest that touches a PAGE_NOACCESS page is only caught where a library asks.
//
// ABI of this interface: NUE_LEGACY_MEMORY_ABI changes when a virtual function is added, removed or reordered (append only, like the COM vtable orders).
#pragma once

#include "nue/base.h"
#include "nue/core.h"
#include "nue/vm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#define NUE_LEGACY_MEMORY_ABI 1

namespace nue::legacy {

// ---- Win32 page protection values --------------------------------------------------------------------------------------------------------

enum PageProtection : uint32_t {
    kPageNoAccess = 0x01, kPageReadOnly = 0x02, kPageReadWrite = 0x04, kPageWriteCopy = 0x08,
    kPageExecute = 0x10, kPageExecuteRead = 0x20, kPageExecuteReadWrite = 0x40, kPageExecuteWriteCopy = 0x80,
    kPageGuard = 0x100
};

inline bool is_valid_protection(uint32_t p) {
    const uint32_t base = p & ~uint32_t(kPageGuard);
    if ((p & kPageGuard) && base == kPageNoAccess) return false;   // PAGE_GUARD cannot be combined with PAGE_NOACCESS
    return base == kPageNoAccess || base == kPageReadOnly || base == kPageReadWrite || base == kPageWriteCopy || base == kPageExecute ||
           base == kPageExecuteRead || base == kPageExecuteReadWrite || base == kPageExecuteWriteCopy;
}
inline bool protection_allows_read(uint32_t p) {
    if (p & kPageGuard) return false;
    return p != 0 && p != kPageNoAccess;   // PAGE_EXECUTE counts as readable, as on x86
}
inline bool protection_allows_write(uint32_t p) {
    if (p & kPageGuard) return false;
    return p == kPageReadWrite || p == kPageWriteCopy || p == kPageExecuteReadWrite || p == kPageExecuteWriteCopy;
}

// ---- the interface -------------------------------------------------------------------------------------------------------------------------

class GuestMemory {
public:
    virtual ~GuestMemory() = default;

    // zero filled, aligned to `align` (a power of two, 16 by default), inside the guest address range; 0 on failure. alloc(0) returns a valid 16 byte block.
    virtual uint32_t alloc(uint32_t bytes, uint32_t align = 16) = 0;
    virtual void free(uint32_t address) = 0;                          // an address returned by alloc; anything else is ignored
    // Host pointer valid for [address, address+bytes), nullptr when any byte is not mapped (committed). Protection is not consulted.
    virtual uint8_t* host(uint32_t address, uint32_t bytes) = 0;
    virtual uint32_t address_of(const void* host_pointer) = 0;        // guest address of mapped memory this object manages, 0 when foreign
    virtual bool readable(uint32_t address, uint32_t bytes) = 0;      // IsBadReadPtr(...) == FALSE: mapped and the protection allows reading
    virtual bool writable(uint32_t address, uint32_t bytes) = 0;
    virtual uint32_t protection(uint32_t address) = 0;                // PAGE_* of the page holding `address`, 0 when not committed (VirtualQuery)
    virtual uint32_t protect(uint32_t address, uint32_t bytes, uint32_t page_protection) = 0;   // VirtualProtect: the old protection of the first page, 0 on failure
    virtual nue_result commit(uint32_t address, uint32_t bytes, uint32_t page_protection) = 0;  // VirtualAlloc(MEM_COMMIT) inside the address space
    virtual nue_result decommit(uint32_t address, uint32_t bytes) = 0;                         // VirtualFree(MEM_DECOMMIT)
    virtual bool identity() const = 0;                                // true when host(a) == a (32-bit host)

    // typed helpers
    template <class T> T* ptr(uint32_t address) { return reinterpret_cast<T*>(host(address, uint32_t(sizeof(T)))); }
    bool read_u32(uint32_t address, uint32_t* out) { const uint8_t* p = host(address, 4); if (!p) return false; std::memcpy(out, p, 4); return true; }
    bool write_u32(uint32_t address, uint32_t value) { uint8_t* p = host(address, 4); if (!p) return false; std::memcpy(p, &value, 4); return true; }
};

// ---- ArenaMemory: 64-bit hosts ------------------------------------------------------------------------------------------------------------

struct ArenaConfig {
    uint64_t arena_size = 4ull << 30;        // bytes of guest address space (<= 4 GiB); less than 4 GiB is a "masked" arena: higher addresses are never handed out
    uint64_t reserve_alignment = 4ull << 30; // alignment of the base (4 GiB makes (uint32_t)host_pointer == guest address)
    const void* preferred_base = nullptr;    // placement hint
    bool exact_base = false;                 // fail unless placed at preferred_base
    uint32_t alloc_begin = 0x10000000u;      // the allocator hands out [alloc_begin, alloc_end); the image (0x00400000) and the token ranges (0xF0000000+) stay outside
    uint32_t alloc_end = 0xE0000000u;        // clamped to arena_size
    uint32_t decommit_threshold = 256u * 1024u;   // a freed block at least this large gives its whole 4 KiB pages back
};

struct ArenaStats {
    uint64_t allocated_bytes = 0;            // sum of live blocks
    uint64_t live_blocks = 0;
    uint64_t committed_guest_bytes = 0;      // 4 KiB guest pages marked committed
    uint64_t free_ranges = 0;
};

class ArenaMemory final : public GuestMemory {
public:
    static constexpr uint32_t kGuestPage = 4096;

    // nullptr (and *error) when the host refuses the reservation or is not a 64-bit host
    static std::unique_ptr<ArenaMemory> create(const ArenaConfig& config = ArenaConfig(), nue_result* error = nullptr) {
        auto fail = [&](nue_result r) { if (error) *error = r; return std::unique_ptr<ArenaMemory>(); };
        if (config.arena_size == 0 || config.arena_size > (4ull << 30) || config.arena_size % kGuestPage != 0) return fail(NUE_ERR_INVALID_ARGUMENT);
        if (sizeof(void*) < 8) return fail(NUE_ERR_UNSUPPORTED);   // 32-bit hosts: guest address == host address, use IdentityMemory
        nue_vm_info info;
        NUE_STRUCT_INIT(&info);
        if (NUE_FAILED(nue_vm_get_info(&info))) return fail(NUE_ERR_UNSUPPORTED);
        nue_vm_reserve_desc d;
        NUE_STRUCT_INIT(&d);
        d.size = config.arena_size;
        d.alignment = config.reserve_alignment;
        d.preferred = config.preferred_base;
        d.flags = config.exact_base ? NUE_VM_RESERVE_EXACT : 0u;
        nue_vm_region region = 0;
        void* base = nullptr;
        nue_result r = nue_vm_reserve(&d, &region, &base);
        if (NUE_FAILED(r)) return fail(r);
        std::unique_ptr<ArenaMemory> a(new ArenaMemory(config, info.page_size, region, static_cast<uint8_t*>(base)));
        if (error) *error = NUE_OK;
        return a;
    }

    ~ArenaMemory() override { if (region_) nue_vm_release(region_); }
    ArenaMemory(const ArenaMemory&) = delete;
    ArenaMemory& operator=(const ArenaMemory&) = delete;

    uint8_t* base() const { return base_; }                 // host address of guest address 0 (what generated code adds)
    uint64_t size() const { return size_; }
    uint32_t host_page_size() const { return host_page_; }

    ArenaStats stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        ArenaStats s;
        for (const auto& b : used_) { s.allocated_bytes += b.second; ++s.live_blocks; }
        for (uint16_t p : table_) if (p) s.committed_guest_bytes += kGuestPage;
        s.free_ranges = free_.size();
        return s;
    }

    // ---- GuestMemory ----
    uint32_t alloc(uint32_t bytes, uint32_t align = 16) override {
        if (align == 0) align = 16;
        if ((align & (align - 1)) != 0 || align > 65536u) return 0;
        if (align < 16) align = 16;
        const uint64_t need = bytes == 0 ? 16 : ((uint64_t(bytes) + 15u) & ~uint64_t(15));
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            const uint64_t start = it->first, stop = it->second;
            const uint64_t at = (start + align - 1) & ~uint64_t(align - 1);
            if (at + need > stop) continue;
            if (!commit_locked(uint32_t(at), uint32_t(need), kPageReadWrite)) return 0;
            // split the free range around the block
            free_.erase(it);
            if (at > start) free_[start] = at;
            if (at + need < stop) free_[at + need] = stop;
            used_[uint32_t(at)] = uint32_t(need);
            if (at < high_water_) std::memset(base_ + at, 0, size_t(std::min<uint64_t>(need, high_water_ - at)));   // above the high water mark the pages are fresh and zero
            if (at + need > high_water_) high_water_ = at + need;
            return uint32_t(at);
        }
        return 0;
    }

    void free(uint32_t address) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = used_.find(address);
        if (it == used_.end()) return;
        const uint64_t size = it->second;
        used_.erase(it);
        if (size >= config_.decommit_threshold) {
            const uint64_t first = (uint64_t(address) + kGuestPage - 1) & ~uint64_t(kGuestPage - 1);
            const uint64_t last = (uint64_t(address) + size) & ~uint64_t(kGuestPage - 1);
            if (last > first) decommit_locked(uint32_t(first), uint32_t(last - first));
        }
        // return to the free list, coalescing with both neighbours
        uint64_t start = address, stop = uint64_t(address) + size;
        auto next = free_.lower_bound(start);
        if (next != free_.end() && next->first == stop) { stop = next->second; next = free_.erase(next); }
        if (next != free_.begin()) {
            auto prev = std::prev(next);
            if (prev->second == start) { start = prev->first; free_.erase(prev); }
        }
        free_[start] = stop;
    }

    uint8_t* host(uint32_t address, uint32_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return mapped_locked(address, bytes) ? base_ + address : nullptr;
    }

    uint32_t address_of(const void* p) override {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        if (b < base_ || uint64_t(b - base_) >= size_) return 0;
        const uint32_t a = uint32_t(b - base_);
        std::lock_guard<std::mutex> lock(mutex_);
        return mapped_locked(a, 1) ? a : 0;
    }

    bool readable(uint32_t address, uint32_t bytes) override { return allowed(address, bytes, false); }
    bool writable(uint32_t address, uint32_t bytes) override { return allowed(address, bytes, true); }

    uint32_t protection(uint32_t address) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return address < size_ ? table_[address / kGuestPage] : 0u;
    }

    uint32_t protect(uint32_t address, uint32_t bytes, uint32_t page_protection) override {
        if (!is_valid_protection(page_protection)) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (bytes == 0 || uint64_t(address) + bytes > size_) return 0;
        const uint32_t first = address / kGuestPage, last = uint32_t((uint64_t(address) + bytes - 1) / kGuestPage);
        for (uint32_t i = first; i <= last; ++i) if (table_[i] == 0) return 0;
        const uint32_t old = table_[first];
        for (uint32_t i = first; i <= last; ++i) table_[i] = uint16_t(page_protection);
        return old;
    }

    nue_result commit(uint32_t address, uint32_t bytes, uint32_t page_protection) override {
        if (!is_valid_protection(page_protection) || bytes == 0) return NUE_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        if (uint64_t(address) + bytes > size_) return NUE_ERR_INVALID_ARGUMENT;
        return commit_locked(address, bytes, page_protection) ? NUE_OK : NUE_ERR_OUT_OF_MEMORY;
    }

    nue_result decommit(uint32_t address, uint32_t bytes) override {
        if (bytes == 0) return NUE_ERR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        if (uint64_t(address) + bytes > size_) return NUE_ERR_INVALID_ARGUMENT;
        decommit_locked(address, bytes);
        return NUE_OK;
    }

    bool identity() const override { return false; }

private:
    ArenaMemory(const ArenaConfig& c, uint32_t host_page, nue_vm_region region, uint8_t* base)
        : config_(c), host_page_(host_page ? host_page : 4096u), region_(region), base_(base), size_(c.arena_size), table_(size_t(c.arena_size / kGuestPage), 0) {
        const uint64_t end = c.alloc_end < size_ ? c.alloc_end : size_;
        const uint64_t begin = (uint64_t(c.alloc_begin) + 15u) & ~uint64_t(15);
        if (begin < end) free_[begin] = end & ~uint64_t(15);
    }

    uint64_t host_page_of(uint64_t guest) const { return guest / host_page_; }
    bool host_page_in_use(uint64_t hp) const {
        const uint64_t first = hp * host_page_ / kGuestPage, count = host_page_ / kGuestPage ? host_page_ / kGuestPage : 1;
        for (uint64_t i = first; i < first + count && i < table_.size(); ++i) if (table_[i]) return true;
        return false;
    }

    bool mapped_locked(uint32_t address, uint32_t bytes) const {
        if (uint64_t(address) + (bytes ? bytes : 1) > size_) return false;
        const uint32_t first = address / kGuestPage, last = uint32_t((uint64_t(address) + (bytes ? bytes : 1) - 1) / kGuestPage);
        for (uint32_t i = first; i <= last; ++i) if (table_[i] == 0) return false;
        return true;
    }

    bool allowed(uint32_t address, uint32_t bytes, bool write) {
        if (bytes == 0) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (uint64_t(address) + bytes > size_) return false;
        const uint32_t first = address / kGuestPage, last = uint32_t((uint64_t(address) + bytes - 1) / kGuestPage);
        for (uint32_t i = first; i <= last; ++i) {
            const uint32_t p = table_[i];
            if (!(write ? protection_allows_write(p) : protection_allows_read(p))) return false;
        }
        return true;
    }

    // Commits the host pages covering the range (always read/write at host level; the guest protection is software), zeroes guest pages that become
    // committed inside an already committed host page, and records the protection.
    bool commit_locked(uint32_t address, uint32_t bytes, uint32_t prot) {
        const uint64_t gfirst = address / kGuestPage, glast = (uint64_t(address) + bytes - 1) / kGuestPage;
        const uint64_t hfirst = host_page_of(gfirst * kGuestPage), hlast = host_page_of(glast * kGuestPage);
        // which host pages are new (no guest page of them committed yet)?
        std::vector<bool> fresh;
        for (uint64_t hp = hfirst; hp <= hlast; ++hp) fresh.push_back(!host_page_in_use(hp));
        // one nue_vm_commit for the whole span of host pages (already committed pages keep their contents). The region is rounded up to the reservation
        // granularity, which is a multiple of the host page, so the last host page of a masked arena is always inside it.
        if (NUE_FAILED(nue_vm_commit(region_, hfirst * host_page_, (hlast - hfirst + 1) * host_page_, NUE_VM_READ | NUE_VM_WRITE))) return false;
        for (uint64_t g = gfirst; g <= glast; ++g) {
            if (table_[g] == 0 && !fresh[size_t(host_page_of(g * kGuestPage) - hfirst)]) std::memset(base_ + g * kGuestPage, 0, kGuestPage);
            table_[g] = uint16_t(prot);
        }
        return true;
    }

    void decommit_locked(uint32_t address, uint32_t bytes) {
        const uint64_t gfirst = address / kGuestPage, glast = (uint64_t(address) + bytes - 1) / kGuestPage;
        for (uint64_t g = gfirst; g <= glast; ++g) table_[g] = 0;
        // give back every host page that no guest page uses any more (runs are merged into one call)
        const uint64_t hfirst = host_page_of(gfirst * kGuestPage), hlast = host_page_of(glast * kGuestPage);
        uint64_t run = 0, run_len = 0;
        auto flush = [&] {
            if (run_len) nue_vm_decommit(region_, run * host_page_, run_len * host_page_);
            run_len = 0;
        };
        for (uint64_t hp = hfirst; hp <= hlast; ++hp) {
            if (!host_page_in_use(hp)) { if (!run_len) run = hp; ++run_len; }
            else flush();
        }
        flush();
    }

    ArenaConfig config_;
    uint32_t host_page_;
    nue_vm_region region_;
    uint8_t* base_;
    uint64_t size_;
    std::mutex mutex_;
    std::vector<uint16_t> table_;                       // one entry per 4 KiB guest page: 0 = not committed, else a PAGE_* value
    std::map<uint64_t, uint64_t> free_;                 // allocator: free [start, end) ranges
    std::map<uint32_t, uint32_t> used_;                 // allocator: live block -> size
    uint64_t high_water_ = 0;                           // highest address ever handed out; memory above it is untouched and zero
};

// ---- IdentityMemory: 32-bit hosts (guest address == host address) -------------------------------------------------------------------------

class IdentityMemory final : public GuestMemory {
public:
    // Registers memory the loader/OS already mapped (the guest image, the stack) so that host()/readable() know it.
    void adopt(uint32_t address, uint32_t bytes, uint32_t page_protection = kPageReadWrite) {
        std::lock_guard<std::mutex> lock(mutex_);
        ranges_[address] = Range{bytes, page_protection, false};
    }

    uint32_t alloc(uint32_t bytes, uint32_t align = 16) override {
        if (align == 0) align = 16;
        if ((align & (align - 1)) != 0) return 0;
        if (align < 16) align = 16;
        const size_t size = bytes == 0 ? 16 : size_t(bytes);
        void* p = nue_alloc(size, align);
        if (!p) return 0;
        const uint64_t address = uint64_t(reinterpret_cast<uintptr_t>(p));
        if (address > 0xFFFFFFFFull || address + size > 0x100000000ull) { nue_free(p); return 0; }   // not addressable by a 32-bit guest (64-bit host)
        std::memset(p, 0, size);
        std::lock_guard<std::mutex> lock(mutex_);
        ranges_[uint32_t(address)] = Range{uint32_t(size), kPageReadWrite, true};
        return uint32_t(address);
    }

    void free(uint32_t address) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ranges_.find(address);
        if (it == ranges_.end() || !it->second.owned) return;
        ranges_.erase(it);
        nue_free(reinterpret_cast<void*>(static_cast<uintptr_t>(address)));
    }

    uint8_t* host(uint32_t address, uint32_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return find_locked(address, bytes ? bytes : 1) ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address)) : nullptr;
    }

    uint32_t address_of(const void* p) override {
        const uint64_t a = uint64_t(reinterpret_cast<uintptr_t>(p));
        if (a > 0xFFFFFFFFull) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        return find_locked(uint32_t(a), 1) ? uint32_t(a) : 0u;
    }

    bool readable(uint32_t address, uint32_t bytes) override {
        if (bytes == 0) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        const Range* r = find_locked(address, bytes);
        return r && protection_allows_read(r->protection);
    }
    bool writable(uint32_t address, uint32_t bytes) override {
        if (bytes == 0) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        const Range* r = find_locked(address, bytes);
        return r && protection_allows_write(r->protection);
    }
    uint32_t protection(uint32_t address) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const Range* r = find_locked(address, 1);
        return r ? r->protection : 0u;
    }
    // software only: the OS protections of memory the loader mapped are not touched (the port keeps using VirtualProtect there)
    uint32_t protect(uint32_t address, uint32_t bytes, uint32_t page_protection) override {
        if (!is_valid_protection(page_protection) || bytes == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        Range* r = find_locked(address, bytes);
        if (!r) return 0;
        const uint32_t old = r->protection;
        r->protection = page_protection;
        return old;
    }
    nue_result commit(uint32_t, uint32_t, uint32_t) override { return NUE_ERR_UNSUPPORTED; }    // the OS owns the address space of a 32-bit host
    nue_result decommit(uint32_t, uint32_t) override { return NUE_ERR_UNSUPPORTED; }
    bool identity() const override { return true; }

private:
    struct Range { uint32_t size; uint32_t protection; bool owned; };
    Range* find_locked(uint32_t address, uint32_t bytes) {
        auto it = ranges_.upper_bound(address);
        if (it == ranges_.begin()) return nullptr;
        --it;
        const uint64_t end = uint64_t(it->first) + it->second.size;
        return (address >= it->first && uint64_t(address) + bytes <= end) ? &it->second : nullptr;
    }
    std::mutex mutex_;
    std::map<uint32_t, Range> ranges_;
};

}  // namespace nue::legacy
