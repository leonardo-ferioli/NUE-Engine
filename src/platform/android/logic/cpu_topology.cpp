#include "cpu_topology.hpp"

#include <algorithm>

namespace nue::android {

CoreMasks classify_cores(const std::vector<uint32_t>& khz) {
    CoreMasks m;
    const size_t n = std::min<size_t>(khz.size(), 64);
    m.total = static_cast<uint32_t>(n);
    for (size_t i = 0; i < n; ++i) {
        m.all |= (1ull << i);
        m.max_khz = std::max(m.max_khz, khz[i]);
    }
    if (m.max_khz == 0) return m;
    for (size_t i = 0; i < n; ++i) {
        if (khz[i] == 0) continue;
        const double ratio = double(khz[i]) / double(m.max_khz);
        if (ratio >= 0.80) {
            m.performance |= (1ull << i);
            ++m.performance_count;
        } else if (ratio <= 0.65) {
            m.efficiency |= (1ull << i);
            ++m.efficiency_count;
        }
    }
    m.hybrid = m.performance_count > 0 && m.efficiency_count > 0;
    return m;
}

int nice_from_priority(int p) {
    switch (p) {
    case 1: return 10;   // LOW      -> THREAD_PRIORITY_BACKGROUND
    case 3: return -2;   // ABOVE    -> THREAD_PRIORITY_FOREGROUND
    case 4: return -8;   // HIGH     -> THREAD_PRIORITY_URGENT_DISPLAY
    case 5: return -16;  // REALTIME -> THREAD_PRIORITY_AUDIO
    default: return 0;   // DEFAULT / NORMAL
    }
}

int priority_from_nice(int nice) {
    if (nice >= 5) return 1;
    if (nice <= -12) return 5;
    if (nice <= -6) return 4;
    if (nice <= -1) return 3;
    return 2;
}

} // namespace nue::android
