// Memory budget of the process.  The recompiled game needs a 32-bit address space image (about 1-1.3 GB
// resident with the engine, textures and audio), so the whole process is capped at 2 GiB by default.
// The budget shrinks with the device's RAM and with the pressure Android reports (onTrimMemory).
#pragma once
#include <cstdint>

namespace nue::android {

enum class MemPressure : int32_t { None = 0, Moderate, Low, Critical, Background };

struct SystemMemory {
    uint64_t total_ram = 0;          // ActivityManager.MemoryInfo.totalMem
    uint64_t available_ram = 0;      // availMem
    uint64_t low_threshold = 0;      // threshold: below this the system starts killing
    bool low_memory = false;
    int memory_class_mb = 0;         // getMemoryClass (Java heap limit; not the native limit)
    int large_memory_class_mb = 0;
    bool low_ram_device = false;
    uint64_t rss = 0;                // this process, /proc/self/statm
};

struct BudgetConfig {
    uint64_t process_cap = 0;              // hard ceiling for the whole process; 0 = none (a policy of the application, e.g. 2 GiB)
    double ram_fraction = 0.55;            // never plan to use more than this share of physical RAM
    uint64_t engine_reserve = 192ull << 20; // code, bgfx, driver, audio, stacks: not available for textures
    double soft_fraction = 0.85;           // shedding starts at soft = hard * this
};

struct Budget {
    uint64_t hard_limit = 0;
    uint64_t soft_limit = 0;
    uint64_t texture_budget = 0;
    uint64_t bytes_to_release = 0;
    MemPressure pressure = MemPressure::None;
};

// ComponentCallbacks2 levels: RUNNING_MODERATE 5, RUNNING_LOW 10, RUNNING_CRITICAL 15, UI_HIDDEN 20,
// BACKGROUND 40, MODERATE 60, COMPLETE 80.  -1 stands for onLowMemory().
MemPressure pressure_from_trim_level(int level);

Budget compute_budget(const SystemMemory& sys, MemPressure pressure, const BudgetConfig& cfg = {});

} // namespace nue::android
