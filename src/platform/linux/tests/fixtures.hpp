// In-memory machine fixtures: the subset of /proc, /sys and /etc that the Linux backend reads,
// shaped after real Steam Deck, Intel hybrid laptop and arm64 board layouts.
#pragma once
#include <string>

#include "lx_fs.hpp"

namespace fixtures {

using nue::lx::MemFs;

inline const char* kCpuInfoX86 =
    "processor\t: 0\nvendor_id\t: AuthenticAMD\ncpu family\t: 23\nmodel name\t: AMD Custom APU 0405\n"
    "flags\t\t: fpu vme sse sse2 avx avx2 fma bmi2\n\n"
    "processor\t: 1\nvendor_id\t: AuthenticAMD\ncpu family\t: 23\nmodel name\t: AMD Custom APU 0405\n"
    "flags\t\t: fpu vme sse sse2 avx avx2 fma bmi2\n";

inline const char* kCpuInfoArm =
    "processor\t: 0\nBogoMIPS\t: 108.00\nFeatures\t: fp asimd evtstrm crc32 cpuid\nCPU implementer\t: 0x41\n"
    "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0xd08\nCPU revision\t: 3\n\n"
    "processor\t: 1\nBogoMIPS\t: 108.00\nFeatures\t: fp asimd evtstrm crc32 cpuid\nCPU implementer\t: 0x41\nCPU part\t: 0xd08\n\n"
    "processor\t: 2\nFeatures\t: fp asimd\nCPU implementer\t: 0x41\nCPU part\t: 0xd08\n\n"
    "processor\t: 3\nFeatures\t: fp asimd\nCPU implementer\t: 0x41\nCPU part\t: 0xd08\n\n"
    "Hardware\t: BCM2711\nRevision\t: c03114\nModel\t\t: Raspberry Pi 4 Model B Rev 1.4\n";

inline void add_cpu(MemFs& fs, int id, int core, const std::string& siblings, int max_khz, const char* governor = "powersave") {
    const std::string b = "/sys/devices/system/cpu/cpu" + std::to_string(id) + "/";
    fs.add_file(b + "topology/physical_package_id", "0\n");
    fs.add_file(b + "topology/core_id", std::to_string(core) + "\n");
    fs.add_file(b + "topology/thread_siblings_list", siblings + "\n");
    if (max_khz) {
        fs.add_file(b + "cpufreq/cpuinfo_max_freq", std::to_string(max_khz) + "\n");
        fs.add_file(b + "cpufreq/scaling_governor", std::string(governor) + "\n");
    }
}

inline void steam_deck(MemFs& fs) {
    fs.add_file("/sys/class/dmi/id/sys_vendor", "Valve\n");
    fs.add_file("/sys/class/dmi/id/product_name", "Jupiter\n");
    fs.add_file("/sys/class/dmi/id/board_name", "Jupiter\n");
    fs.add_file("/etc/os-release", "NAME=\"SteamOS\"\nPRETTY_NAME=\"SteamOS Holo\"\nID=steamos\nID_LIKE=arch\nVARIANT_ID=steamdeck\n");
    fs.add_file("/proc/sys/kernel/osrelease", "6.5.0-valve22-1-neptune-65\n");
    fs.add_file("/proc/version", "Linux version 6.5.0-valve22-1-neptune-65 (gcc)\n");
    fs.add_file("/proc/cpuinfo", kCpuInfoX86);
    fs.add_file("/proc/meminfo", "MemTotal:       16384000 kB\nMemFree:         500000 kB\nMemAvailable:   9000000 kB\n");
    fs.add_file("/sys/devices/system/cpu/present", "0-7\n");
    fs.add_file("/sys/devices/system/cpu/online", "0-7\n");
    for (int i = 0; i < 8; ++i) {
        add_cpu(fs, i, i / 2, std::to_string(i & ~1) + "-" + std::to_string(i | 1), 3500000);
    }
    fs.add_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver", "amd-pstate-epp\n");
    fs.add_file("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", "balance_performance\n");

    fs.add_file("/sys/class/power_supply/BAT1/type", "Battery\n");
    fs.add_file("/sys/class/power_supply/BAT1/status", "Discharging\n");
    fs.add_file("/sys/class/power_supply/BAT1/capacity", "64\n");
    fs.add_file("/sys/class/power_supply/BAT1/energy_now", "20000000\n");
    fs.add_file("/sys/class/power_supply/BAT1/energy_full", "40000000\n");
    fs.add_file("/sys/class/power_supply/BAT1/power_now", "10000000\n");
    fs.add_file("/sys/class/power_supply/hid-28de:1205-battery/type", "Battery\n");
    fs.add_file("/sys/class/power_supply/hid-28de:1205-battery/scope", "Device\n");
    fs.add_file("/sys/class/power_supply/hid-28de:1205-battery/capacity", "50\n");

    fs.add_file("/sys/class/thermal/thermal_zone0/type", "acpitz\n");
    fs.add_file("/sys/class/thermal/thermal_zone0/temp", "55000\n");
    fs.add_file("/sys/class/hwmon/hwmon3/name", "k10temp\n");
    fs.add_file("/sys/class/hwmon/hwmon3/temp1_input", "70500\n");
    fs.add_file("/sys/class/hwmon/hwmon3/temp1_label", "Tctl\n");

    fs.add_file("/proc/bus/input/devices",
                "I: Bus=0003 Vendor=28de Product=1205 Version=0111\nN: Name=\"Valve Software Steam Deck Controller\"\nH: Handlers=event5 js0\n\n"
                "I: Bus=0003 Vendor=28de Product=11ff Version=0001\nN: Name=\"Microsoft X-Box 360 pad\"\nH: Handlers=event8 js1\n\n"
                "I: Bus=0011 Vendor=0001 Product=0001 Version=ab41\nN: Name=\"AT Translated Set 2 keyboard\"\nH: Handlers=sysrq kbd event3 leds\n");
    fs.add_file("/usr/lib/udev/rules.d/60-steam-input.rules", "# steam\n");
    fs.add_file("/usr/lib/udev/rules.d/70-uaccess.rules", "# uaccess\n");
}

inline void intel_hybrid(MemFs& fs) {
    fs.add_file("/sys/devices/system/cpu/present", "0-7\n");
    fs.add_file("/sys/devices/system/cpu/online", "0-7\n");
    add_cpu(fs, 0, 0, "0-1", 4800000);
    add_cpu(fs, 1, 0, "0-1", 4800000);
    add_cpu(fs, 2, 1, "2-3", 4800000);
    add_cpu(fs, 3, 1, "2-3", 4800000);
    for (int i = 4; i < 8; ++i) add_cpu(fs, i, 8 + (i - 4), std::to_string(i), 3500000);
    fs.add_file("/sys/devices/cpu_core/cpus", "0-3\n");
    fs.add_file("/sys/devices/cpu_atom/cpus", "4-7\n");
}

inline void arm_big_little(MemFs& fs) {
    fs.add_file("/sys/devices/system/cpu/present", "0-5\n");
    fs.add_file("/sys/devices/system/cpu/online", "0-5\n");
    for (int i = 0; i < 6; ++i) {
        add_cpu(fs, i, i, std::to_string(i), i < 4 ? 1800000 : 2200000, "schedutil");
        fs.add_file("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpu_capacity", i < 4 ? "512\n" : "1024\n");
    }
}

inline void add_gpu(MemFs& fs, const std::string& card, const char* vendor, const char* device, const char* driver, bool boot, const std::string& render) {
    const std::string d = "/sys/class/drm/" + card + "/device/";
    fs.add_file(d + "vendor", std::string(vendor) + "\n");
    fs.add_file(d + "device", std::string(device) + "\n");
    fs.add_file(d + "boot_vga", boot ? "1\n" : "0\n");
    fs.add_symlink(d + "driver", std::string("../../../../bus/pci/drivers/") + driver);
    fs.add_dir(d + "drm/" + render);
}

inline void hybrid_gpu_laptop(MemFs& fs) {
    add_gpu(fs, "card0", "0x8086", "0x9a49", "i915", true, "renderD128");
    add_gpu(fs, "card1", "0x10de", "0x2560", "nvidia", false, "renderD129");
    fs.add_dir("/sys/class/drm/card0-eDP-1");
    fs.add_file("/usr/share/vulkan/icd.d/intel_icd.x86_64.json",
                "{\n \"file_format_version\": \"1.0.0\",\n \"ICD\": {\n  \"library_path\": \"libvulkan_intel.so\",\n  \"api_version\": \"1.3.255\"\n }\n}\n");
    fs.add_file("/usr/share/vulkan/icd.d/nvidia_icd.json",
                "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"libGLX_nvidia.so.0\",\"api_version\":\"1.3.242\"}}");
}

inline void raspberry_pi3(MemFs& fs) {
    fs.add_file("/sys/class/drm/card0/device/uevent", "DRIVER=vc4-drm\nOF_NAME=gpu\n");
    fs.add_dir("/sys/class/drm/card0/device/drm/renderD128");
}

}  // namespace fixtures
