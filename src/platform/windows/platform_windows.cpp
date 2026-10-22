// NUE Windows platform layer: the nue_platform_ops table over the nue::win modules (see nue_win/*.hpp and docs/PLATFORM_WINDOWS.md).
//
// What this table does NOT provide, on purpose: windows, input devices and audio devices (the SDL3 backend does), GPU preference at run time
// (Windows reads it when the process starts: use nue_win_request_discrete_gpu() in CMake or the persistent per-app setting), trigger rumble, perf hints.
// Everything the table changes outside the process is undone by shutdown() or, after a crash, by crash_release_input().
#include <windows.h>
#include <d3d11.h>
#include <dxgiformat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "internal/platform_ops.h"
#include "nue_win/cpu.hpp"
#include "nue_win/diag.hpp"
#include "nue_win/display.hpp"
#include "nue_win/gamemode.hpp"
#include "nue_win/gpu.hpp"
#include "nue_win/power.hpp"
#include "nue_win/storage.hpp"
#include "nue_win/window.hpp"

// Import libraries of every module, recorded in the objects so that whoever links the static library gets them (the root CMake file only links a few).
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "pdh.lib")

namespace {

using namespace nue::win;

// ---------------------------------------------------------------------------------------------------------------- state

struct PlatformState {
    std::mutex mutex;                       // guards the power request bookkeeping (power_apply may be called from any thread)
    bool initialised = false;
    nue_init_desc desc_copy{};
    std::string app_id = "app", org = "NUE";
    bool headless = false, deterministic = false;

    // power request bookkeeping
    WakeLock wake;
    TimerResolutionScope timer;
    bool throttling_changed = false;
    bool priority_changed = false;
    PriorityClass original_priority = PriorityClass::Normal;
    bool cpu_sets_changed = false;

    // notifications
    PowerNotifier notifier;
    std::atomic<int> power_mode{int(EffectivePowerMode::Unknown)};

    // thermal worker
    std::thread thermal_thread;
    std::atomic<bool> thermal_stop{false};
    std::atomic<int> thermal{int(NUE_THERMAL_UNKNOWN)};
    HANDLE thermal_wake = nullptr;

    // memory pressure and suspend
    HANDLE low_memory_notification = nullptr;
    std::atomic<int> pressure{int(NUE_PRESSURE_NONE)};
    std::atomic<bool> suspended{false};

    // pump bookkeeping (event thread only)
    uint64_t last_poll_ms = 0;
    nue_event_power last_pushed{};
    bool have_last = false;
};
PlatformState& S() { static PlatformState s; return s; }

CpuTopology& topology() {
    static CpuTopology t = [] { CpuTopology x; query_cpu_topology(x); return x; }();
    return t;
}

nue_result copy_text(const std::string& s, char* buffer, size_t capacity, size_t* out_size) {
    if (out_size) *out_size = s.size();
    if (!buffer || capacity == 0) return NUE_OK;
    const size_t n = s.size() < capacity - 1 ? s.size() : capacity - 1;
    std::memcpy(buffer, s.data(), n);
    buffer[n] = 0;
    return n < s.size() ? NUE_OK_PARTIAL : NUE_OK;
}

template <size_t N> void set_text(char (&dst)[N], const std::string& s) {
    const size_t n = s.size() < N - 1 ? s.size() : N - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

nue_result from_status(const Status& s) {
    if (s.ok) return NUE_OK;
    switch (s.code) {
        case ERROR_ACCESS_DENIED: return NUE_ERR_ACCESS_DENIED;
        case ERROR_INVALID_PARAMETER: return NUE_ERR_INVALID_ARGUMENT;
        case ERROR_NOT_SUPPORTED: case ERROR_PROC_NOT_FOUND: return NUE_ERR_UNSUPPORTED;
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return NUE_ERR_NOT_FOUND;
        default: return NUE_ERR_BACKEND;
    }
}

// ---------------------------------------------------------------------------------------------------------------- power / thermal / game mode

nue_thermal_state thermal_from_passive_limit(double min_limit_percent) {
    if (min_limit_percent >= 100.0) return NUE_THERMAL_NOMINAL;
    if (min_limit_percent >= 80.0) return NUE_THERMAL_FAIR;
    if (min_limit_percent >= 50.0) return NUE_THERMAL_SERIOUS;
    return NUE_THERMAL_CRITICAL;
}

// Thermal state comes from the ACPI "% Passive Limit" counters: below 100 the OS is throttling the CPU for heat. Machines without
// thermal zones (many desktops, VMs) stay UNKNOWN: the capability bit is clear for them.
int sample_thermal() {
    const auto zones = read_thermal_zones();
    if (zones.empty()) return NUE_THERMAL_UNKNOWN;
    double min_limit = 100.0;
    for (const auto& z : zones) if (z.passive_limit_percent > 0 && z.passive_limit_percent < min_limit) min_limit = z.passive_limit_percent;
    return int(thermal_from_passive_limit(min_limit));
}

nue_game_mode_state game_mode_from(EffectivePowerMode m) {
    switch (m) {
        case EffectivePowerMode::GameMode: return NUE_GAME_MODE_ON;
        case EffectivePowerMode::BatterySaver: case EffectivePowerMode::BetterBattery: return NUE_GAME_MODE_BATTERY;
        case EffectivePowerMode::HighPerformance: case EffectivePowerMode::MaxPerformance: return NUE_GAME_MODE_PERFORMANCE;
        case EffectivePowerMode::Balanced: return NUE_GAME_MODE_OFF;
        default: return NUE_GAME_MODE_UNKNOWN;
    }
}

void fill_power_info(nue_power_info& out) {
    std::memset(&out, 0, sizeof out);
    out.struct_size = sizeof out;
    const PowerStatus p = query_power_status();
    if (!p.valid) out.state = NUE_POWER_UNKNOWN;
    else if (!p.has_battery) out.state = NUE_POWER_NO_BATTERY;
    else if (p.charging) out.state = NUE_POWER_CHARGING;
    else if (p.on_ac == Support::Yes) out.state = NUE_POWER_CHARGED;
    else out.state = NUE_POWER_ON_BATTERY;
    out.battery_percent = p.has_battery ? p.battery_percent : -1;
    out.seconds_left = p.battery_seconds_left;
    out.on_ac_power = p.on_ac == Support::Yes;
    out.low_power_mode = p.energy_saver;
    out.thermal = nue_thermal_state(S().thermal.load());
    out.game_mode = game_mode_from(EffectivePowerMode(S().power_mode.load()));
    out.thermal_headroom = -1.0f;
    out.display_keep_awake_active = S().wake.active();
    out.tdp_watts_current = out.tdp_watts_min = out.tdp_watts_max = -1;   // no vendor-neutral TDP API on Windows
    out.suspend_supported = NUE_TRUE;        // PowerRegisterSuspendResumeNotification
}

nue_result win_power_info(nue_power_info* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    fill_power_info(*out);
    return NUE_OK;
}

uint32_t timer_us_current() {
    TimerResolutionInfo t;
    return query_timer_resolution(t) ? uint32_t(t.current_ms * 1000.0 + 0.5) : 0;
}

nue_result win_timer_resolution_get(uint32_t* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    *out = timer_us_current();
    return *out ? NUE_OK : NUE_ERR_UNSUPPORTED;
}

thread_local MmcssScope t_games;   // registered by power_apply on the calling thread when the game hint asks for it

nue_result win_power_apply(const nue_power_request* r, nue_power_applied* out_applied) {
    if (!r) return NUE_ERR_INVALID_ARGUMENT;
    PlatformState& st = S();
    std::lock_guard<std::mutex> lock(st.mutex);
    nue_power_applied applied = 0;
    uint32_t asked = 0, honoured = 0;
    auto want = [&](bool b) { if (b) ++asked; return b; };
    auto got = [&](nue_power_applied bit) { applied |= bit; ++honoured; };

    // profile -> process priority class (never above ABOVE_NORMAL: project policy) and EcoQoS state
    if (r->profile == NUE_PERF_DEFAULT) {
        if (st.priority_changed) { process_set_priority(st.original_priority); st.priority_changed = false; }
        if (st.throttling_changed && !r->disable_power_throttling) { process_reset_power_throttling(); st.throttling_changed = false; }
    } else {
        want(true);
        if (!st.priority_changed) { st.original_priority = process_get_priority(); }
        PriorityClass pc = PriorityClass::Normal;
        if (r->profile == NUE_PERF_HIGH || r->profile == NUE_PERF_SUSTAINED) pc = PriorityClass::AboveNormal;
        else if (r->profile == NUE_PERF_POWER_SAVER) pc = PriorityClass::BelowNormal;
        if (process_set_priority(pc).ok) { st.priority_changed = true; got(NUE_POWER_APPLIED_PROFILE); }
        if (r->profile == NUE_PERF_POWER_SAVER && !r->disable_power_throttling) {
            if (process_force_power_throttling().ok) st.throttling_changed = true;
        } else if (st.throttling_changed && !r->disable_power_throttling) {
            process_reset_power_throttling();
            st.throttling_changed = false;
        }
    }

    // game hint -> MMCSS "Games" on the calling thread (Windows has no call that switches Game Mode on: the OS decides)
    if (r->game_hint == NUE_GAME_HINT_NONE || r->game_hint == NUE_GAME_HINT_PAUSED || r->game_hint == NUE_GAME_HINT_MENU) {
        t_games.end();
    } else if (want(true)) {
        if (t_games.active() || t_games.begin(L"Games").ok) got(NUE_POWER_APPLIED_GAME_HINT);
    }

    // keep awake
    if (r->keep_awake == NUE_KEEP_AWAKE_NONE) {
        st.wake.release();
    } else if (want(true)) {
        if (st.wake.acquire(L"NUE game running", (r->keep_awake & NUE_KEEP_AWAKE_DISPLAY) != 0, (r->keep_awake & NUE_KEEP_AWAKE_SYSTEM) != 0).ok) got(NUE_POWER_APPLIED_KEEP_AWAKE);
    }

    // core class for the whole process (hybrid CPUs only)
    if (r->cpu == NUE_CPU_DEFAULT) {
        if (st.cpu_sets_changed) { process_use_cpu_sets({}); st.cpu_sets_changed = false; }
    } else if (want(true)) {
        const CpuTopology& t = topology();
        if (t.hybrid) {
            const auto& ids = r->cpu == NUE_CPU_PERFORMANCE_CORES ? t.performance_set_ids : t.efficiency_set_ids;
            if (process_use_cpu_sets(ids).ok) { st.cpu_sets_changed = true; got(NUE_POWER_APPLIED_CPU); }
        }
    }

    // timer resolution
    if (r->timer_resolution_us == 0) {
        st.timer.end();
    } else if (want(true)) {
        uint32_t granted = 0;
        if (st.timer.begin_us(r->timer_resolution_us, &granted).ok) got(NUE_POWER_APPLIED_TIMER);
    }

    // EcoQoS / timer-resolution throttling opt-out
    if (r->disable_power_throttling) {
        want(true);
        if (process_disable_power_throttling(true).ok) { st.throttling_changed = true; got(NUE_POWER_APPLIED_THROTTLING); }
    }

    // Not available on Windows: gpu (read at process start), target_frame_time_us (no ADPF), prefer_low_latency_audio (SDL's WASAPI thread is not ours).
    asked += (r->gpu != NUE_GPU_DEFAULT) + (r->target_frame_time_us != 0) + (r->prefer_low_latency_audio != 0);

    if (out_applied) *out_applied = applied;
    if (asked == 0) return NUE_OK;
    return honoured == asked ? NUE_OK : (honoured == 0 ? NUE_OK_NOOP : NUE_OK_PARTIAL);
}

void release_power_state() {
    PlatformState& st = S();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.wake.release();
    st.timer.end();
    t_games.end();
    if (st.priority_changed) { process_set_priority(st.original_priority); st.priority_changed = false; }
    if (st.throttling_changed) { process_reset_power_throttling(); st.throttling_changed = false; }
    if (st.cpu_sets_changed) { process_use_cpu_sets({}); st.cpu_sets_changed = false; }
}

// ---------------------------------------------------------------------------------------------------------------- memory

// How much of the address space or of a Job Object limit may be used before the layer reports pressure. These only decide WHEN an event is raised,
// never how much memory anything may use (budgets are policies in the core). A 32-bit process (the recompiled game) runs out of address space long
// before it runs out of RAM, which is why the free fraction of the user address space is watched.
struct PressurePolicy {
    double free_fraction_moderate = 0.15, free_fraction_high = 0.08, free_fraction_critical = 0.03;
};
const PressurePolicy& pressure_policy() { static const PressurePolicy p; return p; }

uint64_t job_memory_limit() {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &info, sizeof info, nullptr)) return 0;   // not in a job
    uint64_t limit = 0;
    if (info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) limit = info.ProcessMemoryLimit;
    if (info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) limit = limit ? std::min<uint64_t>(limit, info.JobMemoryLimit) : info.JobMemoryLimit;
    return limit;
}

struct ProcessMemory { uint64_t used = 0, limit = 0, available = 0; double free_fraction = 1.0; bool limited = false; };

// limit = the smaller of the Job Object memory limit and the user address space of a 32-bit process (a 2 GiB process cannot use more than 2 GiB, whatever the RAM).
ProcessMemory process_memory(uint64_t physical_available) {
    ProcessMemory r;
    const MemorySample m = sample_memory();
    r.used = m.working_set;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    uint64_t va_total = 0, va_free = 0;
    if (GlobalMemoryStatusEx(&ms)) { va_total = ms.ullTotalVirtual; va_free = ms.ullAvailVirtual; }
    const uint64_t kUnlimitedVa = 1ull << 40;   // 64-bit processes have 128 TiB: not a limit worth reporting
    const bool va_limited = va_total && va_total < kUnlimitedVa;
    uint64_t limit = va_limited ? va_total : 0;
    const uint64_t job = job_memory_limit();
    if (job && (limit == 0 || job < limit)) limit = job;
    r.limit = limit;
    r.limited = limit != 0;
    uint64_t room = UINT64_MAX;
    if (va_limited) { room = va_free; r.free_fraction = double(va_free) / double(va_total); }
    if (job) {
        const uint64_t left = job > m.private_bytes ? job - m.private_bytes : 0;
        room = std::min(room, left);
        r.free_fraction = std::min(r.free_fraction, double(left) / double(job));
    }
    if (physical_available) room = std::min(room, physical_available);
    r.available = room == UINT64_MAX ? physical_available : room;
    return r;
}

nue_memory_pressure pressure_level(double free_fraction, bool limited, bool os_low_memory) {
    nue_memory_pressure level = NUE_PRESSURE_NONE;
    const PressurePolicy& p = pressure_policy();
    if (limited) {
        if (free_fraction < p.free_fraction_critical) level = NUE_PRESSURE_CRITICAL;
        else if (free_fraction < p.free_fraction_high) level = NUE_PRESSURE_HIGH;
        else if (free_fraction < p.free_fraction_moderate) level = NUE_PRESSURE_MODERATE;
    }
    if (os_low_memory && level < NUE_PRESSURE_HIGH) level = NUE_PRESSURE_HIGH;   // the OS says physical memory is low
    return level;
}

nue_memory_pressure compute_pressure(const ProcessMemory& pm) {
    BOOL low = FALSE;
    if (S().low_memory_notification) QueryMemoryResourceNotification(S().low_memory_notification, &low);
    return pressure_level(pm.free_fraction, pm.limited, low != FALSE);
}

nue_result win_memory_info_ex(nue_memory_info* io) {
    if (!io) return NUE_ERR_INVALID_ARGUMENT;
    const ProcessMemory pm = process_memory(io->physical_available);
    io->process_used = pm.used;
    io->process_limit = pm.limit;
    io->process_available = pm.available;
    const GpuMemorySnapshot g = query_gpu_memory();
    if (g.valid) {
        io->unified_memory = g.unified ? NUE_TRUE : NUE_FALSE;
        io->gpu_shared_total = g.shared_total;
        io->gpu_dedicated_total = g.dedicated_total;
        io->gpu_dedicated_available = (g.dedicated_total && g.budget > g.usage) ? std::min(g.dedicated_total, g.budget - g.usage) : 0;
    }
    io->pressure = nue_memory_pressure(S().pressure.load());
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------------------- GPU capabilities (D3D11 probe)

struct FamilyMap { nue_format_family family; uint32_t dxgi, dxgi_srgb; };
const FamilyMap kFamilies[] = {
    {NUE_FMT_RGBA8, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}, {NUE_FMT_BGRA8, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
    {NUE_FMT_RGB565, DXGI_FORMAT_B5G6R5_UNORM, 0}, {NUE_FMT_RGBA4444, DXGI_FORMAT_B4G4R4A4_UNORM, 0}, {NUE_FMT_RGB5A1, DXGI_FORMAT_B5G5R5A1_UNORM, 0},
    {NUE_FMT_R8, DXGI_FORMAT_R8_UNORM, 0}, {NUE_FMT_RG8, DXGI_FORMAT_R8G8_UNORM, 0}, {NUE_FMT_R16F, DXGI_FORMAT_R16_FLOAT, 0},
    {NUE_FMT_RGBA16F, DXGI_FORMAT_R16G16B16A16_FLOAT, 0}, {NUE_FMT_RGBA32F, DXGI_FORMAT_R32G32B32A32_FLOAT, 0}, {NUE_FMT_RGB10A2, DXGI_FORMAT_R10G10B10A2_UNORM, 0},
    {NUE_FMT_DEPTH16, DXGI_FORMAT_D16_UNORM, 0}, {NUE_FMT_DEPTH24_STENCIL8, DXGI_FORMAT_D24_UNORM_S8_UINT, 0}, {NUE_FMT_DEPTH32F, DXGI_FORMAT_D32_FLOAT, 0},
    {NUE_FMT_DEPTH32F_STENCIL8, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 0},
    {NUE_FMT_BC1, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB}, {NUE_FMT_BC2, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB},
    {NUE_FMT_BC3, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB}, {NUE_FMT_BC4, DXGI_FORMAT_BC4_UNORM, 0}, {NUE_FMT_BC5, DXGI_FORMAT_BC5_UNORM, 0},
    {NUE_FMT_BC6H, DXGI_FORMAT_BC6H_UF16, 0}, {NUE_FMT_BC7, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB},
};   // ETC/EAC/ASTC/PVRTC/ATC have no Direct3D 11 format: those families stay 0 (the texture module transcodes them)

nue_format_support support_from(UINT bits, bool srgb_exists) {
    nue_format_support s = 0;
    if (bits & (D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_SHADER_LOAD)) s |= NUE_FMTSUP_SAMPLE;
    if (bits & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) s |= NUE_FMTSUP_FILTER;
    if (bits & D3D11_FORMAT_SUPPORT_MIP) s |= NUE_FMTSUP_MIPMAPS;
    if (bits & (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL)) s |= NUE_FMTSUP_RENDER_TARGET;
    if (bits & D3D11_FORMAT_SUPPORT_BLENDABLE) s |= NUE_FMTSUP_BLEND;
    if (srgb_exists) s |= NUE_FMTSUP_SRGB;
    if (bits & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) s |= NUE_FMTSUP_STORAGE;
    if (bits & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) s |= NUE_FMTSUP_MSAA;
    return s;
}

nue_result win_gpu_probe(nue_gpu_caps* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    std::vector<uint32_t> wanted;
    for (const auto& f : kFamilies) { wanted.push_back(f.dxgi); if (f.dxgi_srgb) wanted.push_back(f.dxgi_srgb); }
    D3D11Probe p;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    if (!probe_d3d11(wanted, p).ok) return NUE_ERR_UNSUPPORTED;   // the core falls back to its conservative table (probed = FALSE)
    auto bits_of = [&](uint32_t dxgi) -> UINT { for (const auto& e : p.formats) if (e.first == dxgi) return e.second; return 0; };
    for (const auto& f : kFamilies) {
        const bool srgb = f.dxgi_srgb && (bits_of(f.dxgi_srgb) & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
        out->format[f.family] = support_from(bits_of(f.dxgi), srgb);
    }
    out->probed = NUE_TRUE;
    out->renderer = NUE_RENDERER_D3D11;
    const uint32_t fl = p.feature_level;
    const bool fl11 = fl >= 0xB000, fl10 = fl >= 0xA000, fl93 = fl >= 0x9300;
    // Limits promised by the feature level (D3D11 spec "Hardware Feature Levels"); the driver may offer more, these are guaranteed.
    out->max_texture_2d = fl11 ? 16384 : (fl10 ? 8192 : (fl93 ? 4096 : 2048));
    out->max_texture_cube = out->max_texture_2d;
    out->max_texture_3d = fl10 ? 2048 : 256;
    out->max_array_layers = fl10 ? 2048 : 1;
    out->max_anisotropy = 16;
    out->max_render_targets = fl10 ? 8 : 4;
    out->max_msaa_samples = p.max_msaa_samples;
    out->unified_memory = p.unified_memory ? NUE_TRUE : NUE_FALSE;
    out->npot_mipmaps = fl10 ? NUE_TRUE : NUE_FALSE;
    out->texture_compare = fl10 ? NUE_TRUE : NUE_FALSE;
    out->compute = fl11 ? NUE_TRUE : NUE_FALSE;
    out->instancing = fl93 ? NUE_TRUE : NUE_FALSE;
    out->tessellation = fl11 ? NUE_TRUE : NUE_FALSE;
    out->dedicated_memory_bytes = p.unified_memory ? 0 : p.dedicated_memory;
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------------------- init / pump / shutdown

void thermal_worker() {
    PlatformState& st = S();
    while (!st.thermal_stop.load()) {
        st.thermal = sample_thermal();
        WaitForSingleObject(st.thermal_wake, 5000);
    }
}

void push_simple_event(uint32_t type) {
    nue_event e;
    std::memset(&e, 0, sizeof e);
    e.type = type;
    nue_internal_push_event(&e);
}

nue_result win_init(const nue_backend_init* init) {
    PlatformState& st = S();
    if (st.initialised) return NUE_OK;
    if (init && init->desc) {
        if (init->desc->app_id && init->desc->app_id[0]) st.app_id = init->desc->app_id;
        if (init->desc->org_name && init->desc->org_name[0]) st.org = init->desc->org_name;
        st.headless = init->headless != 0;
        st.deterministic = init->deterministic != 0;
    }
    if (!st.deterministic) {
        st.power_mode = int(query_effective_power_mode());
        st.low_memory_notification = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        st.notifier.start([](const PowerEvent& e) {
            if (e.kind == PowerEvent::PowerMode) { S().power_mode = e.value; return; }
            // Suspend / resume reach the core as events (any thread may push). Windows sends RESUMEAUTOMATIC and then RESUMESUSPEND when the user is there: one event.
            if (e.kind == PowerEvent::Suspend) { if (!S().suspended.exchange(true)) push_simple_event(NUE_EVENT_SYSTEM_SUSPENDING); }
            else if (e.kind == PowerEvent::ResumeAutomatic || e.kind == PowerEvent::ResumeSuspend) { if (S().suspended.exchange(false)) push_simple_event(NUE_EVENT_SYSTEM_RESUMED); }
        });
        st.thermal_stop = false;
        st.thermal_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        st.thermal = sample_thermal();
        st.thermal_thread = std::thread(thermal_worker);
    }
    st.initialised = true;
    return NUE_OK;
}

void win_shutdown() {
    PlatformState& st = S();
    if (!st.initialised) return;
    st.notifier.stop();
    if (st.thermal_thread.joinable()) {
        st.thermal_stop = true;
        SetEvent(st.thermal_wake);
        st.thermal_thread.join();
    }
    if (st.thermal_wake) { CloseHandle(st.thermal_wake); st.thermal_wake = nullptr; }
    if (st.low_memory_notification) { CloseHandle(st.low_memory_notification); st.low_memory_notification = nullptr; }
    st.suspended = false;
    release_power_state();
    st.have_last = false;
    st.initialised = false;
}

void push_power_event(uint32_t type, const nue_event_power& p) {
    nue_event e;
    std::memset(&e, 0, sizeof e);
    e.type = type;
    e.payload.power = p;
    nue_internal_push_event(&e);
}

void win_pump() {
    PlatformState& st = S();
    if (!st.initialised || st.deterministic) return;
    const uint64_t now = GetTickCount64();
    if (st.have_last && now - st.last_poll_ms < 1000) return;   // GetSystemPowerStatus is cheap, but once a second is plenty for batteries
    st.last_poll_ms = now;
    nue_power_info info;
    fill_power_info(info);
    nue_event_power cur{};
    cur.thermal = info.thermal;
    cur.power_state = info.state;
    cur.percent = info.battery_percent;
    cur.low_power = info.low_power_mode ? 1 : 0;
    cur.game_mode = uint8_t(info.game_mode);
    // memory pressure has its own level and payload
    {
        const ProcessMemory pm = process_memory(0);
        const int level = int(compute_pressure(pm));
        if (st.have_last && level != st.pressure.load()) {
            nue_event e;
            std::memset(&e, 0, sizeof e);
            e.type = NUE_EVENT_MEMORY_PRESSURE;
            e.payload.memory.level = uint32_t(level);
            e.payload.memory.available_bytes = pm.available;
            nue_internal_push_event(&e);
        }
        st.pressure = level;
    }
    if (!st.have_last) { st.last_pushed = cur; st.have_last = true; return; }   // first sample is the baseline, not a change
    const nue_event_power& last = st.last_pushed;
    if (cur.power_state != last.power_state || cur.percent != last.percent) push_power_event(NUE_EVENT_POWER_CHANGED, cur);
    if (cur.thermal != last.thermal) push_power_event(NUE_EVENT_THERMAL_CHANGED, cur);
    if (cur.low_power != last.low_power) push_power_event(NUE_EVENT_LOW_POWER_MODE_CHANGED, cur);
    if (cur.game_mode != last.game_mode) push_power_event(NUE_EVENT_GAME_MODE_CHANGED, cur);
    st.last_pushed = cur;
}

// ---------------------------------------------------------------------------------------------------------------- system info

std::string registry_string(const wchar_t* key, const wchar_t* value) {
    std::wstring w;
    return reg_read_string(HKEY_LOCAL_MACHINE, key, value, w) ? to_utf8(w) : std::string();
}

// Handheld PCs are recognised by the product / board name the firmware reports. This list is a best effort from published model identifiers and has not
// been checked on the devices themselves (none was available): a miss only means the device is reported as a laptop.
void classify_handheld(const std::string& product, const std::string& board, nue_system_info& out) {
    auto has = [&](const char* needle) {
        std::string b = needle;
        for (auto& c : b) c = char(std::tolower((unsigned char)c));
        for (const std::string* s : {&product, &board}) {
            std::string a = *s;
            for (auto& c : a) c = char(std::tolower((unsigned char)c));
            if (a.find(b) != std::string::npos) return true;
        }
        return false;
    };
    const char* variant = nullptr;
    if (has("jupiter") || has("galileo")) { out.platform_flags |= NUE_PLATFORM_STEAM_DECK; variant = has("galileo") ? "Steam Deck OLED" : "Steam Deck"; }
    else if (has("rc71l") || has("rc72la") || has("rog ally")) variant = has("rc72la") ? "ROG Ally X" : "ROG Ally";
    else if (has("legion go") || has("83e1") || has("83l3") || has("83n6")) variant = "Legion Go";
    else if (has("msi claw") || has("claw a1m") || has("claw 8")) variant = "MSI Claw";
    else if (has("ayaneo") || has("onexplayer") || has("onexfly") || has("gpd win")) variant = "Handheld PC";
    if (variant) {
        out.platform_flags |= NUE_PLATFORM_HANDHELD;
        out.device_class = NUE_DEVICE_HANDHELD;
        set_text(out.platform_variant, variant);
    }
}

nue_result win_system_info(nue_system_info* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    out->os = NUE_OS_WINDOWS;
    const OsVersion v = os_version();
    set_text(out->os_name, v.major >= 10 && v.build >= 22000 ? "Windows 11" : (v.major >= 10 ? "Windows 10" : "Windows"));
    char ver[64];
    std::snprintf(ver, sizeof ver, "%u.%u.%u", v.major, v.minor, v.build);
    set_text(out->os_version, ver);
    out->os_build = v.build;
#if defined(_M_ARM64) || defined(__aarch64__)
    out->arch = NUE_ARCH_KIND_ARM64;
#elif defined(_WIN64)
    out->arch = NUE_ARCH_KIND_X64;
#else
    out->arch = NUE_ARCH_KIND_X86;
#endif
    // Translation: x86/x64 code on Windows-on-ARM (Prism / emulation), or Wine/Proton exposing wine_get_version from ntdll.
    out->running_under_translation = NUE_FALSE;
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    if (auto fn = load_proc<IsWow64Process2Fn>(L"kernel32.dll", "IsWow64Process2")) {
        USHORT process = 0, native = 0;
        if (fn(GetCurrentProcess(), &process, &native) && native == IMAGE_FILE_MACHINE_ARM64 && out->arch != NUE_ARCH_KIND_ARM64) out->running_under_translation = NUE_TRUE;
    }
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) out->running_under_translation = NUE_TRUE;

    const PowerStatus p = query_power_status();
    out->device_class = p.has_battery ? NUE_DEVICE_LAPTOP : NUE_DEVICE_DESKTOP;
    set_text(out->device_model, registry_string(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName"));

    const CpuTopology& t = topology();
    set_text(out->cpu_name, t.brand);
    out->cpu_logical_cores = t.logical_processors;
    out->cpu_physical_cores = t.physical_cores;
    out->cpu_performance_cores = out->cpu_efficiency_cores = 0;
    if (t.hybrid) {
        std::vector<std::pair<uint16_t, uint8_t>> perf, eff;
        for (const auto& s : t.sets) (s.efficiency_class == t.max_efficiency_class ? perf : eff).push_back({s.group, s.core_index});
        auto uniq = [](std::vector<std::pair<uint16_t, uint8_t>>& x) { std::sort(x.begin(), x.end()); return uint32_t(std::unique(x.begin(), x.end()) - x.begin()); };
        out->cpu_performance_cores = uniq(perf);
        out->cpu_efficiency_cores = uniq(eff);
    }
    out->cpu_max_mhz = cpu_max_mhz();
    const CpuFeatures f = query_cpu_features();
    out->cpu_features = (f.sse2 ? NUE_CPU_SSE2 : 0) | (f.sse41 ? NUE_CPU_SSE41 : 0) | (f.sse42 ? NUE_CPU_SSE42 : 0) | (f.avx ? NUE_CPU_AVX : 0) | (f.avx2 ? NUE_CPU_AVX2 : 0) |
                        (f.avx512f ? NUE_CPU_AVX512 : 0) | (f.fma ? NUE_CPU_FMA : 0) | (f.popcnt ? NUE_CPU_POPCNT : 0);
    uint64_t total = 0, avail = 0;
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) { total = m.ullTotalPhys; avail = m.ullAvailPhys; }
    out->memory_total_bytes = total;
    out->memory_available_bytes = avail;

    out->gpu_count = 0;
    GpuEnumeration g;
    if (enumerate_gpus(g, false, false).ok) {
        for (const auto& a : g.adapters) {
            if (a.software || out->gpu_count >= NUE_MAX_GPUS) continue;
            nue_gpu_info& gi = out->gpus[out->gpu_count++];
            std::memset(&gi, 0, sizeof gi);
            set_text(gi.name, a.name);
            gi.vendor_id = a.vendor_id;
            gi.device_id = a.device_id;
            gi.dedicated_memory_bytes = a.dedicated_video_memory;
            gi.shared_memory_bytes = a.shared_system_memory;
            gi.is_integrated = a.integrated ? NUE_TRUE : NUE_FALSE;
            gi.is_default = int(a.index) == g.high_performance_index ? NUE_TRUE : NUE_FALSE;
        }
    }
    out->display_count = uint32_t(GetSystemMetrics(SM_CMONITORS));
    out->platform_flags = 0;
    out->platform_variant[0] = out->runtime_name[0] = 0;
    if (out->running_under_translation) {
        out->platform_flags |= NUE_PLATFORM_COMPAT_LAYER;
        using WineVersion = const char* (*)(void);
        if (auto wv = reinterpret_cast<WineVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))) set_text(out->runtime_name, std::string("Wine ") + wv());
    }
    classify_handheld(registry_string(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName"), registry_string(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardProduct"), *out);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) set_text(out->locale, to_utf8(locale));
    DWORD light = 1;
    out->dark_mode = reg_read_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", light) && light == 0 ? NUE_TRUE : NUE_FALSE;
    return NUE_OK;
}

nue_result win_memory_info(uint64_t* total, uint64_t* available) {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return NUE_ERR_BACKEND;
    if (total) *total = m.ullTotalPhys;
    if (available) *available = m.ullAvailPhys;
    return NUE_OK;
}

nue_result win_system_locale(char* buffer, size_t capacity) {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (!GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) return NUE_ERR_BACKEND;
    return copy_text(to_utf8(locale), buffer, capacity, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------- storage

nue_result win_storage_root(nue_root_kind kind, const nue_init_desc* desc, char* buffer, size_t capacity, size_t* out_size) {
    const std::string org = desc && desc->org_name && desc->org_name[0] ? desc->org_name : S().org;
    const std::string app = desc && desc->app_id && desc->app_id[0] ? desc->app_id : S().app_id;
    std::wstring path;
    switch (kind) {
        case NUE_ROOT_DATA:
            if (desc && desc->game_data_path && desc->game_data_path[0]) { path = path_from_utf8(desc->game_data_path); break; }
            { AppDirs d; AppDirOptions o; o.create = false; resolve_app_dirs(org, app, o, d); path = d.install; }
            break;
        case NUE_ROOT_USER:
            if (desc && desc->user_data_path && desc->user_data_path[0]) { path = path_from_utf8(desc->user_data_path); break; }
            { AppDirs d; AppDirOptions o; o.create = false; if (!resolve_app_dirs(org, app, o, d).ok) return NUE_ERR_BACKEND; path = d.saves; }
            break;
        case NUE_ROOT_CACHE: {
            if (desc && desc->user_data_path && desc->user_data_path[0]) return NUE_ERR_UNSUPPORTED;   // explicit user folder (portable mode, tests): the core puts cache/ and settings/ below it
            AppDirs d; AppDirOptions o; o.create = false;
            if (!resolve_app_dirs(org, app, o, d).ok) return NUE_ERR_BACKEND;
            path = d.cache;
            break;
        }
        case NUE_ROOT_SETTINGS: {
            if (desc && desc->user_data_path && desc->user_data_path[0]) return NUE_ERR_UNSUPPORTED;
            AppDirs d; AppDirOptions o; o.create = false;
            if (!resolve_app_dirs(org, app, o, d).ok) return NUE_ERR_BACKEND;
            path = d.config;
            break;
        }
        case NUE_ROOT_TEMP: {
            if (desc && desc->user_data_path && desc->user_data_path[0]) return NUE_ERR_UNSUPPORTED;   // portable mode: temp lives below the user folder too
            std::wstring t;
            if (!known_folder_path(KnownFolder::Temp, t).ok) return NUE_ERR_BACKEND;
            path = t + L"\\" + path_from_utf8(sanitize_file_name(org)) + L"\\" + path_from_utf8(sanitize_file_name(app));
            break;
        }
        default: return NUE_ERR_UNSUPPORTED;
    }
    if (path.empty()) return NUE_ERR_NOT_FOUND;
    return copy_text(path_to_utf8(path), buffer, capacity, out_size);
}

// ---------------------------------------------------------------------------------------------------------------- threads

struct ThreadTarget {
    HANDLE handle = nullptr;
    bool owned = false;
    bool current = false;
    ~ThreadTarget() { if (owned && handle) CloseHandle(handle); }
};

bool resolve_thread(const nue_native_thread_ref* t, DWORD access, ThreadTarget& out) {
    if (!t || (t->handle == 0 && t->id == 0)) { out.handle = GetCurrentThread(); out.current = true; return true; }
    if (t->handle != 0) { out.handle = reinterpret_cast<HANDLE>(uintptr_t(t->handle)); return true; }
    out.handle = OpenThread(access, FALSE, DWORD(t->id));
    out.owned = out.handle != nullptr;
    out.current = DWORD(t->id) == GetCurrentThreadId();
    return out.handle != nullptr;
}

thread_local MmcssScope t_audio_class;   // REALTIME on the calling thread = MMCSS "Games" (see win_thread_set_priority)

int win32_priority(nue_thread_priority p) {
    switch (p) {
        case NUE_THREAD_PRIORITY_LOW: return THREAD_PRIORITY_BELOW_NORMAL;
        case NUE_THREAD_PRIORITY_ABOVE_NORMAL: case NUE_THREAD_PRIORITY_HIGH: case NUE_THREAD_PRIORITY_REALTIME: return THREAD_PRIORITY_ABOVE_NORMAL;   // capped by policy
        default: return THREAD_PRIORITY_NORMAL;
    }
}

nue_result win_thread_set_priority(const nue_native_thread_ref* ref, nue_thread_priority p) {
    ThreadTarget t;
    if (!resolve_thread(ref, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, t)) return NUE_ERR_NOT_FOUND;
    // MMCSS owns the priority of a registered thread, so leave it first, set the plain priority, then register again if wanted.
    // REALTIME = MMCSS "Games" on the calling thread. Measured on Windows 11: Games puts the thread at base priority 9 (= ABOVE_NORMAL of the normal class),
    // "Playback" 10, "Audio" 13, "Pro Audio" 15 (= TIME_CRITICAL, which the project forbids), so only "Games" respects the cap.
    if (t.current) t_audio_class.end();
    if (!thread_set_priority(t.handle, win32_priority(p)).ok) return NUE_ERR_ACCESS_DENIED;
    if (t.current && p == NUE_THREAD_PRIORITY_REALTIME) t_audio_class.begin(L"Games");
    return NUE_OK;
}

nue_result win_thread_get_priority(const nue_native_thread_ref* ref, nue_thread_priority* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    ThreadTarget t;
    if (!resolve_thread(ref, THREAD_QUERY_LIMITED_INFORMATION, t)) return NUE_ERR_NOT_FOUND;
    const int p = GetThreadPriority(t.handle);
    if (p == THREAD_PRIORITY_ERROR_RETURN) return NUE_ERR_BACKEND;
    *out = p < 0 ? NUE_THREAD_PRIORITY_LOW : (p == 0 ? NUE_THREAD_PRIORITY_NORMAL : (p == 1 ? NUE_THREAD_PRIORITY_ABOVE_NORMAL : NUE_THREAD_PRIORITY_HIGH));
    return NUE_OK;
}

nue_result win_thread_set_affinity(const nue_native_thread_ref* ref, uint64_t mask) {
    ThreadTarget t;
    if (!resolve_thread(ref, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, t)) return NUE_ERR_NOT_FOUND;
    if (mask == 0) {
        DWORD_PTR process_mask = 0, system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return NUE_ERR_BACKEND;
        mask = uint64_t(process_mask);
    }
#ifndef _WIN64
    if (mask >> 32) return NUE_ERR_INVALID_ARGUMENT;   // 32-bit process: one processor group of at most 32 CPUs
#endif
    return thread_set_affinity_mask(t.handle, mask).ok ? NUE_OK : NUE_ERR_INVALID_ARGUMENT;
}

nue_result win_thread_set_core_class(const nue_native_thread_ref* ref, nue_cpu_preference pref) {
    const CpuTopology& topo = topology();
    if (!topo.hybrid) return NUE_OK_NOOP;
    ThreadTarget t;
    if (!resolve_thread(ref, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, t)) return NUE_ERR_NOT_FOUND;
    const CoreClass c = pref == NUE_CPU_PERFORMANCE_CORES ? CoreClass::Performance : (pref == NUE_CPU_EFFICIENCY_CORES ? CoreClass::Efficiency : CoreClass::Any);
    return thread_use_core_class(t.handle, topo, c).ok ? NUE_OK : NUE_ERR_BACKEND;
}

nue_result win_thread_set_name(const char* name) {
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    auto fn = load_proc<Fn>(L"kernel32.dll", "SetThreadDescription");
    if (!fn) return NUE_ERR_UNSUPPORTED;
    return SUCCEEDED(fn(GetCurrentThread(), to_wide(name ? name : "").c_str())) ? NUE_OK : NUE_ERR_BACKEND;
}

nue_result win_sleep_until(int64_t deadline_ns) {
    thread_local PreciseSleeper sleeper;
    const int64_t remaining = deadline_ns - nue_time_ns();
    if (remaining > 0) sleeper.sleep_ns(remaining);
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------------------- diagnostics

struct CrashBridge {
    const nue_crash_sink* sink = nullptr;
    nue_crash_sink sink_copy{};
    nue_crash_desc desc_copy{};
    bool installed = false;
} g_bridge;

const char* bridge_begin(const nue::win::CrashInfo& info, void*) {
    nue_crash_info ci;
    std::memset(&ci, 0, sizeof ci);
    ci.struct_size = sizeof ci;
    ci.reason = info.reason;
    ci.fault_address = info.fault_address;
    ci.fault_pc = info.fault_pc;
    ci.signal_or_code = info.code;
    return g_bridge.sink_copy.begin ? g_bridge.sink_copy.begin(&ci, g_bridge.sink_copy.userdata) : nullptr;
}

bool bridge_end(const nue::win::CrashInfo& info, void*) {
    nue_crash_info ci;
    std::memset(&ci, 0, sizeof ci);
    ci.struct_size = sizeof ci;
    ci.reason = info.reason;
    ci.fault_address = info.fault_address;
    ci.fault_pc = info.fault_pc;
    ci.signal_or_code = info.code;
    ci.report_dir = info.report_dir;
    return g_bridge.sink_copy.end ? g_bridge.sink_copy.end(&ci, g_bridge.sink_copy.userdata) != 0 : true;
}

nue_result win_crash_install(const nue_crash_desc* desc, const nue_crash_sink* sink) {
    if (!desc || !sink) return NUE_ERR_INVALID_ARGUMENT;
    g_bridge.sink_copy = *sink;
    g_bridge.desc_copy = *desc;
    CrashHook h;
    h.begin = bridge_begin;
    h.end = bridge_end;
    h.full_memory = (desc->flags & NUE_CRASH_FULL_DUMP) != 0;
    h.write_dump = (desc->flags & (NUE_CRASH_MINIDUMP | NUE_CRASH_FULL_DUMP)) != 0;
    h.release_input = (desc->flags & NUE_CRASH_RELEASE_CURSOR) != 0;
    if (!install_crash_hook(h).ok) return NUE_ERR_BACKEND;
    g_bridge.installed = true;
    return NUE_OK;
}

void win_crash_uninstall() {
    if (g_bridge.installed) { uninstall_crash_handler(); g_bridge.installed = false; }
}

nue_result win_crash_write_dump(const char* directory, nue_crash_flags flags) {
    if (!directory) return NUE_ERR_INVALID_ARGUMENT;
    const std::wstring path = path_from_utf8(directory) + L"\\crash.dmp";
    return from_status(write_minidump(nullptr, path, (flags & NUE_CRASH_FULL_DUMP) != 0));
}

void win_crash_release_input() { emergency_restore_desktop(); }

void win_log_write(nue_log_level level, const char* tag, const char* message) {
    static const char* const names[] = {"?", "trace", "debug", "info", "warn", "error", "fatal"};
    char line[1100];
    const unsigned idx = unsigned(level) <= 6 ? unsigned(level) : 0;
    std::snprintf(line, sizeof line, "[%s %s] %s\n", tag ? tag : "nue", names[idx], message ? message : "");
    OutputDebugStringA(line);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- the table

extern "C" const nue_platform_ops* nue_platform_windows_get_ops(void) {
    static nue_platform_ops ops;
    static std::once_flag once;
    std::call_once(once, [] {
        std::memset(&ops, 0, sizeof ops);
        ops.struct_size = sizeof ops;
        ops.name = "windows";
        ops.init = win_init;
        ops.shutdown = win_shutdown;
        ops.pump = win_pump;
        ops.system_info = win_system_info;
        ops.memory_info = win_memory_info;
        ops.system_locale = win_system_locale;
        ops.storage_root = win_storage_root;
        ops.power_info = win_power_info;
        ops.power_apply = win_power_apply;
        ops.timer_resolution_get = win_timer_resolution_get;
        ops.thread_set_priority = win_thread_set_priority;
        ops.thread_get_priority = win_thread_get_priority;
        ops.thread_set_affinity = win_thread_set_affinity;
        ops.thread_set_core_class = win_thread_set_core_class;
        ops.thread_set_name = win_thread_set_name;
        ops.sleep_until = win_sleep_until;
        ops.crash_install = win_crash_install;
        ops.crash_uninstall = win_crash_uninstall;
        ops.crash_write_dump = win_crash_write_dump;
        ops.crash_release_input = win_crash_release_input;
        ops.log_write = win_log_write;
        ops.memory_info_ex = win_memory_info_ex;
        ops.gpu_probe = win_gpu_probe;
        // pre_init, window_request_refresh and screen_keyboard stay NULL: nothing process-wide must precede SDL_Init on Windows (DPI policy is left to the host's
        // manifest), there is no compositor frame-rate API, and SDL's text input already brings up the touch keyboard.
        // perf_hint_*, window_set_gpu_preference, haptics_*, import, window_safe_area stay NULL: Windows offers nothing real for them.

        auto set = [](nue_capability c) { ops.capabilities[unsigned(c) / 64] |= 1ull << (unsigned(c) % 64); };
        set(NUE_CAP_GAME_MODE);          // read through the effective power mode; the OS decides, MMCSS is requested for the game hint
        set(NUE_CAP_LOW_POWER_MODE);
        set(NUE_CAP_KEEP_AWAKE);
        set(NUE_CAP_TIMER_RESOLUTION);
        set(NUE_CAP_THREAD_PRIORITY);
        set(NUE_CAP_THREAD_AFFINITY);
        set(NUE_CAP_CRASH_HANDLER);
        set(NUE_CAP_MINIDUMP);
        if (query_power_status().has_battery) set(NUE_CAP_BATTERY);
        if (topology().hybrid) set(NUE_CAP_CORE_CLASS);
        if (!read_thermal_zones().empty()) set(NUE_CAP_THERMAL_STATE);   // ACPI thermal zones exist (laptops, many desktops); absent in most VMs
    });
    return &ops;
}

// ---------------------------------------------------------------------------------------------------------------- test seams (used by platform_glue_tests only)
extern "C" int nue_win_test_pressure(double free_fraction, int limited, int os_low_memory) { return int(pressure_level(free_fraction, limited != 0, os_low_memory != 0)); }
extern "C" void nue_win_test_inject_power(int kind) { S().notifier.inject_for_test({PowerEvent::Kind(kind), 0}); }
extern "C" void nue_win_test_classify_handheld(const char* product, const char* board, nue_system_info* out) { classify_handheld(product, board, *out); }
