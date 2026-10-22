// NUE Linux platform layer: the nue_platform_ops table (see src/core/internal/platform_ops.h).
//
// The SDL3 backend gives windows, input and audio. This layer adds what SDL3 does not: system and
// GPU information, XDG storage roots, power/thermal/battery state, GameMode and power profiles,
// keep-awake through D-Bus services, thread priorities and affinity (nice, SCHED_*, rtkit,
// hybrid-core sets), the crash reporter and the syslog/journald log sink.
//
// Two build flavours share this file:
//   NUE_LINUX_RUNTIME defined   the real Linux build: services are reached with system calls and dlopen
//   NUE_LINUX_RUNTIME undefined host-test build (any OS): only the entries that read files through
//                               IFileSystem are provided, so they can be unit-tested against fixtures
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "internal/platform_ops.h"
#include "lx_diag.hpp"
#include "lx_fs.hpp"
#include "lx_gpu.hpp"
#include "lx_pathcase.hpp"
#include "lx_policy.hpp"
#include "lx_report.hpp"
#include "lx_signal_safe.hpp"
#include "lx_session.hpp"
#include "lx_steamos.hpp"
#include "lx_sysinfo.hpp"
#include "lx_text.hpp"
#include "lx_xdg.hpp"
#include "nue_ext_linux.h"
#include "platform_linux_test_hooks.h"

#if defined(NUE_LINUX_RUNTIME)
#include <signal.h>
#include <sys/prctl.h>
#include <syslog.h>
#include <unistd.h>

#include "lx_crash.hpp"
#include "lx_gamemode.hpp"
#include "lx_runtime.hpp"
#include "lx_sched.hpp"
#include "lx_steamos_runtime.hpp"
#endif

using namespace nue::lx;

extern "C" const void* nue_linux_get_extension_v1(void);

namespace {

constexpr const char* kTag = "linux";

// ------------------------------------------------------------------------------------------ helpers

nue_result out_string(const std::string& s, char* buffer, size_t capacity, size_t* out_size) {
    if (out_size) *out_size = s.size() + 1;
    if (buffer && capacity) {
        size_t n = std::min(s.size(), capacity - 1);
        std::memcpy(buffer, s.data(), n);
        buffer[n] = 0;
    }
    return NUE_OK;
}

template <size_t N>
void copy_cstr(char (&dst)[N], const std::string& s) {
    size_t n = std::min(s.size(), N - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

void set_cap(uint64_t (&caps)[2], nue_capability c) { caps[static_cast<unsigned>(c) / 64] |= 1ull << (static_cast<unsigned>(c) % 64); }

std::string sanitize_component(const char* in, const char* fallback) {
    std::string out;
    for (const char* p = in ? in : ""; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        out += (c == '/' || c == '\\' || c < 32 || c == ':') ? '_' : static_cast<char>(c);
    }
    while (!out.empty() && out[0] == '.') out.erase(0, 1);  // never a hidden or relative component
    return out.empty() ? std::string(fallback) : out;
}

const char* classify_locale_tag(std::string& v) {
    // "es_ES.UTF-8@euro" -> "es-ES"; "C" / "POSIX" -> "en-US"
    size_t cut = v.find_first_of(".@");
    if (cut != std::string::npos) v.resize(cut);
    for (char& c : v)
        if (c == '_') c = '-';
    if (v.empty() || v == "C" || v == "POSIX") v = "en-US";
    return v.c_str();
}

// ------------------------------------------------------------------------------------------ state

struct PollSnapshot {
    nue_power_info power{};
    bool valid = false;
};

struct State {
    // environment (replaceable by tests before init)
    StdFs real_fs;
    ProcessEnv real_env;
    const IFileSystem* fs = &real_fs;
    const IEnv* env = &real_env;

    SessionInfo session;
    MachineInfo machine;
    AffinityPlan affinity;
    XdgDirs xdg;
    std::vector<int> initial_affinity;
    bool runtime_enabled = false;
    bool hw_frozen = false;  // deterministic run on the real machine: never report its battery / thermal state
    SteamOsInfo steamos;
    std::string org = "nue", app = "app";  // from nue_init_desc, for the per-app directories

    // power
    std::mutex mtx;
    nue_power_request request{};
    PollSnapshot snap;
    nue_power_info last_pushed{};
    bool pushed_once = false;
    std::atomic<int> ppd_low_power{-1};  // -1 unknown, 0 no, 1 power-saver active (written by the poller)
    std::atomic<int> tdp_current{-1}, tdp_min{-1}, tdp_max{-1};  // watts from steamos-manager (written by the poller), -1 unknown
    bool glyph_override_set = false;
    unsigned pump_count = 0;
    std::thread poller;
    std::condition_variable cv;
    bool stop_poller = false;

    // PathResolver for the extension (created lazily per base directory)
    std::unique_ptr<PathResolver> resolver;
    std::string resolver_base;

#if defined(NUE_LINUX_RUNTIME)
    GameModeClient gamemode;
    PowerProfileHold ppd;
    PowerProfileHold ppd_probe;  // used by the poller thread only
    IdleInhibitor inhibit_display{IdleInhibitor::Kind::ScreenSaver};
    IdleInhibitor inhibit_system{IdleInhibitor::Kind::PowerManagement};
    bool profile_held = false;
    PowerProfile held_profile = PowerProfile::Balanced;
    std::string initial_cwd;
    SuspendWatcher suspend;
    unsigned long timer_slack_saved = 50000;
    bool timer_slack_changed = false;
#endif
    nue_crash_sink crash_sink{};
    nue_crash_desc crash_desc{};
    bool crash_installed = false;
};

State* g = nullptr;                 // valid between init and shutdown
const IFileSystem* g_test_fs = nullptr;
const IEnv* g_test_env = nullptr;
nue_platform_ops g_ops;             // filled once by nue_platform_linux_get_ops; capabilities are set in init

}  // namespace

// ------------------------------------------------------------------------------------------ thermal / power mapping

namespace {

nue_thermal_state classify_thermal(double celsius, bool known) {
    if (!known) return NUE_THERMAL_UNKNOWN;
    if (celsius >= 95.0) return NUE_THERMAL_CRITICAL;
    if (celsius >= 85.0) return NUE_THERMAL_SERIOUS;
    if (celsius >= 75.0) return NUE_THERMAL_FAIR;
    return NUE_THERMAL_NOMINAL;
}

// Reads sysfs now. Cheap (a few small files); the poller thread and nue_power_get_info use it.
void read_power_now(State& st, nue_power_info& p) {
    std::memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    if (st.hw_frozen) {  // reproducible runs: unknown power and thermal state, whatever the machine is doing
        p.battery_percent = -1;
        p.seconds_left = -1;
        p.thermal_headroom = -1.0f;
        p.state = NUE_POWER_UNKNOWN;
        p.thermal = NUE_THERMAL_UNKNOWN;
        return;
    }
    PowerInfo pi = read_power(*st.fs);
    const bool have_battery = !pi.batteries.empty();
    p.battery_percent = have_battery ? pi.best_percent() : -1;
    p.seconds_left = -1;
    p.thermal_headroom = -1.0f;
    for (const auto& b : pi.batteries)
        if (b.seconds_remaining >= 0 && (p.seconds_left < 0 || b.seconds_remaining < p.seconds_left)) p.seconds_left = static_cast<int32_t>(std::min<int64_t>(b.seconds_remaining, INT32_MAX));
    bool charging = false, full = false;
    for (const auto& b : pi.batteries) {
        charging = charging || b.charging;
        full = full || b.full;
    }
    if (!have_battery) p.state = NUE_POWER_NO_BATTERY;
    else if (charging) p.state = NUE_POWER_CHARGING;
    else if (full) p.state = NUE_POWER_CHARGED;
    else if (pi.on_battery) p.state = NUE_POWER_ON_BATTERY;
    else p.state = NUE_POWER_CHARGED;  // plugged in, not charging
    p.on_ac_power = pi.ac_online || !have_battery;

    auto th = read_thermal(*st.fs);
    p.thermal = classify_thermal(max_celsius(th), !th.empty());
    int lp = st.ppd_low_power.load();
    p.low_power_mode = lp > 0 ? NUE_TRUE : NUE_FALSE;
}

nue_game_mode_state current_game_mode(State& st) {
#if defined(NUE_LINUX_RUNTIME)
    if (st.gamemode.active()) return st.profile_held && st.held_profile == PowerProfile::Performance ? NUE_GAME_MODE_PERFORMANCE : NUE_GAME_MODE_ON;
    if (st.profile_held) return st.held_profile == PowerProfile::Performance ? NUE_GAME_MODE_PERFORMANCE : NUE_GAME_MODE_BATTERY;
    return NUE_GAME_MODE_OFF;
#else
    (void)st;
    return NUE_GAME_MODE_UNKNOWN;
#endif
}

void push_power_event(nue_event_type type, const nue_power_info& p) {
    nue_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.payload.power.thermal = p.thermal;
    ev.payload.power.power_state = p.state;
    ev.payload.power.percent = p.battery_percent;
    ev.payload.power.low_power = p.low_power_mode;
    ev.payload.power.game_mode = static_cast<uint8_t>(p.game_mode);
    nue_internal_push_event(&ev);
}

}  // namespace

// ------------------------------------------------------------------------------------------ system

namespace {

uint32_t cpu_feature_bits(const CpuInfoText& c) {
    uint32_t f = 0;
    if (cpu_has_flag(c, "sse2")) f |= NUE_CPU_SSE2;
    if (cpu_has_flag(c, "sse4_1")) f |= NUE_CPU_SSE41;
    if (cpu_has_flag(c, "sse4_2")) f |= NUE_CPU_SSE42;
    if (cpu_has_flag(c, "avx")) f |= NUE_CPU_AVX;
    if (cpu_has_flag(c, "avx2")) f |= NUE_CPU_AVX2;
    if (cpu_has_flag(c, "avx512f")) f |= NUE_CPU_AVX512;
    if (cpu_has_flag(c, "fma")) f |= NUE_CPU_FMA;
    if (cpu_has_flag(c, "popcnt")) f |= NUE_CPU_POPCNT;
    if (c.is_arm && (cpu_has_flag(c, "asimd") || cpu_has_flag(c, "neon"))) f |= NUE_CPU_NEON;  // Advanced SIMD is architectural on arm64
    if (c.is_arm) f |= NUE_CPU_NEON;
    return f;
}

nue_device_class device_class_of(State& st, bool battery) {
    if (st.session.steam_deck) return NUE_DEVICE_HANDHELD;
    uint64_t chassis = 0;
    parse_u64(st.fs->read_line("/sys/class/dmi/id/chassis_type"), chassis);
    switch (chassis) {
        case 8: case 9: case 10: case 14: case 31: case 32: return NUE_DEVICE_LAPTOP;
        case 11: return NUE_DEVICE_HANDHELD;
        case 30: return NUE_DEVICE_TABLET;
        case 3: case 4: case 5: case 6: case 7: case 13: case 15: case 16: case 23: case 24: return NUE_DEVICE_DESKTOP;
        default: break;
    }
    return battery ? NUE_DEVICE_LAPTOP : NUE_DEVICE_DESKTOP;
}

std::string device_model_of(State& st) {
    std::string m = st.fs->read_line("/sys/class/dmi/id/product_name");
    std::string family = st.fs->read_line("/sys/class/dmi/id/product_version");
    if (m.empty()) {  // device tree boards: Raspberry Pi, Rockchip, Qualcomm...
        std::string dt;
        if (st.fs->read_file("/proc/device-tree/model", dt, 256) || st.fs->read_file("/sys/firmware/devicetree/base/model", dt, 256)) {
            size_t nul = dt.find('\0');
            if (nul != std::string::npos) dt.resize(nul);
            return std::string(trim(dt));
        }
        return std::string();
    }
    return m;
}

uint64_t cgroup_memory_limit(State& st) {
    // cgroup v2 limit of this process (containers, Flatpak with limits, systemd slices); "max" = none
    std::string line = st.fs->read_line("/sys/fs/cgroup/memory.max");
    uint64_t v = 0;
    return parse_u64(line, v) ? v : 0;
}

nue_result fill_memory(State& st, uint64_t* total, uint64_t* available) {
    std::string text;
    if (!st.fs->read_file("/proc/meminfo", text)) return NUE_ERR_IO;
    MemInfo mi = parse_meminfo(text);
    uint64_t t = mi.total_kb * 1024, a = mi.available_kb * 1024;
    uint64_t limit = cgroup_memory_limit(st);
    if (limit && limit < t) {
        a = std::min(a, limit);  // the container cannot use more than its limit
        t = limit;
    }
    if (total) *total = t;
    if (available) *available = a;
    return NUE_OK;
}

uint32_t platform_flags_of(const State& st) {
    const SteamOsInfo& s = st.steamos;
    uint32_t f = 0;
    const bool deck = s.hardware == SteamHardware::DeckLcd || s.hardware == SteamHardware::DeckOled;
    if (deck) f |= NUE_PLATFORM_HANDHELD | NUE_PLATFORM_STEAM_DECK;
    if (s.steamos_like()) f |= NUE_PLATFORM_STEAMOS;
    if (s.hardware == SteamHardware::SteamMachine) f |= NUE_PLATFORM_STEAM_MACHINE;
    if (s.hardware == SteamHardware::SteamFrame) f |= NUE_PLATFORM_STEAM_FRAME;
    if (st.session.gamescope) f |= NUE_PLATFORM_GAMESCOPE;
    if (s.gaming_mode()) f |= NUE_PLATFORM_STEAM_GAMING_MODE;
    if (s.runtime_container) f |= NUE_PLATFORM_STEAM_RUNTIME;
    if (st.session.flatpak) f |= NUE_PLATFORM_FLATPAK;
    if (s.immutable_root) f |= NUE_PLATFORM_READ_ONLY_ROOT;
    if (!s.sd_cards.empty()) f |= NUE_PLATFORM_REMOVABLE_STORAGE;
    if (s.gaming_mode() && !deck && s.steam_hardware()) f |= NUE_PLATFORM_TV_UI;  // Steam Machine in gaming mode: couch UI on a TV
    return f;
}

std::string platform_variant_of(const State& st) {
    const SteamOsInfo& s = st.steamos;
    if (s.steam_hardware() && !s.hardware_is_heuristic) return to_string(s.hardware);
    if (s.steamos_like()) {
        std::string v = to_string(s.flavor);
        if (s.flavor == SteamOsFlavor::SteamOS && !s.distro_version.empty()) v += " " + s.distro_version;
        return v;
    }
    return std::string();
}

std::string runtime_name_of(const State& st) {
    if (st.session.steam_runtime) return "Steam Linux Runtime" + (st.session.steam_runtime_name.empty() ? std::string() : " (" + st.session.steam_runtime_name + ")");
    if (st.session.flatpak) return "Flatpak";
    if (st.session.snap) return "Snap";
    if (st.session.appimage) return "AppImage";
    return std::string();
}

nue_result p_system_info(nue_system_info* out) {
    if (!g || !out) return NUE_ERR_NOT_INITIALIZED;
    State& st = *g;
    out->os = NUE_OS_LINUX;
    copy_cstr(out->os_name, st.machine.os.pretty_name.empty() ? (st.machine.os.name.empty() ? std::string("Linux") : st.machine.os.name) : st.machine.os.pretty_name);
    copy_cstr(out->os_version, st.machine.kernel_release);
    out->os_build = 0;
    out->arch = st.machine.arch == "x86_64" ? NUE_ARCH_KIND_X64 : st.machine.arch == "aarch64" ? NUE_ARCH_KIND_ARM64 : NUE_ARCH_KIND_UNKNOWN;
    out->running_under_translation = NUE_FALSE;  // box64/FEX/qemu-user make the process look native from inside; no reliable marker
    PowerInfo pi = read_power(*st.fs);
    out->device_class = device_class_of(st, !pi.batteries.empty());
    copy_cstr(out->device_model, device_model_of(st));
    copy_cstr(out->cpu_name, st.machine.cpu.model_name);
    int online = 0;
    uint32_t perf = 0, eff = 0;
    int64_t max_khz = 0;
    for (const auto& c : st.machine.topo.cpus) {
        if (!c.online) continue;
        ++online;
        max_khz = std::max(max_khz, c.max_khz);
        if (c.klass == CoreClass::Efficiency) ++eff;
        else ++perf;
    }
    out->cpu_logical_cores = static_cast<uint32_t>(online ? online : st.machine.cpu.processors);
    out->cpu_physical_cores = static_cast<uint32_t>(st.machine.topo.physical_core_count());
    if (st.machine.topo.hybrid) {
        out->cpu_performance_cores = perf;
        out->cpu_efficiency_cores = eff;
    }
    out->cpu_max_mhz = static_cast<uint32_t>(max_khz / 1000);
    out->cpu_features = cpu_feature_bits(st.machine.cpu);
    fill_memory(st, &out->memory_total_bytes, &out->memory_available_bytes);
    if (NUE_STRUCT_HAS(out, nue_system_info, runtime_name)) {
        out->platform_flags = platform_flags_of(st);
        copy_cstr(out->platform_variant, platform_variant_of(st));
        copy_cstr(out->runtime_name, runtime_name_of(st));
    }

    out->gpu_count = 0;
    for (const auto& d : read_drm_devices(*st.fs)) {
        if (out->gpu_count >= NUE_MAX_GPUS) break;
        nue_gpu_info& gi = out->gpus[out->gpu_count++];
        std::memset(&gi, 0, sizeof gi);
        char id[24];
        std::snprintf(id, sizeof id, "%04x:%04x", d.vendor, d.device);
        copy_cstr(gi.name, std::string(d.platform ? "SoC GPU" : vendor_name(d.vendor)) + (d.kernel_driver.empty() ? "" : " (" + d.kernel_driver + ")") + (d.platform ? "" : std::string(" [") + id + "]"));
        gi.vendor_id = d.vendor;
        gi.device_id = d.device;
        gi.dedicated_memory_bytes = d.vram_bytes;
        gi.is_integrated = d.discrete_guess ? NUE_FALSE : NUE_TRUE;
        gi.is_default = d.boot_vga ? NUE_TRUE : NUE_FALSE;
    }
    if (out->gpu_count && std::none_of(out->gpus, out->gpus + out->gpu_count, [](const nue_gpu_info& x) { return x.is_default; })) out->gpus[0].is_default = NUE_TRUE;

    char loc[32];
    nue_result lr = g_ops.system_locale ? g_ops.system_locale(loc, sizeof loc) : NUE_ERR_UNSUPPORTED;
    if (NUE_SUCCEEDED(lr)) copy_cstr(out->locale, loc);
    return NUE_OK;
}

nue_result p_memory_info(uint64_t* total, uint64_t* available) {
    if (!g) return NUE_ERR_NOT_INITIALIZED;
    return fill_memory(*g, total, available);
}

nue_result p_system_locale(char* buffer, size_t capacity) {
    static const ProcessEnv process_env;
    const IEnv& env = g ? *g->env : static_cast<const IEnv&>(process_env);
    std::string v;
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANGUAGE", "LANG"}) {
        v = env.get_or(name);
        if (!v.empty()) break;
    }
    size_t colon = v.find(':');  // LANGUAGE is a priority list
    if (colon != std::string::npos) v.resize(colon);
    classify_locale_tag(v);
    return out_string(v, buffer, capacity, nullptr);
}

// ------------------------------------------------------------------------------------------ storage

nue_result p_storage_root(nue_root_kind kind, const nue_init_desc* desc, char* buffer, size_t capacity, size_t* out_size) {
    // Also answers before init (pre_init runs first and the core needs the roots then): resolve from the environment of that moment.
    struct Early {
        StdFs fs;
        ProcessEnv env;
        XdgDirs xdg;
    };
    std::unique_ptr<Early> early;
    if (!g) {
        early = std::make_unique<Early>();
        const IFileSystem& tfs = g_test_fs ? *g_test_fs : static_cast<const IFileSystem&>(early->fs);
        const IEnv& tenv = g_test_env ? *g_test_env : static_cast<const IEnv&>(early->env);
        early->xdg = resolve_xdg(tenv, tfs, tenv.get_or("HOME"));
    }
    const XdgDirs& xdg = g ? g->xdg : early->xdg;
    const IEnv& envr = g ? *g->env : (g_test_env ? *g_test_env : static_cast<const IEnv&>(early->env));
    const std::string org = sanitize_component(desc ? desc->org_name : nullptr, "nue");
    const std::string app = sanitize_component(desc ? desc->app_id : nullptr, "app");
    std::string path;
    // Portable mode: an explicit user_data_path keeps every writable root below it (core contract, tests/test_storage.cpp).
    if (desc && desc->user_data_path && *desc->user_data_path && (kind == NUE_ROOT_CACHE || kind == NUE_ROOT_SETTINGS || kind == NUE_ROOT_TEMP)) {
        const char* leaf = kind == NUE_ROOT_CACHE ? "cache" : kind == NUE_ROOT_SETTINGS ? "settings" : "temp";
        return out_string(std::string(desc->user_data_path) + "/" + leaf, buffer, capacity, out_size);
    }
    switch (kind) {
        case NUE_ROOT_DATA: {
            if (desc && desc->game_data_path && *desc->game_data_path) {
                path = desc->game_data_path;
            } else {
#if defined(NUE_LINUX_RUNTIME)
                char exe[4096];
                ssize_t n = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
                if (n > 0) {
                    exe[n] = 0;
                    std::string e(exe);
                    path = e.substr(0, e.rfind('/') == std::string::npos ? 0 : e.rfind('/'));
                }
#endif
                if (path.empty()) path = ".";
            }
            break;
        }
        case NUE_ROOT_USER:
            path = desc && desc->user_data_path && *desc->user_data_path ? std::string(desc->user_data_path) : xdg.data_home.empty() ? std::string() : xdg.data_home + "/" + org + "/" + app;
            break;
        case NUE_ROOT_CACHE: path = xdg.cache_home.empty() ? std::string() : xdg.cache_home + "/" + org + "/" + app; break;
        case NUE_ROOT_SETTINGS: path = xdg.config_home.empty() ? std::string() : xdg.config_home + "/" + org + "/" + app; break;
        case NUE_ROOT_TEMP: {
            // Per-user tmpfs when the session has one (cleared at logout, 0700), else /tmp/<org>-<uid>.
            if (!xdg.runtime_dir.empty()) {
                path = xdg.runtime_dir + "/" + org + "/" + app;
            } else {
                std::string base = envr.get_or("TMPDIR");
                if (base.empty() || base[0] != '/') base = "/tmp";
#if defined(NUE_LINUX_RUNTIME)
                path = base + "/" + org + "-" + std::to_string(static_cast<long>(::geteuid())) + "/" + app;
#else
                path = base + "/" + org + "/" + app;
#endif
            }
            break;
        }
        default: return NUE_ERR_INVALID_ARGUMENT;
    }
    if (path.empty()) return NUE_ERR_NOT_FOUND;  // no HOME and no override: nothing sensible to return
    return out_string(path, buffer, capacity, out_size);
}

// ------------------------------------------------------------------------------------------ power

nue_result p_power_info(nue_power_info* out) {
    if (!g || !out) return NUE_ERR_NOT_INITIALIZED;
    State& st = *g;
    nue_power_info p;
    read_power_now(st, p);
    p.game_mode = current_game_mode(st);
    p.tdp_watts_current = st.tdp_current.load();
    p.tdp_watts_min = st.tdp_min.load();
    p.tdp_watts_max = st.tdp_max.load();
    if (p.tdp_watts_current < 0) {  // no steamos-manager: the amdgpu power cap is the same limit when the OS programs one
        AmdGpuStatus gpu = read_amdgpu_status(*st.fs);
        if (gpu.present && gpu.power_cap_w > 0) p.tdp_watts_current = static_cast<int32_t>(gpu.power_cap_w + 0.5);
    }
#if defined(NUE_LINUX_RUNTIME)
    p.display_keep_awake_active = st.inhibit_display.held() ? NUE_TRUE : NUE_FALSE;
    p.suspend_supported = st.suspend.running() ? NUE_TRUE : NUE_FALSE;
#endif
    const uint32_t n = std::min<uint32_t>(out->struct_size, sizeof p);
    p.struct_size = n;
    std::memcpy(out, &p, n);
    return NUE_OK;
}

nue_result p_timer_resolution_get(uint32_t* out_us) {
    if (!out_us) return NUE_ERR_INVALID_ARGUMENT;
    *out_us = 1;  // hrtimers: nanosecond clocks, wake-up precision limited by timer slack (default 50 us)
    return NUE_OK;
}

#if defined(NUE_LINUX_RUNTIME)

void apply_gpu_environment(State& st, nue_gpu_preference pref, bool& hybrid_out);

nue_result p_power_apply(const nue_power_request* r, nue_power_applied* out_applied) {
    if (!g || !r) return NUE_ERR_NOT_INITIALIZED;
    State& st = *g;
    std::lock_guard<std::mutex> lock(st.mtx);
    uint32_t asked = 0, applied = 0;

    // ---- profile (power-profiles-daemon)
    const bool want_perf = r->profile == NUE_PERF_HIGH || r->profile == NUE_PERF_SUSTAINED;
    const bool want_saver = r->profile == NUE_PERF_POWER_SAVER;
    if (r->profile != NUE_PERF_DEFAULT) asked |= NUE_POWER_APPLIED_PROFILE;
    if (want_perf || want_saver) {
        PowerProfile p = want_saver ? PowerProfile::PowerSaver : PowerProfile::Performance;
        if (st.profile_held && st.held_profile != p) {
            st.ppd.release();
            st.profile_held = false;
        }
        if (!st.profile_held && st.ppd.hold(p, "NUE game", st.session.flatpak_id.empty() ? "nue" : st.session.flatpak_id)) {
            st.profile_held = true;
            st.held_profile = p;
        }
        if (st.profile_held) applied |= NUE_POWER_APPLIED_PROFILE;
    } else {
        if (st.profile_held) {
            st.ppd.release();
            st.profile_held = false;
        }
        if (r->profile == NUE_PERF_BALANCED && st.ppd.available()) applied |= NUE_POWER_APPLIED_PROFILE;  // nothing held = daemon default
    }

    // ---- game mode (Feral GameMode)
    const bool gm_wanted = r->game_hint == NUE_GAME_HINT_GAMEPLAY || r->game_hint == NUE_GAME_HINT_LOADING || (r->game_hint == NUE_GAME_HINT_NONE && want_perf);
    if (r->game_hint != NUE_GAME_HINT_NONE) asked |= NUE_POWER_APPLIED_GAME_HINT;
    if (gm_wanted && !st.gamemode.active()) st.gamemode.start();
    if (!gm_wanted && st.gamemode.active()) st.gamemode.stop();
    if (r->game_hint != NUE_GAME_HINT_NONE && (st.gamemode.active() == gm_wanted)) {
        // MENU/PAUSED are honoured by leaving game mode; GAMEPLAY/LOADING only if the daemon accepted us
        if (gm_wanted ? st.gamemode.active() : true) applied |= NUE_POWER_APPLIED_GAME_HINT;
    }

    // ---- keep awake
    if (r->keep_awake != NUE_KEEP_AWAKE_NONE) asked |= NUE_POWER_APPLIED_KEEP_AWAKE;
    bool keep_ok = r->keep_awake != NUE_KEEP_AWAKE_NONE;
    if (r->keep_awake & NUE_KEEP_AWAKE_DISPLAY) keep_ok = st.inhibit_display.acquire("nue", "Playing a game") && keep_ok;
    else st.inhibit_display.release();
    if (r->keep_awake & NUE_KEEP_AWAKE_SYSTEM) keep_ok = st.inhibit_system.acquire("nue", "Playing a game") && keep_ok;
    else st.inhibit_system.release();
    if (r->keep_awake != NUE_KEEP_AWAKE_NONE && keep_ok) applied |= NUE_POWER_APPLIED_KEEP_AWAKE;

    // ---- GPU (takes effect at the next device creation)
    if (r->gpu != NUE_GPU_DEFAULT) asked |= NUE_POWER_APPLIED_GPU;
    {
        bool hybrid = false;
        apply_gpu_environment(st, r->gpu, hybrid);
        if (r->gpu != NUE_GPU_DEFAULT && hybrid) applied |= NUE_POWER_APPLIED_GPU;
    }

    // ---- CPU class for the whole process
    if (r->cpu != NUE_CPU_DEFAULT) asked |= NUE_POWER_APPLIED_CPU;
    {
        std::vector<int> target;
        if (r->cpu == NUE_CPU_PERFORMANCE_CORES && st.machine.topo.hybrid) {
            target = st.machine.topo.cpus_of_class(CoreClass::Performance);
            auto prime = st.machine.topo.cpus_of_class(CoreClass::Prime);
            target.insert(target.end(), prime.begin(), prime.end());
        } else if (r->cpu == NUE_CPU_EFFICIENCY_CORES && st.machine.topo.hybrid) {
            target = st.machine.topo.cpus_of_class(CoreClass::Efficiency);
        } else {
            target = st.initial_affinity;
        }
        if (!target.empty() && set_process_affinity(target) && r->cpu != NUE_CPU_DEFAULT && st.machine.topo.hybrid) applied |= NUE_POWER_APPLIED_CPU;
    }

    // ---- timer resolution (timer slack of the calling thread, inherited by threads it creates later)
    if (r->timer_resolution_us) asked |= NUE_POWER_APPLIED_TIMER;
    if (r->timer_resolution_us) {
        if (!st.timer_slack_changed) {
            long cur = ::prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
            if (cur > 0) st.timer_slack_saved = static_cast<unsigned long>(cur);
        }
        if (set_timer_slack_ns(1000)) {
            st.timer_slack_changed = true;
            applied |= NUE_POWER_APPLIED_TIMER;
        }
    } else if (st.timer_slack_changed) {
        set_timer_slack_ns(st.timer_slack_saved);
        st.timer_slack_changed = false;
    }

    // ---- fields with no Linux equivalent are asked-but-not-applied so the caller sees the truth
    // The engine never writes the TDP: it belongs to the player (Steam quick access menu); a request is reported as not applied.
    if (NUE_STRUCT_HAS(r, nue_power_request, tdp_watts) && r->tdp_watts) asked |= NUE_POWER_APPLIED_TDP;
    if (r->target_frame_time_us) asked |= NUE_POWER_APPLIED_FRAME_TIME;
    if (r->disable_power_throttling) asked |= NUE_POWER_APPLIED_THROTTLING;
    if (r->prefer_low_latency_audio) asked |= NUE_POWER_APPLIED_AUDIO;

    st.request = *r;
    if (out_applied) *out_applied = applied;
    if (asked == 0) return NUE_OK;
    if (applied == asked) return NUE_OK;
    return applied ? NUE_OK_PARTIAL : NUE_ERR_UNSUPPORTED;
}

void apply_gpu_environment(State& st, nue_gpu_preference pref, bool& hybrid_out) {
    static const char* const kVars[] = {"MESA_VK_DEVICE_SELECT", "MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "DRI_PRIME",
                                        "__NV_PRIME_RENDER_OFFLOAD", "__VK_LAYER_NV_optimus", "__GLX_VENDOR_LIBRARY_NAME"};
    GraphicsProbe gp;
    gp.drm = read_drm_devices(*st.fs);
    gp.icds = read_vulkan_icds(*st.env, *st.fs, st.xdg);
    gp.gl.gl = gp.gl.egl = gp.gl.gles2 = true;  // only the device choice matters here
    PlanRequest req;
    req.prefer_low_power = pref == NUE_GPU_LOW_POWER;
    RendererPlan plan = plan_renderer(gp, req, st.session);
    hybrid_out = plan.hybrid_gpu;
    for (const char* v : kVars) ::unsetenv(v);  // DEFAULT undoes what an earlier request set
    if (pref == NUE_GPU_DEFAULT || !plan.hybrid_gpu) return;
    for (const auto& kv : plan.env) ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
    NUE_LOGI(kTag, "GPU preference %s: %s", pref == NUE_GPU_LOW_POWER ? "low power" : "high performance", plan.device_name.c_str());
}

nue_result p_window_set_gpu_preference(nue_gpu_preference pref) {
    if (!g) return NUE_ERR_NOT_INITIALIZED;
    bool hybrid = false;
    std::lock_guard<std::mutex> lock(g->mtx);
    apply_gpu_environment(*g, pref, hybrid);
    return hybrid || pref == NUE_GPU_DEFAULT ? NUE_OK : NUE_OK_NOOP;
}

#endif  // NUE_LINUX_RUNTIME

// ------------------------------------------------------------------------------------------ threads

#if defined(NUE_LINUX_RUNTIME)

// Returns false if the reference cannot be turned into a tid.
bool ref_to_tid(const nue_native_thread_ref* ref, int64_t& tid) {
    if (!ref || (ref->id == 0 && ref->handle == 0)) { tid = 0; return true; }  // calling thread
    if (ref->id != 0) { tid = static_cast<int64_t>(ref->id); return true; }
    return false;  // a pthread_t alone cannot be mapped to a tid portably
}

std::mutex g_nice_mtx;
std::vector<std::pair<int64_t, int>> g_original_nice;

int original_nice_of(int64_t tid, int current) {
    std::lock_guard<std::mutex> l(g_nice_mtx);
    int64_t key = tid > 0 ? tid : current_tid();
    for (auto& e : g_original_nice)
        if (e.first == key) return e.second;
    g_original_nice.emplace_back(key, current);
    return current;
}

nue_result p_thread_set_priority(const nue_native_thread_ref* ref, nue_thread_priority priority) {
    int64_t tid;
    if (!ref_to_tid(ref, tid)) return NUE_ERR_UNSUPPORTED;
    int cur_nice = 0;
    if (!get_thread_nice(cur_nice, tid)) return NUE_ERR_NOT_FOUND;
    const int orig = original_nice_of(tid, cur_nice);
    const PriorityLimits lim = query_priority_limits();
    const uint64_t rt_tid = static_cast<uint64_t>(tid > 0 ? tid : current_tid());
    RtKit rk;
    auto try_nice = [&](int want, int rtkit_nice) {
        if (want >= cur_nice) return want == cur_nice || set_thread_nice(want, tid);  // keeping or raising niceness is always allowed
        const int n = std::max(want, lim.min_nice);
        if (n < cur_nice && set_thread_nice(n, tid)) return true;  // RLIMIT_NICE / CAP_SYS_NICE permit the boost
        return rk.make_high_priority(rt_tid, rtkit_nice);          // otherwise ask rtkit, which may refuse
    };
    switch (priority) {
        case NUE_THREAD_PRIORITY_DEFAULT:
        case NUE_THREAD_PRIORITY_NORMAL:
            set_thread_policy(SchedPolicy::Other, 0, tid);
            if (cur_nice < orig) set_thread_nice(orig, tid);  // undo our own boost: raising niceness is always allowed
            return NUE_OK;
        case NUE_THREAD_PRIORITY_LOW:
            if (!set_thread_policy(SchedPolicy::Batch, 0, tid)) return NUE_ERR_ACCESS_DENIED;
            set_io_priority_best_effort(6, tid);
            return NUE_OK;
        case NUE_THREAD_PRIORITY_ABOVE_NORMAL:
            set_thread_policy(SchedPolicy::Other, 0, tid);
            return try_nice(orig - 3, -3) ? NUE_OK : NUE_ERR_ACCESS_DENIED;
        case NUE_THREAD_PRIORITY_HIGH:
            set_thread_policy(SchedPolicy::Other, 0, tid);
            return try_nice(orig - 8, -8) ? NUE_OK : NUE_ERR_ACCESS_DENIED;
        case NUE_THREAD_PRIORITY_REALTIME: {
            if (lim.max_rt_priority > 0 && set_thread_policy(SchedPolicy::RoundRobin, std::min(10, lim.max_rt_priority), tid)) return NUE_OK;
            if (rk.make_realtime(rt_tid, 10)) return NUE_OK;
            // downgrade to the best non-real-time boost
            set_thread_policy(SchedPolicy::Other, 0, tid);
            return try_nice(orig - 10, -10) ? NUE_OK_PARTIAL : NUE_ERR_ACCESS_DENIED;
        }
    }
    return NUE_ERR_INVALID_ARGUMENT;
}

nue_result p_thread_get_priority(const nue_native_thread_ref* ref, nue_thread_priority* out) {
    int64_t tid;
    if (!out || !ref_to_tid(ref, tid)) return NUE_ERR_UNSUPPORTED;
    SchedPolicy pol;
    int rt = 0, nice = 0;
    if (!get_thread_policy(pol, rt, tid) || !get_thread_nice(nice, tid)) return NUE_ERR_NOT_FOUND;
    if (pol == SchedPolicy::Fifo || pol == SchedPolicy::RoundRobin) *out = NUE_THREAD_PRIORITY_REALTIME;
    else if (pol == SchedPolicy::Idle || pol == SchedPolicy::Batch || nice > 0) *out = NUE_THREAD_PRIORITY_LOW;
    else if (nice <= -6) *out = NUE_THREAD_PRIORITY_HIGH;
    else if (nice < 0) *out = NUE_THREAD_PRIORITY_ABOVE_NORMAL;
    else *out = NUE_THREAD_PRIORITY_NORMAL;
    return NUE_OK;
}

std::vector<int> mask_to_cpus(uint64_t mask) {
    std::vector<int> cpus;
    for (int i = 0; i < 64; ++i)
        if (mask & (1ull << i)) cpus.push_back(i);
    return cpus;
}

nue_result p_thread_set_affinity(const nue_native_thread_ref* ref, uint64_t mask) {
    int64_t tid;
    if (!ref_to_tid(ref, tid)) return NUE_ERR_UNSUPPORTED;
    std::vector<int> cpus = mask ? mask_to_cpus(mask) : g->initial_affinity;
    // only CPUs the process may use: sched_setaffinity rejects the rest and cgroups/cpusets can shrink the set
    std::vector<int> allowed = g->initial_affinity;
    cpus.erase(std::remove_if(cpus.begin(), cpus.end(), [&](int c) { return std::find(allowed.begin(), allowed.end(), c) == allowed.end(); }), cpus.end());
    if (cpus.empty()) return NUE_ERR_INVALID_ARGUMENT;
    return set_thread_affinity(cpus, tid) ? NUE_OK : NUE_ERR_ACCESS_DENIED;
}

nue_result p_thread_set_core_class(const nue_native_thread_ref* ref, nue_cpu_preference pref) {
    int64_t tid;
    if (!ref_to_tid(ref, tid)) return NUE_ERR_UNSUPPORTED;
    const CpuTopology& t = g->machine.topo;
    if (!t.hybrid) return pref == NUE_CPU_DEFAULT ? NUE_OK_NOOP : NUE_OK_NOOP;  // uniform CPU: every core is the same class
    std::vector<int> target;
    if (pref == NUE_CPU_PERFORMANCE_CORES) {
        target = t.cpus_of_class(CoreClass::Performance);
        auto prime = t.cpus_of_class(CoreClass::Prime);
        target.insert(target.end(), prime.begin(), prime.end());
    } else if (pref == NUE_CPU_EFFICIENCY_CORES) {
        target = t.cpus_of_class(CoreClass::Efficiency);
    } else {
        target = g->initial_affinity;
    }
    if (target.empty()) return NUE_ERR_UNSUPPORTED;
    return set_thread_affinity(target, tid) ? NUE_OK : NUE_ERR_ACCESS_DENIED;
}

nue_result p_thread_set_name(const char* name) { return name && set_thread_name(name) ? NUE_OK : NUE_ERR_FAILED; }

nue_result p_sleep_until(int64_t deadline_ns) {
    // The deadline is in the nue_time_ns() domain, whose epoch the core owns: translate through "now" on both clocks.
    const int64_t remaining = deadline_ns - nue_time_ns();
    if (remaining <= 0) return NUE_OK;
    sleep_until_ns(monotonic_ns() + remaining);
    return NUE_OK;
}

#endif  // NUE_LINUX_RUNTIME

// ------------------------------------------------------------------------------------------ crash

#if defined(NUE_LINUX_RUNTIME)

const nue_crash_sink* g_sink = nullptr;
const nue_crash_desc* g_crash_desc = nullptr;
char g_reason[96];
nue_crash_info g_info;

const char* crash_begin(int sig, void* siginfo_v, void* uc, void*) {
    if (!g_sink || !g_sink->begin) return nullptr;
    FixedWriter w(g_reason, sizeof g_reason - 1);
    w.str(signal_name(sig));
    w.str(" (signal ");
    w.dec(sig);
    w.chr(')');
    g_reason[w.size()] = 0;
    std::memset(&g_info, 0, sizeof g_info);
    g_info.struct_size = sizeof g_info;
    g_info.reason = g_reason;
    g_info.signal_or_code = static_cast<uint32_t>(sig);
    if (siginfo_v) g_info.fault_address = reinterpret_cast<uint64_t>(static_cast<siginfo_t*>(siginfo_v)->si_addr);
    (void)uc;
    if (g_crash_desc && g_crash_desc->guest_pc) g_info.fault_pc = g_crash_desc->guest_pc(g_crash_desc->guest_pc_userdata);
    const char* dir = g_sink->begin(&g_info, g_sink->userdata);
    g_info.report_dir = dir;
    return dir;
}

bool crash_end(int, void*) {
    return g_sink && g_sink->end ? g_sink->end(&g_info, g_sink->userdata) == NUE_TRUE : false;
}

nue_result p_crash_install(const nue_crash_desc* desc, const nue_crash_sink* sink) {
    if (!g || !desc || !sink) return NUE_ERR_INVALID_ARGUMENT;
    State& st = *g;
    st.crash_desc = *desc;
    st.crash_sink = *sink;
    g_sink = &st.crash_sink;
    g_crash_desc = &st.crash_desc;
    CrashOptions o;
    o.crash_dir = st.xdg.state_home.empty() ? "/tmp" : st.xdg.state_home;
    o.app_name = "nue";
    o.begin = crash_begin;
    o.end = crash_end;
    o.write_maps = true;
    o.raise_core_limit = true;
    if (!install_crash_handler(o)) return NUE_ERR_BACKEND;
    st.crash_installed = true;
    return NUE_OK;
}

void p_crash_uninstall() {
    if (!g || !g->crash_installed) return;
    uninstall_crash_handler();
    g->crash_installed = false;
    g_sink = nullptr;
    g_crash_desc = nullptr;
}

nue_result p_crash_write_dump(const char* directory, nue_crash_flags) {
    return write_stack_report(directory) ? NUE_OK : NUE_ERR_IO;
}

// ------------------------------------------------------------------------------------------ log

void p_log_write(nue_log_level level, const char* tag, const char* message) {
    int prio = LOG_INFO;
    switch (level) {
        case NUE_LOG_TRACE: case NUE_LOG_DEBUG: prio = LOG_DEBUG; break;
        case NUE_LOG_INFO: prio = LOG_INFO; break;
        case NUE_LOG_WARN: prio = LOG_WARNING; break;
        case NUE_LOG_ERROR: prio = LOG_ERR; break;
        case NUE_LOG_FATAL: prio = LOG_CRIT; break;
        default: return;
    }
    ::syslog(prio, "[%s] %s", tag ? tag : "", message ? message : "");  // journald receives syslog(3) on systemd systems
}

#endif  // NUE_LINUX_RUNTIME

// ------------------------------------------------------------------------------------------ poller / pump

void poller_main(State* st) {
    std::unique_lock<std::mutex> l(st->mtx);
    int tick = 0;  // counts cycles; the profile query runs every fifth
    while (!st->stop_poller) {
        l.unlock();
        nue_power_info p;
        read_power_now(*st, p);
#if defined(NUE_LINUX_RUNTIME)
        if (tick % 5 == 0 && st->runtime_enabled) {  // power-profiles-daemon: every ~10 s, off the event thread
            std::string active = st->ppd_probe.active_profile();
            if (!active.empty()) st->ppd_low_power = active == "power-saver" ? 1 : 0;
            if (st->steamos.steamos_like()) {
                SteamOsManagerInfo m = query_steamos_manager();
                auto find = [&](const char* suffix) {
                    for (const auto& kv : m.values)
                        if (ends_with(kv.first, suffix)) {
                            uint64_t v;
                            if (parse_u64(kv.second, v) && v < 1000) return static_cast<int>(v);
                        }
                    return -1;
                };
                if (m.available) {
                    st->tdp_current = find(".TdpLimit");
                    st->tdp_min = find(".TdpLimitMin");
                    st->tdp_max = find(".TdpLimitMax");
                }
            }
        }
#endif
        l.lock();
        st->snap.power = p;
        st->snap.valid = true;
        ++tick;
        (void)tick;  // only used by the runtime build (profile query cadence)
        st->cv.wait_for(l, std::chrono::seconds(2), [st] { return st->stop_poller; });
    }
}

void update_glyph_override(State& st) {
    // Steam Input shows the game a virtual Xbox pad: on a Deck the player holds a Steam Deck, so tell the core which glyphs to draw.
    const bool deck = st.steamos.hardware == SteamHardware::DeckLcd || st.steamos.hardware == SteamHardware::DeckOled;
    SteamInputPlan ip = plan_steam_input(st.steamos, st.session, *st.env, read_input_devices(*st.fs));
    const bool want = deck && (ip.active || ip.raw_deck_access);
    if (want && !st.glyph_override_set) {
        nue_input_set_glyph_override(NUE_GLYPHS_STEAM);
        st.glyph_override_set = true;
    } else if (!want && st.glyph_override_set) {
        nue_input_set_glyph_override(-1);
        st.glyph_override_set = false;
    }
}

void p_pump() {
    if (!g) return;
    State& st = *g;
    if (st.pump_count++ % 240 == 0) update_glyph_override(st);
    nue_power_info now{};
    {
        std::lock_guard<std::mutex> l(st.mtx);
        if (!st.snap.valid) return;
        now = st.snap.power;
    }
    now.game_mode = current_game_mode(st);
    if (!st.pushed_once) {
        st.last_pushed = now;
        st.pushed_once = true;
        return;
    }
    const nue_power_info& old = st.last_pushed;
    if (now.thermal != old.thermal) push_power_event(NUE_EVENT_THERMAL_CHANGED, now);
    if (now.low_power_mode != old.low_power_mode) push_power_event(NUE_EVENT_LOW_POWER_MODE_CHANGED, now);
    if (now.game_mode != old.game_mode) push_power_event(NUE_EVENT_GAME_MODE_CHANGED, now);
    if (now.state != old.state || now.battery_percent != old.battery_percent || now.on_ac_power != old.on_ac_power) push_power_event(NUE_EVENT_POWER_CHANGED, now);
    st.last_pushed = now;
}

#if defined(NUE_LINUX_RUNTIME)
// logind announced suspend / resume (watcher thread): tell the core the whole device sleeps, and move the app state so audio and GPU go quiet.
void on_system_sleep(bool sleeping) {
    nue_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.type = sleeping ? NUE_EVENT_SYSTEM_SUSPENDING : NUE_EVENT_SYSTEM_RESUMED;
    nue_internal_push_event(&ev);
    nue_internal_set_app_state(sleeping ? NUE_APP_STATE_BACKGROUND : NUE_APP_STATE_FOREGROUND);
}
#endif

nue_result p_init(const nue_backend_init* bi) {
    if (g) return NUE_OK_NOOP;
    auto* st = new State;
    if (g_test_fs) st->fs = g_test_fs;
    if (g_test_env) st->env = g_test_env;
    st->session = detect_session(*st->env, *st->fs);
    st->machine = read_machine_info(*st->fs);
    st->affinity = plan_affinity(st->machine.topo);
    st->xdg = resolve_xdg(*st->env, *st->fs, st->env->get_or("HOME"));
    st->hw_frozen = bi && bi->deterministic && !g_test_fs;
    st->steamos = detect_steamos(*st->env, *st->fs, st->session);
    if (bi && bi->desc) {
        if (bi->desc->org_name && *bi->desc->org_name) st->org = bi->desc->org_name;
        if (bi->desc->app_id && *bi->desc->app_id) st->app = bi->desc->app_id;
    }
    st->initial_affinity = st->machine.topo.online_list;
#if defined(NUE_LINUX_RUNTIME)
    st->runtime_enabled = !(bi && bi->deterministic);  // deterministic runs must not depend on the desktop's services
    std::vector<int> cur;
    if (get_thread_affinity(cur) && !cur.empty()) st->initial_affinity = cur;
    ::openlog("nue", LOG_PID, LOG_USER);
#endif
    (void)bi;

    uint64_t(&c)[2] = g_ops.capabilities;
    c[0] = c[1] = 0;
    if (!st->hw_frozen) {
        if (!read_power(*st->fs).batteries.empty()) set_cap(c, NUE_CAP_BATTERY);
        if (!read_thermal(*st->fs).empty()) set_cap(c, NUE_CAP_THERMAL_STATE);
    }
#if defined(NUE_LINUX_RUNTIME)
    set_cap(c, NUE_CAP_TIMER_RESOLUTION);  // needs power_apply, which only the runtime build provides
    set_cap(c, NUE_CAP_THREAD_PRIORITY);
    set_cap(c, NUE_CAP_THREAD_AFFINITY);
    set_cap(c, NUE_CAP_CRASH_HANDLER);
    if (st->machine.topo.hybrid) set_cap(c, NUE_CAP_CORE_CLASS);
    if (st->runtime_enabled) {
        PowerProfileHold ppd;
        const bool have_ppd = ppd.available();
        GameModeClient gm;
        const bool have_gm = gm.available();
        if (have_ppd) {
            set_cap(c, NUE_CAP_LOW_POWER_MODE);
            st->ppd_low_power = ppd.active_profile() == "power-saver" ? 1 : 0;
        }
        if (have_ppd || have_gm) set_cap(c, NUE_CAP_GAME_MODE);
        set_cap(c, NUE_CAP_KEEP_AWAKE);  // power_apply tries ScreenSaver + PowerManagement; the SDL backend adds its own inhibition
        // Suspend / resume: logind tells us before the system sleeps (laptops, Steam Deck). The game is moved to BACKGROUND so
        // audio and GPU go quiet, and back to FOREGROUND on resume.
        if (st->suspend.start(on_system_sleep))
            set_cap(c, NUE_CAP_LIFECYCLE_PAUSE);
    }
    if (read_drm_devices(*st->fs).size() > 1) set_cap(c, NUE_CAP_GPU_PREFERENCE);
#endif

    g = st;
    st->poller = std::thread(poller_main, st);
    nue_internal_register_extension("nue.linux.v1", nue_linux_get_extension_v1());
    NUE_LOGI(kTag, "Linux platform layer up: %s, %s%s%s", st->machine.os.pretty_name.c_str(), st->session.steam_deck ? "Steam Deck, " : "",
             st->session.gamescope ? "gamescope, " : "", st->session.flatpak ? "Flatpak" : (st->session.snap ? "Snap" : "native"));
    return NUE_OK;
}

void p_shutdown() {
    if (!g) return;
    State* st = g;
    {
        std::lock_guard<std::mutex> l(st->mtx);
        st->stop_poller = true;
    }
    st->cv.notify_all();
    if (st->poller.joinable()) st->poller.join();
#if defined(NUE_LINUX_RUNTIME)
    // Give back everything we asked the system for.
    st->suspend.stop();
    st->inhibit_display.release();
    st->inhibit_system.release();
    if (st->profile_held) st->ppd.release();
    st->gamemode.stop();
    if (st->timer_slack_changed) set_timer_slack_ns(st->timer_slack_saved);
    if (st->crash_installed) uninstall_crash_handler();
    ::closelog();
    bool dummy = false;
    apply_gpu_environment(*st, NUE_GPU_DEFAULT, dummy);
    if (!st->initial_affinity.empty()) set_process_affinity(st->initial_affinity);
#endif
    g = nullptr;
    delete st;
}

}  // namespace

// ------------------------------------------------------------------------------------------ extension

namespace {

PathResolver& resolver_for(const std::string& base) {
    State& st = *g;
    if (!st.resolver || st.resolver_base != base) {
        PathResolveOptions o;
        o.base_dir = base;
        st.resolver = std::make_unique<PathResolver>(*st.fs, o);
        st.resolver_base = base;
    }
    return *st.resolver;
}

nue_result x_resolve_path(const char* base_dir, const char* windows_path, char* buffer, size_t capacity, size_t* out_size, uint32_t* out_flags) {
    if (!g || !windows_path) return NUE_ERR_INVALID_ARGUMENT;
    std::string base = base_dir && *base_dir ? base_dir : std::string();
    if (base.empty()) {
#if defined(NUE_LINUX_RUNTIME)
        char cwd[4096];
        base = ::getcwd(cwd, sizeof cwd) ? cwd : "/";
#else
        base = "/";
#endif
    }
    std::lock_guard<std::mutex> l(g->mtx);
    ResolveResult r = resolver_for(base).resolve(windows_path);
    if (out_flags)
        *out_flags = (r.found ? NUE_LINUX_PATH_FOUND : 0u) | (r.case_changed ? NUE_LINUX_PATH_CASE_CHANGED : 0u) | (r.ambiguous ? NUE_LINUX_PATH_AMBIGUOUS : 0u) |
                     (r.parent_found ? NUE_LINUX_PATH_PARENT_EXISTS : 0u);
    out_string(r.path, buffer, capacity, out_size);
    return r.found ? NUE_OK : NUE_ERR_NOT_FOUND;
}

nue_result x_get_session(nue_linux_session_info* out) {
    if (!g || !out) return NUE_ERR_INVALID_ARGUMENT;
    const SessionInfo& s = g->session;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->wayland_session = s.wayland_session;
    out->x11_only = s.display_server == DisplayServer::X11 && !s.wayland_session;
    out->xwayland = s.xwayland;
    out->headless = s.headless;
    out->steam_deck = s.steam_deck;
    out->steamos = s.steamos;
    out->gamescope = s.gamescope;
    out->steam_client = s.steam_client;
    out->steam_runtime = s.steam_runtime;
    out->flatpak = s.flatpak;
    out->snap = s.snap;
    out->appimage = s.appimage;
    out->container = s.container;
    out->wsl = s.wsl;
    copy_cstr(out->deck_model, s.deck_model);
    copy_cstr(out->distro_id, s.distro_id);
    copy_cstr(out->steam_runtime_name, s.steam_runtime_name);
    return NUE_OK;
}

nue_result x_plan_renderer(nue_bool live, nue_linux_renderer_plan* out) {
    if (!g || !out) return NUE_ERR_INVALID_ARGUMENT;
    GraphicsProbe gp;
    gp.drm = read_drm_devices(*g->fs);
    gp.icds = read_vulkan_icds(*g->env, *g->fs, g->xdg);
    if (live) {
        gp.vk = probe_vulkan();
        gp.gl = probe_gl_libraries();
    } else {
        gp.gl.gl = gp.gl.egl = gp.gl.gles2 = true;  // unknown without loading them: assume present
    }
    RendererPlan p = plan_renderer(gp, PlanRequest(), g->session);
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    copy_cstr(out->api, to_string(p.api));
    copy_cstr(out->bgfx_renderer, bgfx_renderer_name(p.api));
    out->vendor_id = p.bgfx_vendor_id;
    out->device_id = p.bgfx_device_id;
    copy_cstr(out->device_name, p.device_name);
    out->software = p.software;
    out->hybrid_gpu = p.hybrid_gpu;
    std::string fb, env, warn;
    for (GfxApi a : p.fallbacks) fb += (fb.empty() ? "" : ",") + std::string(to_string(a));
    for (const auto& kv : p.env) env += kv.first + "=" + kv.second + "\n";
    for (const auto& w : p.warnings) warn += w + "\n";
    copy_cstr(out->fallbacks, fb);
    copy_cstr(out->environment, env);
    copy_cstr(out->warnings, warn);
    return NUE_OK;
}

nue_result x_describe(nue_bool live, char* buffer, size_t capacity, size_t* out_size) {
    if (!g) return NUE_ERR_NOT_INITIALIZED;
    ReportOptions o;
    o.live_graphics = live != 0;
    return out_string(build_system_report(*g->fs, *g->env, o), buffer, capacity, out_size);
}

// ---- SteamOS

int glyph_code(GlyphFamily f) {
    switch (f) {
        case GlyphFamily::KeyboardMouse: return NUE_LINUX_GLYPHS_KEYBOARD_MOUSE;
        case GlyphFamily::Xbox: return NUE_LINUX_GLYPHS_XBOX;
        case GlyphFamily::PlayStation: return NUE_LINUX_GLYPHS_PLAYSTATION;
        case GlyphFamily::Nintendo: return NUE_LINUX_GLYPHS_NINTENDO;
        case GlyphFamily::Steam: return NUE_LINUX_GLYPHS_STEAM;
        default: return NUE_LINUX_GLYPHS_GENERIC;
    }
}

nue_result x_get_steamos_info(nue_linux_steamos_info* out) {
    if (!g || !out) return NUE_ERR_INVALID_ARGUMENT;
    const SteamOsInfo& s = g->steamos;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->hardware = static_cast<nue_linux_steam_hardware>(static_cast<int>(s.hardware));  // same order as the C enum
    out->hardware_is_heuristic = s.hardware_is_heuristic;
    out->steamos_like = s.steamos_like();
    if (s.steamos_like()) copy_cstr(out->flavor, to_string(s.flavor));
    copy_cstr(out->distro_id, s.distro_id);
    copy_cstr(out->variant_id, s.variant_id);
    out->immutable_root = s.immutable_root;
    out->session_mode = static_cast<nue_linux_session_mode>(static_cast<int>(s.mode));
    out->launched_by_steam = s.launched_by_steam;
    out->non_steam_shortcut = s.non_steam_shortcut;
    out->proton = s.proton;
    out->steam_in_flatpak = s.steam_in_flatpak;
    out->steam_runtime_container = s.runtime_container;
    out->steam_game_id = s.game_id;
    copy_cstr(out->steam_root, s.steam_root);
    out->sd_card_count = static_cast<uint32_t>(s.sd_cards.size());
    if (!s.sd_cards.empty()) copy_cstr(out->first_sd_card, s.sd_cards[0]);
    const PanelInfo panel = panel_for(s.hardware);
    out->panel_width = panel.width;
    out->panel_height = panel.height;
    out->panel_min_hz = panel.min_hz;
    out->panel_max_hz = panel.max_hz;
    out->panel_oled = panel.oled;
    out->hdr_available = plan_display(s, g->session).hdr_available;
    SteamInputPlan ip = plan_steam_input(s, g->session, *g->env, read_input_devices(*g->fs));
    out->steam_input_active = ip.active;
    out->physical_pads_hidden = ip.physical_hidden;
    out->visible_gamepads = ip.visible_gamepads;
    out->raw_deck_access = ip.raw_deck_access;
    out->gyro_via_sdl = ip.gyro_via_sdl;
    out->back_buttons_via_sdl = ip.back_buttons_via_sdl;
    out->default_glyphs = static_cast<nue_linux_glyph_family>(glyph_code(ip.default_glyphs));
    return NUE_OK;
}

nue_result x_plan_frame_pacing(int32_t game_fps, double display_hz, nue_linux_frame_pacing* out) {
    if (!g || !out || game_fps <= 0) return NUE_ERR_INVALID_ARGUMENT;
    FramePacingRequest r;
    r.game_fps = game_fps;
    r.display_hz = display_hz;
    FramePacing p = plan_frame_pacing(g->steamos, g->session, r);
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->target_fps = p.target_fps;
    out->present_vsync = p.present_vsync;
    out->own_limiter = p.own_limiter;
    out->judder_expected = p.judder_expected;
    out->suggested_steam_limit = p.suggested_steam_limit;
    std::string notes;
    for (const auto& n : p.notes) notes += (notes.empty() ? "" : "; ") + n;
    copy_cstr(out->notes, notes);
    return NUE_OK;
}

nue_result x_suggest_fps_cap(int32_t wanted_fps, int32_t* out_fps) {
    if (!g || !out_fps) return NUE_ERR_INVALID_ARGUMENT;
    nue_power_info pi;
    read_power_now(*g, pi);
    *out_fps = suggest_fps_cap(g->steamos, read_power(*g->fs), pi.thermal >= NUE_THERMAL_SERIOUS, wanted_fps);
    return NUE_OK;
}

nue_result x_controller_glyph(uint32_t vendor_id, uint32_t product_id, int32_t* out_family) {
    if (!g || !out_family) return NUE_ERR_INVALID_ARGUMENT;
    SteamInputPlan ip = plan_steam_input(g->steamos, g->session, *g->env, read_input_devices(*g->fs));
    const bool deck = g->steamos.hardware == SteamHardware::DeckLcd || g->steamos.hardware == SteamHardware::DeckOled;
    *out_family = glyph_code(glyph_family_for(static_cast<uint16_t>(vendor_id), static_cast<uint16_t>(product_id), ip.active, deck));
    return NUE_OK;
}

nue_result x_show_onscreen_keyboard() {
#if defined(NUE_LINUX_RUNTIME)
    if (!g) return NUE_ERR_NOT_INITIALIZED;
    KeyboardRequest k = plan_onscreen_keyboard(g->steamos, *g->fs, *g->env);
    if (k.argv.empty()) return NUE_ERR_UNSUPPORTED;
    return spawn_detached(k.argv) ? NUE_OK : NUE_ERR_FAILED;
#else
    return NUE_ERR_UNSUPPORTED;
#endif
}

nue_result x_shader_cache_dir(char* buffer, size_t capacity, size_t* out_size) {
    if (!g) return NUE_ERR_NOT_INITIALIZED;
    SteamOsPaths p = plan_steamos_paths(g->steamos, g->xdg, *g->env, sanitize_component(g->org.c_str(), "nue"), sanitize_component(g->app.c_str(), "app"));
    if (p.shader_cache.empty()) return NUE_ERR_NOT_FOUND;
    return out_string(p.shader_cache, buffer, capacity, out_size);
}

nue_result x_find_steam_app(uint64_t app_id, char* buffer, size_t capacity, size_t* out_size) {
    if (!g) return NUE_ERR_NOT_INITIALIZED;
    std::string dir = find_app_install(*g->fs, steam_library_paths(*g->fs, g->steamos.steam_root), app_id);
    if (dir.empty()) return NUE_ERR_NOT_FOUND;
    return out_string(dir, buffer, capacity, out_size);
}

nue_result x_describe_power_tuning(char* buffer, size_t capacity, size_t* out_size) {
    if (!g) return NUE_ERR_NOT_INITIALIZED;
    std::string text;
    AmdGpuStatus a = read_amdgpu_status(*g->fs);
    if (a.present) {
        text += "amdgpu.card=" + a.card + "\namdgpu.busy_percent=" + std::to_string(a.busy_percent) + "\namdgpu.performance_level=" + a.perf_level +
                "\namdgpu.sclk_mhz=" + std::to_string(a.sclk_mhz) + "\namdgpu.mclk_mhz=" + std::to_string(a.mclk_mhz) +
                "\namdgpu.power_cap_w=" + std::to_string(a.power_cap_w) + "\namdgpu.power_now_w=" + std::to_string(a.power_now_w) +
                "\namdgpu.temp_c=" + std::to_string(a.temp_c) + "\n";
    }
#if defined(NUE_LINUX_RUNTIME)
    if (g->runtime_enabled) {
        SteamOsManagerInfo m = query_steamos_manager();
        text += std::string("steamos_manager.available=") + (m.available ? "1" : "0") + "\n";
        for (const auto& kv : m.values) text += "steamos_manager." + kv.first + "=" + kv.second + "\n";
    }
#endif
    return out_string(text, buffer, capacity, out_size);
}

// ---- hooks added in API 0.1.1

nue_result p_pre_init(const nue_backend_init* init) {
    uint32_t flags = 0;
    if (init && init->headless) flags |= NUE_LINUX_POLICY_HEADLESS;
    nue_linux_apply_early_policy(flags);  // audio / video driver order, latency, joystick hints; never overrides what the user exported
    return NUE_OK;
}

nue_result p_screen_keyboard(const nue_native_window*, nue_bool show) {
    // Steam's keyboard closes itself when the player confirms or cancels; there is no portable "hide" request to send.
    if (!show) return NUE_OK_NOOP;
    return x_show_onscreen_keyboard();
}

nue_result p_memory_info_ex(nue_memory_info* io) {
    if (!g || !io) return NUE_ERR_NOT_INITIALIZED;
    State& st = *g;
    std::string text;
    if (st.fs->read_file("/proc/self/status", text, 65536)) io->process_used = parse_proc_status(text).vm_rss_kb * 1024;
    const uint64_t limit = cgroup_memory_limit(st);
    if (limit && (io->physical_total == 0 || limit < io->physical_total)) io->process_limit = limit;
    uint64_t avail = io->physical_available;
    if (io->process_limit && io->process_limit > io->process_used) {
        const uint64_t headroom = io->process_limit - io->process_used;
        avail = avail ? std::min(avail, headroom) : headroom;
    }
    io->process_available = avail;

    // GPU memory: amdgpu exposes VRAM and GTT in sysfs. Integrated parts share system memory.
    std::vector<DrmDevice> gpus = read_drm_devices(*st.fs);
    for (const auto& d : gpus) {
        if (d.platform) { io->unified_memory = NUE_TRUE; continue; }
        const std::string base = "/sys/class/drm/" + d.card + "/device/";
        uint64_t v;
        const bool integrated = !d.discrete_guess;
        if (integrated) io->unified_memory = NUE_TRUE;
        if (d.vendor == kVendorAmd) {
            if (parse_u64(st.fs->read_line(base + "mem_info_gtt_total"), v)) io->gpu_shared_total = std::max(io->gpu_shared_total, v);
            if (!integrated && d.vram_bytes) {
                io->gpu_dedicated_total = d.vram_bytes;
                if (parse_u64(st.fs->read_line(base + "mem_info_vram_used"), v) && v <= d.vram_bytes) io->gpu_dedicated_available = d.vram_bytes - v;
            }
        }
    }
    // Pressure: PSI "some avg10" is the percentage of the last 10 s in which tasks stalled on memory.
    std::string psi = st.fs->read_line("/proc/pressure/memory");
    size_t p = psi.find("avg10=");
    if (p != std::string::npos) {
        double avg = std::atof(psi.c_str() + p + 6);
        io->pressure = avg >= 40.0 ? NUE_PRESSURE_CRITICAL : avg >= 15.0 ? NUE_PRESSURE_HIGH : avg >= 3.0 ? NUE_PRESSURE_MODERATE : NUE_PRESSURE_NONE;
    }
    return NUE_OK;
}

struct FamilyFormat {
    nue_format_family family;
    int32_t vk_format;  // VkFormat, 0 = no Vulkan equivalent
};
const FamilyFormat kFamilyFormats[] = {
    {NUE_FMT_RGBA8, 37}, {NUE_FMT_BGRA8, 44}, {NUE_FMT_RGB565, 4}, {NUE_FMT_RGBA4444, 2}, {NUE_FMT_RGB5A1, 6}, {NUE_FMT_R8, 9}, {NUE_FMT_RG8, 16},
    {NUE_FMT_R16F, 76}, {NUE_FMT_RGBA16F, 97}, {NUE_FMT_RGBA32F, 109}, {NUE_FMT_RGB10A2, 64}, {NUE_FMT_DEPTH16, 124}, {NUE_FMT_DEPTH24_STENCIL8, 129},
    {NUE_FMT_DEPTH32F, 126}, {NUE_FMT_DEPTH32F_STENCIL8, 130},
    {NUE_FMT_BC1, 133}, {NUE_FMT_BC2, 135}, {NUE_FMT_BC3, 137}, {NUE_FMT_BC4, 139}, {NUE_FMT_BC5, 141}, {NUE_FMT_BC6H, 143}, {NUE_FMT_BC7, 145},
    {NUE_FMT_ETC1, 147}, {NUE_FMT_ETC2_RGB, 147}, {NUE_FMT_ETC2_RGBA, 151}, {NUE_FMT_EAC_R11, 153}, {NUE_FMT_EAC_RG11, 155},
    {NUE_FMT_ASTC_LDR, 157}, {NUE_FMT_ASTC_HDR, 1000066000}, {NUE_FMT_PVRTC1, 1000054000}, {NUE_FMT_PVRTC2, 1000054002},
};

nue_format_support support_from_vk_features(uint32_t f) {
    nue_format_support s = 0;
    if (f & 0x1u) s |= NUE_FMTSUP_SAMPLE | NUE_FMTSUP_MIPMAPS;
    if (f & 0x1000u) s |= NUE_FMTSUP_FILTER;
    if (f & (0x80u | 0x200u)) s |= NUE_FMTSUP_RENDER_TARGET;
    if (f & 0x100u) s |= NUE_FMTSUP_BLEND;
    if (f & 0x2u) s |= NUE_FMTSUP_STORAGE;
    return s;
}

nue_result p_gpu_probe(nue_gpu_caps* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep;
    out->renderer = NUE_RENDERER_AUTO;
    out->probed = NUE_FALSE;
    out->npot_mipmaps = out->texture_compare = out->instancing = NUE_TRUE;

    std::vector<int32_t> fmts;
    for (const auto& f : kFamilyFormats) fmts.push_back(f.vk_format);
    VulkanCaps vk = probe_vulkan_caps(0, 0, fmts);
    if (vk.ok && !(vk.device_type == 4)) {
        out->probed = NUE_TRUE;
        out->renderer = NUE_RENDERER_VULKAN;
        for (size_t i = 0; i < vk.format_features.size() && i < sizeof kFamilyFormats / sizeof kFamilyFormats[0]; ++i)
            out->format[kFamilyFormats[i].family] = support_from_vk_features(vk.format_features[i].second);
        out->max_texture_2d = vk.max_texture_2d;
        out->max_texture_3d = vk.max_texture_3d;
        out->max_texture_cube = vk.max_texture_cube;
        out->max_array_layers = vk.max_array_layers;
        // maxSamplerAnisotropy, maxColorAttachments and the sample count masks are further inside VkPhysicalDeviceLimits and are not read:
        // report the common values only when the feature exists, and the Vulkan-guaranteed minimum for the rest.
        out->max_anisotropy = vk.sampler_anisotropy ? 16 : 1;
        out->max_render_targets = 4;
        out->max_msaa_samples = 4;
        out->compute = NUE_TRUE;
        out->tessellation = vk.tessellation_shader ? NUE_TRUE : NUE_FALSE;
        out->unified_memory = vk.device_type == 1 ? NUE_TRUE : NUE_FALSE;
        out->dedicated_memory_bytes = vk.device_type == 2 ? vk.device_local_bytes : 0;
        return NUE_OK;
    }
    // No Vulkan device: a conservative desktop table. Uncompressed formats and the S3TC / BPTC families are universal on desktop GL drivers.
    static const nue_format_family basic[] = {NUE_FMT_RGBA8, NUE_FMT_BGRA8, NUE_FMT_R8, NUE_FMT_RG8, NUE_FMT_RGBA16F, NUE_FMT_DEPTH24_STENCIL8, NUE_FMT_BC1, NUE_FMT_BC2, NUE_FMT_BC3, NUE_FMT_BC4, NUE_FMT_BC5};
    for (nue_format_family f : basic) out->format[f] = NUE_FMTSUP_SAMPLE | NUE_FMTSUP_FILTER | NUE_FMTSUP_MIPMAPS;
    out->max_texture_2d = out->max_texture_cube = 8192;
    out->max_texture_3d = 2048;
    out->max_array_layers = 256;
    out->max_anisotropy = 8;
    out->max_render_targets = 4;
    out->max_msaa_samples = 4;
    return NUE_OK;
}

nue_ext_linux_v1 g_ext;

}  // namespace

extern "C" const void* nue_linux_get_extension_v1(void) {
    std::memset(&g_ext, 0, sizeof g_ext);
    g_ext.struct_size = sizeof g_ext;
    g_ext.resolve_path = x_resolve_path;
    g_ext.get_session = x_get_session;
    g_ext.plan_renderer = x_plan_renderer;
    g_ext.describe = x_describe;
    g_ext.get_steamos_info = x_get_steamos_info;
    g_ext.plan_frame_pacing = x_plan_frame_pacing;
    g_ext.suggest_fps_cap = x_suggest_fps_cap;
    g_ext.controller_glyph = x_controller_glyph;
    g_ext.show_onscreen_keyboard = x_show_onscreen_keyboard;
    g_ext.shader_cache_dir = x_shader_cache_dir;
    g_ext.find_steam_app = x_find_steam_app;
    g_ext.describe_power_tuning = x_describe_power_tuning;
    return &g_ext;
}

extern "C" int32_t nue_linux_apply_early_policy(uint32_t flags) {
    ProcessEnv env;
    StdFs fs;
    SessionInfo ses = detect_session(env, fs);
    XdgDirs xdg = resolve_xdg(env, fs, env.get_or("HOME"));
    int32_t count = 0;
    auto set = [&](const std::string& k, const std::string& v) {
        if (env.is_set(k)) return;  // the user's own setting always wins
#if defined(NUE_LINUX_RUNTIME)
        if (::setenv(k.c_str(), v.c_str(), 0) == 0) ++count;
#else
        (void)v;
        ++count;
#endif
    };
    AudioOptions ao;
    ao.null_device = (flags & (NUE_LINUX_POLICY_HEADLESS | NUE_LINUX_POLICY_NO_AUDIO)) != 0;
    ao.low_latency = (flags & NUE_LINUX_POLICY_HIGH_LATENCY_AUDIO) == 0;
    HintSet ah = plan_audio(detect_audio(env, fs, xdg), ao, ses);
    VideoOptions vo;
    vo.headless = (flags & NUE_LINUX_POLICY_HEADLESS) != 0;
    VideoPlan vp = plan_video(vo, ses);
    HintSet ih = plan_input(read_input_devices(fs), ses);
    for (const HintSet* h : {&ah, &vp.hints, &ih}) {
        for (const auto& kv : h->hints) set(kv.first, kv.second);  // SDL3 reads hint values from the environment too
        for (const auto& kv : h->env) set(kv.first, kv.second);
    }
    return count;
}

// ------------------------------------------------------------------------------------------ the table

extern "C" const nue_platform_ops* nue_platform_linux_get_ops(void) {
    static bool built = false;
    if (!built) {
        std::memset(&g_ops, 0, sizeof g_ops);
        g_ops.struct_size = sizeof g_ops;
        g_ops.name = "linux";
        g_ops.init = p_init;
        g_ops.shutdown = p_shutdown;
        g_ops.pump = p_pump;
        g_ops.system_info = p_system_info;
        g_ops.memory_info = p_memory_info;
        g_ops.system_locale = p_system_locale;
        g_ops.pre_init = p_pre_init;
        g_ops.memory_info_ex = p_memory_info_ex;
        g_ops.gpu_probe = p_gpu_probe;
        g_ops.screen_keyboard = p_screen_keyboard;
        g_ops.storage_root = p_storage_root;
        g_ops.power_info = p_power_info;
        g_ops.timer_resolution_get = p_timer_resolution_get;
#if defined(NUE_LINUX_RUNTIME)
        g_ops.power_apply = p_power_apply;
        g_ops.thread_set_priority = p_thread_set_priority;
        g_ops.thread_get_priority = p_thread_get_priority;
        g_ops.thread_set_affinity = p_thread_set_affinity;
        g_ops.thread_set_core_class = p_thread_set_core_class;
        g_ops.thread_set_name = p_thread_set_name;
        g_ops.sleep_until = p_sleep_until;
        g_ops.window_set_gpu_preference = p_window_set_gpu_preference;
        g_ops.crash_install = p_crash_install;
        g_ops.crash_uninstall = p_crash_uninstall;
        g_ops.crash_write_dump = p_crash_write_dump;
        g_ops.log_write = p_log_write;
        // No crash_release_input: X11 pointer grabs and Wayland pointer constraints belong to the client connection and the
        // compositor drops them when the process dies; there is no OS-wide input state for this layer to undo.
        // No haptics / perf hints / safe area: SDL3 covers gamepad rumble; the others do not exist on desktop Linux.
#endif
        built = true;
    }
    return &g_ops;
}

extern "C" void nue_linux_test_poll_now(void) {
    if (!g) return;
    nue_power_info p;
    read_power_now(*g, p);
    std::lock_guard<std::mutex> l(g->mtx);
    g->snap.power = p;
    g->snap.valid = true;
}

extern "C" void nue_linux_test_set_environment(const void* fs, const void* env) {
    g_test_fs = static_cast<const IFileSystem*>(fs);
    g_test_env = static_cast<const IEnv*>(env);
}
