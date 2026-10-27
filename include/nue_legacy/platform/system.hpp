// Platform API: what the machine is. Shaped like NUE's nue_system_get_info / nue_system_get_memory (include/nue/system.h): processor count and
// features, page size and memory, so GetSystemInfo, GlobalMemoryStatus, GetVersionEx and IsProcessorFeaturePresent are answered from here
// ("reports are shaped to what the guest expects": the Win32 layer decides which Windows it claims to be).
#pragma once
#include "nue_legacy/platform/types.hpp"
#include <string>

namespace nue::legacy::platform {

struct SystemInfo {
    uint32_t logical_processors = 1;
    uint32_t page_size = 4096;
    uint32_t allocation_granularity = 65536;
    uint64_t total_memory = 0, available_memory = 0;
    uint64_t total_virtual = 0, available_virtual = 0;   // address space of the process (a 32-bit guest sees at most 2 or 4 GiB)
    bool sse = true, sse2 = true, sse3 = false, ssse3 = false, sse41 = false, sse42 = false, avx = false, mmx = true, cmpxchg8b = true, rdtsc = true;
    // Processor features as a bit mask indexed by the Windows PF_* numbers (bit n = IsProcessorFeaturePresent(n)); filled from the host when it is Windows,
    // otherwise the baseline of an x86 PC (cmpxchg8b, MMX, SSE, SSE2, RDTSC, PAE, NX).
    uint64_t pf_mask = (1ull << 2) | (1ull << 3) | (1ull << 6) | (1ull << 8) | (1ull << 9) | (1ull << 10) | (1ull << 12);
    std::string cpu_vendor, cpu_name;
    std::string os_name;
    uint32_t ansi_code_page = 1252, oem_code_page = 437;   // the machine's legacy code pages (GetACP / GetOEMCP of a Windows host)
    uint32_t user_lcid = 0x0409;                           // the user's default locale as a Windows LCID
};

class System {
public:
    virtual ~System() = default;
    virtual SystemInfo info() = 0;                       // cheap; memory numbers refreshed at each call
};

}  // namespace nue::legacy::platform
