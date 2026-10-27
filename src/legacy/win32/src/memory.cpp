#include "nue_legacy/win32/memory.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace nue::legacy::win32 {
namespace {

constexpr uint32_t kPage = 4096, kGranularity = 65536, kBaseAddress = 0x10000;
constexpr uint32_t kErrorInvalidAddress = 487;

uint32_t round_up(uint64_t v, uint32_t to) { return uint32_t((v + to - 1) / to * to); }

bool valid_protect(uint32_t p) {
    const uint32_t base = p & ~(page::guard | page::nocache | page::writecombine);
    switch (base) {
    case page::noaccess: case page::readonly: case page::readwrite: case page::writecopy: case page::execute: case page::execute_read: case page::execute_readwrite: case page::execute_writecopy: return true;
    default: return false;
    }
}
bool readable(uint32_t p) { if (p & (page::guard | page::noaccess)) return false; const uint32_t b = p & 0xFF; return b == page::readonly || b == page::readwrite || b == page::writecopy || b == page::execute_read || b == page::execute_readwrite || b == page::execute_writecopy; }
bool writable(uint32_t p) { if (p & (page::guard | page::noaccess)) return false; const uint32_t b = p & 0xFF; return b == page::readwrite || b == page::writecopy || b == page::execute_readwrite || b == page::execute_writecopy; }
bool executable(uint32_t p) { if (p & (page::guard | page::noaccess)) return false; const uint32_t b = p & 0xFF; return b == page::execute || b == page::execute_read || b == page::execute_readwrite || b == page::execute_writecopy; }

class Arena final : public AddressSpace {
public:
    // The window [kBaseAddress, kBaseAddress + bytes) of the guest's address space belongs to this object: the host (guest::Memory) must map all of it
    // contiguously. Region state and page protections are kept here, in software, in 4 KiB guest pages: the host's own page size and MMU play no part.
    Arena(guest::Memory& memory, uint32_t bytes) : size_(round_up(bytes, kGranularity)), pages_(size_ / kPage) {
        memory_ = reinterpret_cast<std::byte*>(memory.host(kBaseAddress, size_));
        if (!memory_) throw std::runtime_error("the guest memory does not map the Win32 address-space window");
    }

    uint32_t virtual_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect, uint32_t& result) override {
        std::lock_guard lock(m_);
        result = 0;
        if (size == 0) return err::invalid_parameter;
        if (!(type & (mem::commit | mem::reserve | mem::reset))) return err::invalid_parameter;
        if (type & ~(mem::commit | mem::reserve | mem::reset | mem::top_down)) return err::invalid_parameter;
        if (type & mem::reset) { return 0; }
        if ((type & mem::commit) && !valid_protect(protect)) return err::invalid_parameter;
        if (!(type & mem::commit)) protect = protect ? protect : page::noaccess;
        if (address == 0) {
            const uint32_t length = round_up(size, kPage);
            const uint32_t base = find_free(length);
            if (!base) return err::not_enough_memory;
            mark_reserved(base, length, protect);
            if (type & mem::commit) mark_committed(base, length, protect);
            result = base;
            return 0;
        }
        if (type & mem::reserve) {
            const uint32_t base = address & ~(kGranularity - 1);
            const uint64_t end = uint64_t(address) + size;
            const uint32_t length = round_up(end - base, kPage);
            if (!in_range(base, length)) return kErrorInvalidAddress;
            for (uint32_t a = base; a < base + length; a += kPage) if (page_at(a).state != 0) return kErrorInvalidAddress;
            mark_reserved(base, length, protect);
            if (type & mem::commit) mark_committed(address & ~(kPage - 1), round_up(end - (address & ~(kPage - 1)), kPage), protect);
            result = address & ~(kPage - 1);
            if (address & (kPage - 1)) result = address & ~(kPage - 1);
            return 0;
        }
        // MEM_COMMIT alone: the pages must lie in a reservation.
        const uint32_t first = address & ~(kPage - 1);
        const uint32_t length = round_up(uint64_t(address) + size - first, kPage);
        if (!in_range(first, length)) return kErrorInvalidAddress;
        uint32_t allocation = 0;
        for (uint32_t a = first; a < first + length; a += kPage) {
            const Page& p = page_at(a);
            if (p.state == 0) return kErrorInvalidAddress;
            if (!allocation) allocation = p.allocation; else if (allocation != p.allocation) return kErrorInvalidAddress;   // across two reservations
        }
        mark_committed(first, length, protect);
        result = first;
        return 0;
    }

    uint32_t virtual_free(uint32_t address, uint32_t size, uint32_t type) override {
        std::lock_guard lock(m_);
        if (type & mem::release) {
            if (type != mem::release || size != 0) return err::invalid_parameter;
            if (!in_range(address, kPage)) return kErrorInvalidAddress;
            const Page& p = page_at(address);
            if (p.state == 0 || p.allocation != address) return kErrorInvalidAddress;       // only the base of an allocation can be released
            const uint32_t a = address;
            uint32_t end = a;
            while (end < kBaseAddress + size_ && page_at(end).state != 0 && page_at(end).allocation == a) end += kPage;
            for (uint32_t x = a; x < end; x += kPage) page_at(x) = Page{};
            return 0;
        }
        if (type != mem::decommit) return err::invalid_parameter;
        uint32_t first = address & ~(kPage - 1);
        uint32_t length;
        if (size == 0) {                                                                   // decommit the whole allocation: the address must be its base
            if (!in_range(address, kPage) || page_at(address).state == 0 || page_at(address).allocation != address) return err::invalid_parameter;
            uint32_t end = address;
            while (end < kBaseAddress + size_ && page_at(end).state != 0 && page_at(end).allocation == address) end += kPage;
            length = end - address;
        } else length = round_up(uint64_t(address) + size - first, kPage);
        if (!in_range(first, length)) return kErrorInvalidAddress;
        for (uint32_t a = first; a < first + length; a += kPage) if (page_at(a).state == 0) return kErrorInvalidAddress;
        for (uint32_t a = first; a < first + length; a += kPage) { Page& p = page_at(a); p.state = 1; p.protect = 0; std::memset(memory_ + (a - kBaseAddress), 0, kPage); }
        return 0;
    }

    uint32_t virtual_protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t& old_protect) override {
        std::lock_guard lock(m_);
        if (size == 0 || !valid_protect(protect)) return err::invalid_parameter;
        const uint32_t first = address & ~(kPage - 1);
        const uint32_t length = round_up(uint64_t(address) + size - first, kPage);
        if (!in_range(first, length)) return kErrorInvalidAddress;
        for (uint32_t a = first; a < first + length; a += kPage) if (page_at(a).state != 2) return kErrorInvalidAddress;
        old_protect = page_at(first).protect;
        for (uint32_t a = first; a < first + length; a += kPage) page_at(a).protect = protect;
        return 0;
    }

    uint32_t virtual_query(uint32_t address, RegionInfo& out) override {
        std::lock_guard lock(m_);
        out = RegionInfo{};
        const uint32_t first = address & ~(kPage - 1);
        if (address >= kBaseAddress + size_) { return err::invalid_parameter; }
        if (address < kBaseAddress) {                                                        // below the arena: free space up to its start
            out.base_address = first; out.state = mem::free_; out.protect = page::noaccess; out.region_size = kBaseAddress - first;
            return 0;
        }
        const Page p = page_at(first);
        out.base_address = first;
        out.state = p.state == 0 ? mem::free_ : p.state == 1 ? mem::reserve : mem::commit;
        out.allocation_base = p.state == 0 ? 0 : p.allocation;
        out.allocation_protect = p.state == 0 ? 0 : p.allocation_protect;
        out.protect = p.state == 2 ? p.protect : (p.state == 0 ? page::noaccess : 0);
        out.type = p.state == 0 ? 0 : mem::private_;
        uint32_t end = first + kPage;
        while (end < kBaseAddress + size_) {
            const Page& q = page_at(end);
            if (q.state != p.state || (p.state != 0 && (q.allocation != p.allocation || q.protect != p.protect))) break;
            end += kPage;
        }
        out.region_size = end - first;
        if (p.state == 0) out.region_size = end - first;
        return 0;
    }

    std::byte* host(uint32_t address, uint32_t size) override {
        std::lock_guard lock(m_);
        if (address < kBaseAddress) return nullptr;
        const uint64_t end = uint64_t(address) + size;
        if (end > uint64_t(kBaseAddress) + size_) return nullptr;
        for (uint32_t a = address & ~(kPage - 1); a < end; a += kPage) if (page_at(a).state != 2) return nullptr;
        return memory_ + (address - kBaseAddress);
    }

private:
    struct Page { uint8_t state = 0; uint32_t protect = 0, allocation = 0, allocation_protect = 0; };   // state 0 free, 1 reserved, 2 committed
    Page& page_at(uint32_t address) { return pages_[(address - kBaseAddress) / kPage]; }
    bool in_range(uint32_t address, uint32_t length) const { return address >= kBaseAddress && uint64_t(address) + length <= uint64_t(kBaseAddress) + size_; }
    uint32_t find_free(uint32_t length) {
        uint32_t run = 0, start = 0;
        for (uint32_t a = kBaseAddress; a < kBaseAddress + size_; a += kPage) {
            if (page_at(a).state == 0) {
                if (run == 0) { if (a % kGranularity) continue; start = a; }
                run += kPage;
                if (run >= length) return start;
            } else run = 0;
        }
        return 0;
    }
    void mark_reserved(uint32_t base, uint32_t length, uint32_t protect) {
        for (uint32_t a = base; a < base + length; a += kPage) { Page& p = page_at(a); p.state = 1; p.allocation = base; p.allocation_protect = protect; p.protect = 0; }
    }
    void mark_committed(uint32_t first, uint32_t length, uint32_t protect) {
        for (uint32_t a = first; a < first + length; a += kPage) { Page& p = page_at(a); if (p.state != 2) { std::memset(memory_ + (a - kBaseAddress), 0, kPage); } p.state = 2; p.protect = protect; }
    }

    std::mutex m_;
    uint32_t size_;
    std::vector<Page> pages_;
    std::byte* memory_ = nullptr;
};

}  // namespace

std::unique_ptr<AddressSpace> make_arena_address_space(guest::Memory& memory, uint32_t window_bytes) { return std::make_unique<Arena>(memory, window_bytes); }

bool is_bad_read_ptr(AddressSpace& s, uint32_t address, uint32_t size) {
    if (size == 0) return false;
    for (uint64_t a = address & ~(kPage - 1ull); a < uint64_t(address) + size; a += kPage) {
        RegionInfo r;
        if (s.virtual_query(uint32_t(a), r) != 0 || r.state != mem::commit || !readable(r.protect)) return true;
    }
    return false;
}
bool is_bad_write_ptr(AddressSpace& s, uint32_t address, uint32_t size) {
    if (size == 0) return false;
    for (uint64_t a = address & ~(kPage - 1ull); a < uint64_t(address) + size; a += kPage) {
        RegionInfo r;
        if (s.virtual_query(uint32_t(a), r) != 0 || r.state != mem::commit || !writable(r.protect)) return true;
    }
    return false;
}
bool is_bad_code_ptr(AddressSpace& s, uint32_t address) {
    RegionInfo r;
    return s.virtual_query(address, r) != 0 || r.state != mem::commit || !(executable(r.protect) || readable(r.protect));
}

// ------------------------------------------------------------------------------------------------------------------------------ VirtualApi
VirtualApi::VirtualApi(AddressSpace& space, uint32_t image_base, uint32_t image_size, RegionListener on_executable, RegionListener on_released)
    : space_(space), image_base_(image_base), image_size_(image_size), on_executable_(std::move(on_executable)), on_released_(std::move(on_released)) {}

Ret<uint32_t> VirtualApi::virtual_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect) {
    uint32_t result = 0;
    const uint32_t error = space_.virtual_alloc(address, size, type, protect, result);
    if (error) return fail<uint32_t>(error);
    if (result && (protect & 0xF0) && outside_image(result, size)) {
        { std::lock_guard lock(m_); regions_.push_back({result, size}); }
        if (on_executable_) on_executable_(result, size);
    }
    return ok(result);
}

Ret<bool> VirtualApi::virtual_protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t* old_protect) {
    uint32_t old = 0;
    const uint32_t error = space_.virtual_protect(address, size, protect, old);
    if (old_protect) *old_protect = old;
    if (error) return fail<bool>(error);
    if ((protect & 0xF0) && outside_image(address, size)) {
        { std::lock_guard lock(m_); regions_.push_back({address, size}); }
        if (on_executable_) on_executable_(address, size);
    }
    return ok(true);
}

Ret<bool> VirtualApi::virtual_free(uint32_t address, uint32_t size, uint32_t type) {
    // The extent of what a release frees, measured before it happens: the whole allocation that starts at `address`.
    uint32_t end = address;
    if (type & mem::release) {
        RegionInfo info;
        if (space_.virtual_query(address, info) == 0 && info.allocation_base == address) {
            for (uint64_t a = address; a < 0xFFFF0000ull; a = uint64_t(info.base_address) + info.region_size) {
                if (space_.virtual_query(uint32_t(a), info) != 0 || info.allocation_base != address) break;
                end = uint32_t(uint64_t(info.base_address) + info.region_size);
            }
        }
    }
    const uint32_t error = space_.virtual_free(address, size, type);
    if (error) return fail<bool>(error);
    if (type & mem::release) {
        std::vector<Region> gone;
        {
            std::lock_guard lock(m_);
            for (auto it = regions_.begin(); it != regions_.end();) {
                if (it->base >= address && it->base < end) { gone.push_back(*it); it = regions_.erase(it); } else ++it;
            }
        }
        if (on_released_) for (const auto& r : gone) on_released_(r.base, r.size);
    }
    return ok(true);
}

Ret<RegionInfo> VirtualApi::virtual_query(uint32_t address) {
    RegionInfo info;
    const uint32_t error = space_.virtual_query(address, info);
    if (error) return fail<RegionInfo>(error);
    return ok(info);
}

std::vector<VirtualApi::Region> VirtualApi::executable_regions() const { std::lock_guard lock(m_); return regions_; }
bool VirtualApi::in_executable_region(uint32_t address) const {
    std::lock_guard lock(m_);
    for (const auto& r : regions_) if (address >= r.base && uint64_t(address) < uint64_t(r.base) + r.size) return true;
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------------ heaps
struct HeapManager::Heap {
    struct Header { uint32_t size, cookie; };   // 8 bytes before every block: the user size and a cookie that says "allocated by this heap"
    static constexpr uint32_t kFreeCookie = 0xFEEEFEEEu, kLiveCookieBase = 0xA110C000u;
    static constexpr uint32_t kChunk = 1u << 20, kLargeThreshold = 64u << 10;

    Heap(AddressSpace& s, HeapId id_, uint32_t maximum_) : space(s), id(id_), maximum(maximum_) {}
    AddressSpace& space;
    HeapId id;
    uint32_t maximum;                           // 0 = growable
    std::mutex m;
    uint32_t chunk_cur = 0, chunk_end = 0;
    std::vector<uint32_t> chunks;
    std::map<uint32_t, std::vector<uint32_t>> free_lists;   // capacity -> block addresses (the address of the header)
    std::map<uint32_t, uint32_t> large;         // header address -> region bytes
    uint64_t blocks = 0, bytes = 0, committed = 0;

    uint32_t cookie() const { return kLiveCookieBase ^ id; }
    static uint32_t capacity_for(uint32_t size) {
        if (size <= 2048) return std::max<uint32_t>(16, (size + 15) & ~15u);
        uint32_t c = 4096;
        while (c < size && c < kLargeThreshold) c <<= 1;
        return c;
    }
    bool grab_chunk() {
        uint32_t base = 0;
        if (space.virtual_alloc(0, kChunk, mem::reserve | mem::commit, page::readwrite, base) != 0 || !base) return false;
        chunks.push_back(base);
        chunk_cur = base;
        chunk_end = base + kChunk;
        committed += kChunk;
        return true;
    }
    // Returns the address of the user data (header + 8), 0 on failure.
    uint32_t allocate(uint32_t size, bool zero) {
        if (size > 0x7FFFFFF0u) return 0;
        if (maximum && (bytes + size > maximum)) return 0;
        const uint32_t capacity = capacity_for(size);
        uint32_t header_address = 0;
        if (size > kLargeThreshold) {
            const uint32_t region = round_up(uint64_t(size) + 16, 4096);
            uint32_t base = 0;
            if (space.virtual_alloc(0, region, mem::reserve | mem::commit, page::readwrite, base) != 0 || !base) return 0;
            header_address = base + 8;                                    // user data at base + 16: 16-byte aligned like a large block
            large[header_address] = region;
            committed += region;
        } else {
            auto it = free_lists.find(capacity);
            if (it != free_lists.end() && !it->second.empty()) { header_address = it->second.back(); it->second.pop_back(); }
            else {
                const uint32_t total = capacity + 8;
                if (chunk_cur + total > chunk_end && !grab_chunk()) return 0;
                header_address = chunk_cur;
                chunk_cur += total;
            }
        }
        auto* header = reinterpret_cast<Header*>(space.host(header_address, 8));
        if (!header) return 0;
        header->size = size;
        header->cookie = cookie();
        std::byte* user = space.host(header_address + 8, std::max<uint32_t>(size, 1));
        if (zero && user) std::memset(user, 0, size);
        ++blocks;
        bytes += size;
        return header_address + 8;
    }
    Header* header_of(uint32_t user) {
        if (user < 8 + kBaseAddress) return nullptr;
        auto* h = reinterpret_cast<Header*>(space.host(user - 8, 8));
        return h && h->cookie == cookie() ? h : nullptr;
    }
    uint32_t release(uint32_t user, Header* h) {
        const uint32_t size = h->size;
        h->cookie = kFreeCookie;
        --blocks;
        bytes -= size;
        const uint32_t header_address = user - 8;
        auto big = large.find(header_address);
        if (big != large.end()) { space.virtual_free(header_address - 8, 0, mem::release); committed -= big->second; large.erase(big); return 0; }
        free_lists[capacity_for(size)].push_back(header_address);
        return 0;
    }
};

HeapManager::HeapManager(AddressSpace& space) : space_(space) {
    process_heap_ = heap_create(0, 0, 0).value;
}
HeapManager::~HeapManager() = default;

HeapManager::Heap* HeapManager::find(HeapId id) const {
    std::lock_guard lock(m_);
    auto it = heaps_.find(id);
    return it == heaps_.end() ? nullptr : it->second.get();
}

Ret<HeapId> HeapManager::heap_create(uint32_t flags, uint32_t initial_size, uint32_t maximum_size) {
    (void)flags;
    if (maximum_size && initial_size > maximum_size) return fail<HeapId>(err::invalid_parameter);
    std::lock_guard lock(m_);
    const HeapId id = next_;
    next_ += 0x10000;
    heaps_[id] = std::make_unique<Heap>(space_, id, maximum_size);
    return ok(id);
}

Ret<bool> HeapManager::heap_destroy(HeapId id) {
    if (id == process_heap_) return fail<bool>(err::invalid_parameter);       // the process heap cannot be destroyed
    std::unique_ptr<Heap> heap;
    {
        std::lock_guard lock(m_);
        auto it = heaps_.find(id);
        if (it == heaps_.end()) return fail<bool>(err::invalid_handle);
        heap = std::move(it->second);
        heaps_.erase(it);
    }
    for (uint32_t c : heap->chunks) space_.virtual_free(c, 0, mem::release);
    for (auto& [address, region] : heap->large) { (void)region; space_.virtual_free(address - 8, 0, mem::release); }
    return ok(true);
}

Ret<uint32_t> HeapManager::heap_alloc(HeapId id, uint32_t flags, uint32_t size) {
    Heap* h = find(id);
    if (!h) return fail<uint32_t>(err::invalid_handle);
    std::lock_guard lock(h->m);
    const uint32_t address = h->allocate(size, flags & heap_flag::zero_memory);
    if (!address) return fail<uint32_t>(err::not_enough_memory);
    return ok(address);
}

Ret<bool> HeapManager::heap_free(HeapId id, uint32_t flags, uint32_t address) {
    (void)flags;
    Heap* h = find(id);
    if (!h) return fail<bool>(err::invalid_handle);
    if (address == 0) return ok(true);                                        // freeing NULL succeeds
    std::lock_guard lock(h->m);
    Heap::Header* header = h->header_of(address);
    if (!header) return fail<bool>(err::invalid_parameter);                   // not a block of this heap (or freed twice)
    h->release(address, header);
    return ok(true);
}

Ret<uint32_t> HeapManager::heap_size(HeapId id, uint32_t flags, uint32_t address) {
    (void)flags;
    Heap* h = find(id);
    if (!h) return fail<uint32_t>(err::invalid_handle, 0xFFFFFFFFu);
    std::lock_guard lock(h->m);
    Heap::Header* header = h->header_of(address);
    if (!header) return fail<uint32_t>(err::invalid_parameter, 0xFFFFFFFFu);
    return ok(header->size);
}

Ret<uint32_t> HeapManager::heap_realloc(HeapId id, uint32_t flags, uint32_t address, uint32_t size) {
    Heap* h = find(id);
    if (!h) return fail<uint32_t>(err::invalid_handle);
    std::lock_guard lock(h->m);
    Heap::Header* header = h->header_of(address);
    if (!header) return fail<uint32_t>(err::invalid_parameter);
    const uint32_t old_size = header->size;
    const uint32_t old_capacity = h->large.count(address - 8) ? header->size : Heap::capacity_for(old_size);
    if (size <= old_capacity && size <= Heap::kLargeThreshold && old_size <= Heap::kLargeThreshold) {   // fits in the block it has: stays in place
        if (size > old_size && (flags & heap_flag::zero_memory)) if (std::byte* user = h->space.host(address + old_size, size - old_size)) std::memset(user, 0, size - old_size);
        h->bytes += uint64_t(size) - old_size;
        header->size = size;
        return ok(address);
    }
    if (flags & heap_flag::realloc_in_place_only) return fail<uint32_t>(err::not_enough_memory);
    const uint32_t fresh = h->allocate(size, flags & heap_flag::zero_memory);
    if (!fresh) return fail<uint32_t>(err::not_enough_memory);
    std::byte* from = h->space.host(address, std::max<uint32_t>(1, std::min(old_size, size)));
    std::byte* to = h->space.host(fresh, std::max<uint32_t>(1, std::min(old_size, size)));
    if (from && to) std::memcpy(to, from, std::min(old_size, size));
    h->release(address, header);
    return ok(fresh);
}

Ret<uint32_t> HeapManager::global_alloc(uint32_t flags, uint32_t size) {
    return heap_alloc(process_heap_, flags & gmem::zeroinit ? heap_flag::zero_memory : 0, size);
}
Ret<uint32_t> HeapManager::global_free(uint32_t address) {
    auto r = heap_free(process_heap_, 0, address);
    return r.error ? Ret<uint32_t>{address, r.error} : ok(0u);
}

uint64_t HeapManager::live_blocks(HeapId id) const { Heap* h = find(id); return h ? h->blocks : 0; }
uint64_t HeapManager::live_bytes(HeapId id) const { Heap* h = find(id); return h ? h->bytes : 0; }

}  // namespace nue::legacy::win32
