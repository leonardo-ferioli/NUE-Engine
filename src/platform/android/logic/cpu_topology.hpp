// big.LITTLE classification from per-core maximum frequencies (kHz, as in cpufreq/cpuinfo_max_freq).  Pure logic.
#pragma once
#include <cstdint>
#include <vector>

namespace nue::android {

struct CoreMasks {
    uint64_t performance = 0; // cores within 80 % of the fastest core (prime + gold)
    uint64_t efficiency = 0;  // cores at or below 65 % of the fastest core (silver / little)
    uint64_t all = 0;
    uint32_t performance_count = 0, efficiency_count = 0, total = 0;
    uint32_t max_khz = 0;
    bool hybrid = false;      // both classes exist
};

// Cores whose frequency is unknown (0) are ignored in the classes but kept in `all`.  Up to 64 cores.
CoreMasks classify_cores(const std::vector<uint32_t>& max_khz);

// nue_thread_priority (0 default, 1 low, 2 normal, 3 above normal, 4 high, 5 realtime) -> Linux nice value in the
// range Android allows for apps (android.os.Process: BACKGROUND 10, DEFAULT 0, FOREGROUND -2, DISPLAY -4,
// URGENT_DISPLAY -8, AUDIO -16).  Never below -16.
int nice_from_priority(int nue_priority);
int priority_from_nice(int nice);

} // namespace nue::android
