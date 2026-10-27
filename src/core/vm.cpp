// Virtual address space service (include/nue/vm.h): region table with generation-checked ids, committed-range bookkeeping, thin OS layer in os_util.cpp.
#include "internal/core_internal.h"
#include "internal/os_util.h"
#include "nue/vm.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace {

struct Region {
    void* base = nullptr;
    uint64_t size = 0;
    std::map<uint64_t, uint64_t> committed;   // disjoint, non-adjacent [start, end) ranges (offsets from the base)
    uint64_t committed_bytes = 0;
};

struct VmState {
    std::mutex mutex;
    std::vector<std::unique_ptr<Region>> regions;   // slot i -> id low half i + 1
    std::vector<uint16_t> generations;
    std::vector<uint32_t> free_slots;
};

VmState& S() {
    static VmState* s = new VmState();   // never destroyed: regions are the caller's and may outlive static destruction order
    return *s;
}

Region* lookup(VmState& s, nue_vm_region id) {
    const uint32_t idx = id & 0xFFFFu;
    if (idx == 0 || idx > s.regions.size() || !s.regions[idx - 1] || s.generations[idx - 1] != (id >> 16)) return nullptr;
    return s.regions[idx - 1].get();
}

void add_range(Region& r, uint64_t start, uint64_t stop) {   // merges with every overlapping or adjacent range
    auto it = r.committed.lower_bound(start);
    if (it != r.committed.begin()) {
        auto prev = std::prev(it);
        if (prev->second >= start) it = prev;
    }
    while (it != r.committed.end() && it->first <= stop) {
        if (it->first < start) start = it->first;
        if (it->second > stop) stop = it->second;
        it = r.committed.erase(it);
    }
    r.committed[start] = stop;
}

void remove_range(Region& r, uint64_t start, uint64_t stop) {
    auto it = r.committed.upper_bound(start);
    if (it != r.committed.begin()) --it;
    while (it != r.committed.end() && it->first < stop) {
        const uint64_t s = it->first, e = it->second;
        if (e <= start) { ++it; continue; }
        it = r.committed.erase(it);
        if (s < start) r.committed[s] = start;
        if (e > stop) { r.committed[stop] = e; break; }
    }
}

bool covered(const Region& r, uint64_t start, uint64_t stop) {   // the whole range lies inside one committed range (ranges are kept coalesced)
    auto it = r.committed.upper_bound(start);
    if (it == r.committed.begin()) return false;
    --it;
    return it->first <= start && it->second >= stop;
}

uint64_t sum_committed(const Region& r) {
    uint64_t total = 0;
    for (const auto& kv : r.committed) total += kv.second - kv.first;
    return total;
}

bool is_pow2(uint64_t v) { return v && (v & (v - 1)) == 0; }

nue_result check_range(const Region& r, uint64_t offset, uint64_t size, uint32_t page) {
    if (size == 0 || offset % page != 0 || size % page != 0) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_vm: offset and size must be non-zero multiples of the host page size (%u)", page);
    if (offset > r.size || size > r.size - offset) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_vm: range outside the region");
    return NUE_OK;
}

}  // namespace

using namespace nuei;

extern "C" {

NUE_API nue_result NUE_CALL nue_vm_get_info(nue_vm_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    const OsVmInfo os = os_vm_info();
    nue_vm_info i;
    std::memset(&i, 0, sizeof(i));
    i.struct_size = sizeof(i);
    i.page_size = os.page_size;
    i.granularity = os.granularity;
    i.address_bits = os.address_bits;
    i.flags = NUE_VM_CAN_ALIGN | (os.can_exec ? NUE_VM_CAN_EXEC : 0u);
    i.address_limit_bytes = os.address_limit;
    {
        VmState& s = S();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (const auto& r : s.regions) {
            if (!r) continue;
            ++i.region_count;
            i.reserved_bytes += r->size;
            i.committed_bytes += r->committed_bytes;
        }
    }
    const uint32_t keep = out->struct_size < sizeof(i) ? out->struct_size : uint32_t(sizeof(i));
    std::memcpy(out, &i, keep);
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vm_reserve(const nue_vm_reserve_desc* desc, nue_vm_region* out_region, void** out_base) {
    if (!desc || !out_region || desc->struct_size < sizeof(nue_vm_reserve_desc)) return NUE_ERR_INVALID_ARGUMENT;
    *out_region = 0;
    const OsVmInfo os = os_vm_info();
    if (desc->size == 0) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_vm_reserve: size is 0");
    uint64_t alignment = desc->alignment ? desc->alignment : os.granularity;
    if (!is_pow2(alignment)) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_vm_reserve: alignment %llu is not a power of two", (unsigned long long)alignment);
    if (alignment < os.granularity) alignment = os.granularity;
    const uint64_t gran = os.granularity;
    if (desc->size > UINT64_MAX - gran) return NUE_ERR_INVALID_ARGUMENT;
    const uint64_t size = (desc->size + gran - 1) / gran * gran;
    if (size > uint64_t(SIZE_MAX) / 2 || alignment > uint64_t(SIZE_MAX) / 2)
        return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_vm_reserve: %llu bytes (alignment %llu) do not fit the %zu-bit address space of this process",
                          (unsigned long long)size, (unsigned long long)alignment, sizeof(void*) * 8);
    void* base = nullptr;
    nue_result r = os_vm_reserve(size_t(size), size_t(alignment), desc->preferred, (desc->flags & NUE_VM_RESERVE_EXACT) != 0, &base);
    if (NUE_FAILED(r)) return nuei::fail(r, "nue_vm_reserve: the OS refused %llu bytes aligned to %llu", (unsigned long long)size, (unsigned long long)alignment);

    VmState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    uint32_t slot;
    if (!s.free_slots.empty()) { slot = s.free_slots.back(); s.free_slots.pop_back(); }
    else {
        if (s.regions.size() >= 0xFFFEu) { os_vm_release(base, size_t(size)); return nuei::fail(NUE_ERR_OUT_OF_MEMORY, "nue_vm_reserve: too many regions"); }
        slot = uint32_t(s.regions.size());
        s.regions.emplace_back();
        s.generations.push_back(0);
    }
    uint16_t gen = uint16_t(s.generations[slot] + 1);
    if (gen == 0) gen = 1;
    s.generations[slot] = gen;
    std::unique_ptr<Region> reg(new Region());
    reg->base = base;
    reg->size = size;
    s.regions[slot] = std::move(reg);
    *out_region = (uint32_t(gen) << 16) | (slot + 1);
    if (out_base) *out_base = base;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vm_region_get_info(nue_vm_region id, nue_vm_region_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    VmState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Region* r = lookup(s, id);
    if (!r) return NUE_ERR_INVALID_HANDLE;
    nue_vm_region_info i;
    std::memset(&i, 0, sizeof(i));
    i.struct_size = sizeof(i);
    i.base = r->base;
    i.size = r->size;
    i.committed_bytes = r->committed_bytes;
    const uint32_t keep = out->struct_size < sizeof(i) ? out->struct_size : uint32_t(sizeof(i));
    std::memcpy(out, &i, keep);
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vm_commit(nue_vm_region id, uint64_t offset, uint64_t size, uint32_t protection) {
    if (protection & ~uint32_t(NUE_VM_READ | NUE_VM_WRITE | NUE_VM_EXEC)) return NUE_ERR_INVALID_ARGUMENT;
    const OsVmInfo os = os_vm_info();
    VmState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Region* r = lookup(s, id);
    if (!r) return NUE_ERR_INVALID_HANDLE;
    nue_result c = check_range(*r, offset, size, os.page_size);
    if (NUE_FAILED(c)) return c;
    if ((protection & NUE_VM_EXEC) && !os.can_exec) return nuei::fail(NUE_ERR_UNSUPPORTED, "nue_vm_commit: executable memory is not available on this platform");
    nue_result res = os_vm_commit(static_cast<uint8_t*>(r->base) + offset, size_t(size), protection);
    if (NUE_FAILED(res)) return nuei::fail(res, "nue_vm_commit: the OS refused %llu bytes", (unsigned long long)size);
    add_range(*r, offset, offset + size);
    r->committed_bytes = sum_committed(*r);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vm_protect(nue_vm_region id, uint64_t offset, uint64_t size, uint32_t protection) {
    if (protection & ~uint32_t(NUE_VM_READ | NUE_VM_WRITE | NUE_VM_EXEC)) return NUE_ERR_INVALID_ARGUMENT;
    const OsVmInfo os = os_vm_info();
    VmState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Region* r = lookup(s, id);
    if (!r) return NUE_ERR_INVALID_HANDLE;
    nue_result c = check_range(*r, offset, size, os.page_size);
    if (NUE_FAILED(c)) return c;
    if (!covered(*r, offset, offset + size)) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_vm_protect: the range is not fully committed");
    if ((protection & NUE_VM_EXEC) && !os.can_exec) return nuei::fail(NUE_ERR_UNSUPPORTED, "nue_vm_protect: executable memory is not available on this platform");
    nue_result res = os_vm_protect(static_cast<uint8_t*>(r->base) + offset, size_t(size), protection);
    return NUE_FAILED(res) ? nuei::fail(res, "nue_vm_protect: the OS refused the change") : NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vm_decommit(nue_vm_region id, uint64_t offset, uint64_t size) {
    const OsVmInfo os = os_vm_info();
    VmState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Region* r = lookup(s, id);
    if (!r) return NUE_ERR_INVALID_HANDLE;
    nue_result c = check_range(*r, offset, size, os.page_size);
    if (NUE_FAILED(c)) return c;
    nue_result res = os_vm_decommit(static_cast<uint8_t*>(r->base) + offset, size_t(size));
    if (NUE_FAILED(res)) return nuei::fail(res, "nue_vm_decommit: the OS refused the change");
    remove_range(*r, offset, offset + size);
    r->committed_bytes = sum_committed(*r);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vm_release(nue_vm_region id) {
    VmState& s = S();
    std::unique_ptr<Region> victim;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        Region* r = lookup(s, id);
        if (!r) return NUE_ERR_INVALID_HANDLE;
        const uint32_t idx = (id & 0xFFFFu) - 1;
        victim = std::move(s.regions[idx]);
        s.generations[idx] = uint16_t(s.generations[idx] + 1 == 0 ? 1 : s.generations[idx] + 1);   // stale ids fail at once
        s.free_slots.push_back(idx);
    }
    os_vm_release(victim->base, size_t(victim->size));
    return NUE_OK;
}

}  // extern "C"
