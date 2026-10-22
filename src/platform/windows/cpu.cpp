#include "nue_win/cpu.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <intrin.h>
#define NUE_HAVE_CPUID 1
#endif

namespace nue::win {

namespace {
void cpuid_strings(std::string& vendor, std::string& brand) {
#ifdef NUE_HAVE_CPUID
    int r[4] = {};
    __cpuid(r, 0);
    char v[13] = {};
    std::memcpy(v, &r[1], 4);
    std::memcpy(v + 4, &r[3], 4);
    std::memcpy(v + 8, &r[2], 4);
    vendor = v;
    __cpuid(r, int(0x80000000));
    if (unsigned(r[0]) >= 0x80000004u) {
        char b[49] = {};
        for (int i = 0; i < 3; ++i) { __cpuid(r, int(0x80000002 + i)); std::memcpy(b + 16 * i, r, 16); }
        brand = b;
        size_t a = brand.find_first_not_of(' ');
        brand = a == std::string::npos ? std::string() : brand.substr(a);
    }
#else
    vendor.clear();
    brand.clear();
#endif
}
}  // namespace

void classify_cpu_sets(CpuTopology& t) {
    t.logical_processors = uint32_t(t.sets.size());
    t.performance_set_ids.clear();
    t.efficiency_set_ids.clear();
    t.performance_primary_set_ids.clear();
    if (t.sets.empty()) { t.hybrid = false; t.max_efficiency_class = t.min_efficiency_class = 0; t.physical_cores = 0; t.smt = false; return; }
    uint8_t lo = 255, hi = 0;
    std::set<std::pair<uint16_t, uint8_t>> cores;
    for (const auto& s : t.sets) {
        lo = std::min(lo, s.efficiency_class);
        hi = std::max(hi, s.efficiency_class);
        cores.insert({s.group, s.core_index});
    }
    t.min_efficiency_class = lo;
    t.max_efficiency_class = hi;
    t.hybrid = hi != lo;
    t.physical_cores = uint32_t(cores.size());
    t.smt = t.physical_cores < t.logical_processors;
    std::set<std::pair<uint16_t, uint8_t>> seen;
    for (const auto& s : t.sets) {
        if (s.efficiency_class == hi) {
            t.performance_set_ids.push_back(s.id);
            if (seen.insert({s.group, s.core_index}).second) t.performance_primary_set_ids.push_back(s.id);
        } else {
            t.efficiency_set_ids.push_back(s.id);
        }
    }
}

bool query_cpu_topology(CpuTopology& out) {
    out = CpuTopology{};
    cpuid_strings(out.vendor, out.brand);
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) return false;
    std::vector<uint8_t> buffer(length);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length, GetCurrentProcess(), 0)) return false;
    std::set<uint16_t> groups;
    for (ULONG off = 0; off + sizeof(DWORD) * 2 <= length;) {
        auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + off);
        if (info->Size == 0) break;
        if (info->Type == CpuSetInformation) {
            CpuSetInfo s;
            const auto& c = info->CpuSet;
            s.id = c.Id;
            s.group = c.Group;
            s.logical_index = c.LogicalProcessorIndex;
            s.core_index = c.CoreIndex;
            s.last_level_cache = c.LastLevelCacheIndex;
            s.numa_node = c.NumaNodeIndex;
            s.efficiency_class = c.EfficiencyClass;
            s.scheduling_class = c.SchedulingClass;
            s.parked = c.Parked != 0;
            s.allocated = c.Allocated != 0;
            s.allocated_to_target = c.AllocatedToTargetProcess != 0;
            s.real_time = c.RealTime != 0;
            groups.insert(s.group);
            out.sets.push_back(s);
        }
        off += info->Size;
    }
    out.processor_groups = uint32_t(std::max<size_t>(1, groups.size()));
    classify_cpu_sets(out);
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    out.wow64 = wow != FALSE;
    return !out.sets.empty();
}

Status thread_use_cpu_sets(HANDLE thread, const std::vector<uint32_t>& ids) {
    HANDLE h = thread ? thread : GetCurrentThread();
    std::vector<ULONG> list(ids.begin(), ids.end());
    if (!SetThreadSelectedCpuSets(h, list.empty() ? nullptr : list.data(), ULONG(list.size()))) return Status::last_error("SetThreadSelectedCpuSets");
    return Status::success();
}

Status thread_use_core_class(HANDLE thread, const CpuTopology& t, CoreClass cls) {
    switch (cls) {
        case CoreClass::Performance: return thread_use_cpu_sets(thread, t.hybrid ? t.performance_set_ids : std::vector<uint32_t>{});
        case CoreClass::Efficiency: return thread_use_cpu_sets(thread, t.hybrid ? t.efficiency_set_ids : std::vector<uint32_t>{});
        default: return thread_use_cpu_sets(thread, {});
    }
}

Status process_use_cpu_sets(const std::vector<uint32_t>& ids) {
    std::vector<ULONG> list(ids.begin(), ids.end());
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), list.empty() ? nullptr : list.data(), ULONG(list.size()))) return Status::last_error("SetProcessDefaultCpuSets");
    return Status::success();
}

Status thread_set_ideal_processor(HANDLE thread, uint16_t group, uint8_t number) {
    PROCESSOR_NUMBER p{};
    p.Group = group;
    p.Number = number;
    if (!SetThreadIdealProcessorEx(thread ? thread : GetCurrentThread(), &p, nullptr)) return Status::last_error("SetThreadIdealProcessorEx");
    return Status::success();
}

Status thread_set_affinity_mask(HANDLE thread, uint64_t mask) {
    if (!SetThreadAffinityMask(thread ? thread : GetCurrentThread(), DWORD_PTR(mask))) return Status::last_error("SetThreadAffinityMask");
    return Status::success();
}

uint32_t current_processor_number() { return GetCurrentProcessorNumber(); }

}  // namespace nue::win
