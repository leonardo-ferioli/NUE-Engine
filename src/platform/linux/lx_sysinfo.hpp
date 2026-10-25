// Parsers and collectors for /proc, /sys and /etc information. Everything reads through
// IFileSystem, so the same code runs on the real system and on captured fixtures.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lx_fs.hpp"

namespace nue::lx {

struct CpuInfoText {
    std::string model_name;  // x86 "model name", arm64 "Model"/"CPU part" description
    std::string vendor;      // GenuineIntel / AuthenticAMD / ARM implementer
    std::string flags;       // x86 "flags" or arm64 "Features"
    int processors = 0;      // number of "processor" stanzas
    std::vector<std::string> arm_parts;  // "implementer:part" per CPU for arm64 (e.g. "0x41:0xd0b")
    bool is_arm = false;
};
CpuInfoText parse_cpuinfo(const std::string& text);
bool cpu_has_flag(const CpuInfoText& c, const std::string& flag);

struct MemInfo {
    uint64_t total_kb = 0, free_kb = 0, available_kb = 0, swap_total_kb = 0, swap_free_kb = 0;
};
MemInfo parse_meminfo(const std::string& text);

struct OsRelease {
    std::string id, id_like, name, pretty_name, version_id, variant_id, build_id;
};
OsRelease parse_os_release(const std::string& text);

// /proc/stat aggregate line; busy fraction between two samples.
struct CpuTimes {
    uint64_t busy = 0, total = 0;
};
bool parse_proc_stat_cpu(const std::string& text, CpuTimes& out);
double cpu_busy_fraction(const CpuTimes& a, const CpuTimes& b);

// /proc/self/status (selected fields)
struct ProcStatus {
    uint64_t vm_rss_kb = 0, vm_peak_kb = 0;
    int threads = 0;
    std::string cpus_allowed_list;
};
ProcStatus parse_proc_status(const std::string& text);

// --- CPU topology ----------------------------------------------------------------------
enum class CoreClass : uint8_t { Efficiency = 0, Performance = 1, Prime = 2 };

struct LogicalCpu {
    int id = -1;
    int package = 0;
    int core = 0;                // core_id within the package
    std::vector<int> siblings;   // SMT siblings including itself
    CoreClass klass = CoreClass::Performance;
    int64_t max_khz = 0;         // cpuinfo_max_freq
    int capacity = 0;            // arm cpu_capacity (0 = unknown)
    std::string governor;        // scaling_governor
    bool online = true;
};

struct CpuTopology {
    std::vector<LogicalCpu> cpus;
    bool hybrid = false;         // more than one core class present
    std::string hybrid_source;   // "intel-sysfs", "arm-capacity", "max-freq", ""
    std::string governor;        // governor of cpu0 (representative)
    std::string epp;             // energy_performance_preference of cpu0 if present
    std::string scaling_driver;  // amd-pstate, intel_pstate, acpi-cpufreq, cpufreq-dt...
    std::vector<int> online_list;

    std::vector<int> cpus_of_class(CoreClass c) const;
    int physical_core_count() const;
};
CpuTopology read_cpu_topology(const IFileSystem& fs);

// Which CPUs the game should use. Performance cores first, one thread per core before SMT
// siblings; efficiency cores are kept for background work on hybrid parts.
struct AffinityPlan {
    std::vector<int> main_thread;   // single best CPU (or a small set)
    std::vector<int> render_thread;
    std::vector<int> workers;       // worker pool CPUs
    std::vector<int> background;    // loaders, audio mixing off the critical path, logging
    std::vector<int> all_allowed;   // what the process may run on (usually everything online)
    bool use_pinning = false;       // only recommended on hybrid CPUs; elsewhere the scheduler does better
};
AffinityPlan plan_affinity(const CpuTopology& t);

// --- battery / thermal ----------------------------------------------------------------
struct BatteryInfo {
    bool present = false;
    bool charging = false;
    bool full = false;
    int percent = -1;
    int64_t seconds_remaining = -1;
    std::string name;
};
struct PowerInfo {
    bool on_battery = false;  // no AC online and a battery discharging
    bool ac_online = false;
    std::vector<BatteryInfo> batteries;
    int best_percent() const;
};
PowerInfo read_power(const IFileSystem& fs);

struct ThermalReading {
    std::string source;  // zone type or hwmon name + label
    double celsius = 0;
};
std::vector<ThermalReading> read_thermal(const IFileSystem& fs);
double max_celsius(const std::vector<ThermalReading>& r);

// --- machine summary -------------------------------------------------------------------
struct MachineInfo {
    std::string kernel_release;  // /proc/sys/kernel/osrelease
    std::string arch;            // "x86_64" or "aarch64" (compile target, not measured)
    OsRelease os;
    CpuInfoText cpu;
    MemInfo mem;
    CpuTopology topo;
    std::string dmi_vendor, dmi_product, dmi_board;
    bool is_wsl = false;
    bool in_container = false;
};
MachineInfo read_machine_info(const IFileSystem& fs);

}  // namespace nue::lx
