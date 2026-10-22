// CPU topology and thread placement helpers, including hybrid CPUs (performance / efficiency cores) via CPU Sets.
#pragma once
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

struct CpuSetInfo {
    uint32_t id = 0;               // CPU Set id (not an index)
    uint16_t group = 0;
    uint8_t logical_index = 0;     // within the group
    uint8_t core_index = 0;        // physical core, within the group
    uint8_t last_level_cache = 0;
    uint8_t numa_node = 0;
    uint8_t efficiency_class = 0;  // higher = faster core class (0 on non-hybrid CPUs)
    uint8_t scheduling_class = 0;
    bool parked = false;
    bool allocated = false;        // exclusively allocated to another process
    bool allocated_to_target = false;
    bool real_time = false;
};

struct CpuTopology {
    std::vector<CpuSetInfo> sets;
    std::string brand;                 // CPUID brand string
    std::string vendor;
    uint32_t logical_processors = 0;
    uint32_t physical_cores = 0;
    uint32_t processor_groups = 1;
    bool hybrid = false;               // at least two distinct efficiency classes
    uint8_t max_efficiency_class = 0;
    uint8_t min_efficiency_class = 0;
    bool smt = false;
    std::vector<uint32_t> performance_set_ids;  // the highest efficiency class (all, on a non-hybrid CPU)
    std::vector<uint32_t> efficiency_set_ids;   // lower classes (empty on a non-hybrid CPU)
    // One CPU Set id per physical core of the performance class: the first SMT sibling only (for threads that should not share a core).
    std::vector<uint32_t> performance_primary_set_ids;
    bool wow64 = false;                // 32-bit process on 64-bit Windows
};

struct CpuFeatures {
    bool sse2 = false, sse41 = false, sse42 = false, avx = false, avx2 = false, avx512f = false, fma = false, popcnt = false;
};
// CPUID plus the OS support check (XGETBV) for AVX and AVX-512 state; all false on non-x86.
CpuFeatures query_cpu_features();
uint32_t cpu_max_mhz();   // highest MaxMhz over the logical processors (CallNtPowerInformation), 0 unknown

// Reads GetSystemCpuSetInformation + CPUID. Returns false when CPU Sets are not available.
bool query_cpu_topology(CpuTopology& out);

// Pure part of the classification: derives hybrid/performance/efficiency lists from `topo.sets`. Exposed for tests with synthetic data.
void classify_cpu_sets(CpuTopology& topo);

enum class CoreClass : uint8_t { Any, Performance, Efficiency };

// Restricts one thread to the CPU Sets of a class (soft preference: the scheduler may still use others when the sets are busy).
// `thread` null = current thread. Passing an empty list removes the restriction.
Status thread_use_cpu_sets(HANDLE thread, const std::vector<uint32_t>& set_ids);
Status thread_use_core_class(HANDLE thread, const CpuTopology& topo, CoreClass cls);
// Default CPU sets for the whole process (threads without their own selection).
Status process_use_cpu_sets(const std::vector<uint32_t>& set_ids);
// Ideal processor (a hint, group-aware).
Status thread_set_ideal_processor(HANDLE thread, uint16_t group, uint8_t number);
// Hard affinity (mask inside the thread's current group). Prefer CPU Sets: affinity bypasses the scheduler's hybrid awareness.
Status thread_set_affinity_mask(HANDLE thread, uint64_t mask);
// Which CPU Set / core the calling thread runs on right now (diagnostics).
uint32_t current_processor_number();

}  // namespace nue::win
