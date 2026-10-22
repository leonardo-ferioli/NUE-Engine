#include "memory_budget.hpp"

#include <algorithm>

namespace nue::android {

MemPressure pressure_from_trim_level(int level) {
    if (level < 0) return MemPressure::Critical; // onLowMemory
    if (level >= 20) return MemPressure::Background; // UI hidden and everything deeper in the LRU list
    if (level >= 15) return MemPressure::Critical;
    if (level >= 10) return MemPressure::Low;
    if (level >= 5) return MemPressure::Moderate;
    return MemPressure::None;
}

Budget compute_budget(const SystemMemory& sys, MemPressure pressure, const BudgetConfig& cfg) {
    Budget b;
    b.pressure = pressure;

    uint64_t hard = cfg.process_cap;
    if (sys.total_ram > 0) hard = std::min<uint64_t>(hard, static_cast<uint64_t>(double(sys.total_ram) * cfg.ram_fraction));
    // What the system can still give: what we already hold plus what is free above the kill threshold.
    if (sys.available_ram > 0) {
        uint64_t free_above = sys.available_ram > sys.low_threshold ? sys.available_ram - sys.low_threshold : 0;
        hard = std::min<uint64_t>(hard, sys.rss + free_above);
    }
    // A device that is already in the low state gets no growth room at all.
    if (sys.low_memory) hard = std::min<uint64_t>(hard, std::max<uint64_t>(sys.rss, cfg.engine_reserve));
    b.hard_limit = hard;

    double soft_scale = cfg.soft_fraction;
    switch (pressure) {
    case MemPressure::None: break;
    case MemPressure::Moderate: soft_scale *= 0.90; break;
    case MemPressure::Low: soft_scale *= 0.75; break;
    case MemPressure::Critical: soft_scale *= 0.60; break;
    case MemPressure::Background: soft_scale *= 0.50; break; // a backgrounded game should shrink, not grow
    }
    b.soft_limit = static_cast<uint64_t>(double(hard) * soft_scale);
    b.texture_budget = b.soft_limit > cfg.engine_reserve ? b.soft_limit - cfg.engine_reserve : 0;
    b.bytes_to_release = sys.rss > b.soft_limit ? sys.rss - b.soft_limit : 0;
    return b;
}

} // namespace nue::android
