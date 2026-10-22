// Android platform layer: implements nue_platform_ops (src/core/internal/platform_ops.h) on top of the Java glue and the
// NDK code of this folder.  SDL3 (shared backend) keeps window, input and audio; this adds what SDL3 does not give:
// lifecycle with surface hand-shake, power / thermal / Game Mode, ADPF, storage roots and the SAF import source, crash
// reports, logcat, system info, thread helpers, haptics.
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "android_state.hpp"
#include "cpu_topology.hpp"
#include "import_engine.hpp"
#include "internal/platform_ops.h"
#include "nue_ext_android.h"

using namespace nue::android;
using namespace nue::android::native;
namespace fs = std::filesystem;

extern "C" void nue_android_jni_anchor(void);

namespace {

// ---------------------------------------------------------------------------------------------------- helpers

nue_result put_string(const std::string& s, char* buffer, size_t capacity, size_t* out_size) {
    if (out_size) *out_size = s.size() + 1;
    if (buffer && capacity) {
        size_t n = s.size() < capacity - 1 ? s.size() : capacity - 1;
        std::memcpy(buffer, s.data(), n);
        buffer[n] = '\0';
    }
    return NUE_OK;
}

template <size_t N>
void set_cstr(char (&dst)[N], const char* src) {
    std::snprintf(dst, N, "%s", src ? src : "");
}

nue_platform_ops g_ops;
nue_power_request g_request;          // what the game asked for last
float g_requested_fps = 0;

// ---------------------------------------------------------------------------------------------------- capabilities

uint64_t cap_bit(nue_capability c) { return 1ull << (static_cast<unsigned>(c) & 63u); }

void fill_capabilities(nue_platform_ops* ops) {
    State& st = state();
    uint64_t w0 = 0, w1 = 0;
    auto add = [&](nue_capability c) {
        if (static_cast<unsigned>(c) < 64u) w0 |= cap_bit(c);
        else w1 |= cap_bit(c);
    };
    int sdk;
    bool vulkan, gles, lowlat;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        sdk = st.device.sdk_int;
        vulkan = st.gpu.probed && st.gpu.vulkan_present;
        gles = st.gpu.probed && st.gpu.gles_major >= 3;
        lowlat = st.audio_props.low_latency || st.audio_props.pro;
    }
    add(NUE_CAP_DATA_IMPORT);
    add(NUE_CAP_DATA_IMPORT_PICKER);
    add(NUE_CAP_THERMAL_STATE);
    add(NUE_CAP_BATTERY);
    add(NUE_CAP_LOW_POWER_MODE);
    add(NUE_CAP_KEEP_AWAKE);
    add(NUE_CAP_THREAD_PRIORITY);
    add(NUE_CAP_THREAD_AFFINITY);
    add(NUE_CAP_CRASH_HANDLER);
    add(NUE_CAP_LIFECYCLE_PAUSE);
    add(NUE_CAP_MMAP_FILES);
    if (sdk >= 31) add(NUE_CAP_GAME_MODE);
    if (nue_android_has_vibrator()) add(NUE_CAP_HAPTICS);
    if (lowlat) add(NUE_CAP_AUDIO_LOW_LATENCY);
    if (adpf_available()) add(NUE_CAP_PERF_HINT);
    if (vulkan) add(NUE_CAP_GFX_VULKAN);
    if (gles) add(NUE_CAP_GFX_GLES);
    ops->capabilities[0] = w0;
    ops->capabilities[1] = w1;
}

// ---------------------------------------------------------------------------------------------------- init / shutdown / pump

nue_power_state power_state_now(int percent, bool charging) {
    if (percent < 0) return NUE_POWER_UNKNOWN;
    if (charging) return percent >= 100 ? NUE_POWER_CHARGED : NUE_POWER_CHARGING;
    return NUE_POWER_ON_BATTERY;
}

nue_thermal_state thermal_from_level(int status, int level) {
    if (status < 0) return NUE_THERMAL_UNKNOWN;
    switch (level) {
    case 0: return NUE_THERMAL_NOMINAL;
    case 1: return NUE_THERMAL_FAIR;
    case 2: return NUE_THERMAL_SERIOUS;
    default: return NUE_THERMAL_CRITICAL;
    }
}

nue_game_mode_state game_mode_from(int m) {
    switch (m) {
    case 1: return NUE_GAME_MODE_ON;          // STANDARD
    case 2: return NUE_GAME_MODE_PERFORMANCE;
    case 3: return NUE_GAME_MODE_BATTERY;
    case 4: return NUE_GAME_MODE_ON;          // CUSTOM
    default: return NUE_GAME_MODE_UNKNOWN;    // 0 = unsupported
    }
}

void fill_power_event(nue_event* e) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    const ThermalAdvice& a = st.thermal.advice();
    e->payload.power.thermal = thermal_from_level(st.thermal_status, static_cast<int>(a.level));
    e->payload.power.power_state = power_state_now(st.battery_percent, st.charging != 0);
    e->payload.power.percent = st.battery_percent;
    e->payload.power.low_power = st.power_save ? 1 : 0;
    e->payload.power.game_mode = game_mode_from(st.game_mode);
}

nue_window_id g_main_window = 0;

// The window the game renders into: the focused one, else the last one seen.
nue_window_id main_window_id() {
    nue_window w = nue_window_get_focused();
    if (w) g_main_window = nue_window_get_id(w);
    return g_main_window;
}

nue_memory_pressure pressure_to_nue(int p) {
    switch (p) {
    case NUE_ANDROID_MEM_MODERATE:
    case NUE_ANDROID_MEM_BACKGROUND: return NUE_PRESSURE_MODERATE; // trimmed caches are cheap to rebuild
    case NUE_ANDROID_MEM_LOW: return NUE_PRESSURE_HIGH;
    case NUE_ANDROID_MEM_CRITICAL: return NUE_PRESSURE_CRITICAL;
    default: return NUE_PRESSURE_NONE;
    }
}

void push_memory_pressure(int android_pressure) {
    nue_event e;
    std::memset(&e, 0, sizeof e);
    e.type = NUE_EVENT_MEMORY_PRESSURE;
    e.payload.memory.level = static_cast<uint32_t>(pressure_to_nue(android_pressure));
    uint64_t total = 0, avail = 0;
    {
        State& st = state();
        std::lock_guard<std::mutex> lock(st.mu);
        total = st.mem.total_ram;
        avail = st.mem.available_ram > st.mem.low_threshold ? st.mem.available_ram - st.mem.low_threshold : 0;
    }
    (void)total;
    e.payload.memory.available_bytes = avail;
    nue_internal_push_event(&e);
}

void push(uint32_t type) {
    nue_event e;
    std::memset(&e, 0, sizeof e);
    e.type = type;
    if (type == NUE_EVENT_SURFACE_CREATED || type == NUE_EVENT_SURFACE_LOST) e.window_id = main_window_id();
    if (type == NUE_EVENT_AUDIO_INTERRUPTED || type == NUE_EVENT_AUDIO_RESUMED || type == NUE_EVENT_AUDIO_ROUTE_CHANGED)
        e.payload.audio_device.device_id = 0;
    if (type == NUE_EVENT_THERMAL_CHANGED || type == NUE_EVENT_POWER_CHANGED || type == NUE_EVENT_LOW_POWER_MODE_CHANGED ||
        type == NUE_EVENT_GAME_MODE_CHANGED)
        fill_power_event(&e);
    nue_internal_push_event(&e);
}

void (*g_pick_done)(void*, nue_result, const char*) = nullptr;
void* g_pick_user = nullptr;
std::mutex g_pick_mu;

int g_last_battery = -2;
bool g_last_charging = false;

nue_result p_init(const nue_backend_init*) {
    nue_android_jni_anchor();
    nue_android_probe_gpu();
    // The probe takes a few hundred ms at most; capabilities are fixed per run, so wait for it here (bounded).
    for (int i = 0; i < 150; ++i) {
        NueAndroidGpuInfo g{};
        g.struct_size = sizeof g;
        if (nue_android_get_gpu_info(&g)) break;
        usleep(10 * 1000);
    }
    static const nue_ext_android_v1 ext = {
        sizeof(nue_ext_android_v1),
        nue_android_get_device_info,
        nue_android_get_display_info,
        nue_android_probe_gpu,
        nue_android_get_gpu_info,
        nue_android_recommend_dxt,
        nue_android_get_memory_budget,
        nue_android_set_memory_cap,
        nue_android_set_preferred_refresh,
        nue_android_set_immersive,
        nue_android_rotation_available,
        nue_android_enable_rotation,
        nue_android_get_rotation,
        nue_android_get_audio_info,
        nue_android_audio_focus,
        nue_android_audio_focus_state,
        nue_android_get_storage_paths,
        nue_android_get_power_info,
        nue_android_trace_begin,
        nue_android_trace_end,
        nue_android_trace_counter,
        nue_android_dump_capabilities,
    };
    nue_internal_register_extension(NUE_EXT_ANDROID_V1, &ext);
    fill_capabilities(&g_ops);
    NLOGI("platform layer 'android' up");
    return NUE_OK;
}

nue_power_request zero_request() {
    nue_power_request r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    return r;
}

nue_result p_power_apply(const nue_power_request* request, nue_power_applied* out_applied);

void p_shutdown(void) {
    // Undo everything we changed in the OS: keep-screen-on, sustained performance, frame-rate vote, ADPF, audio focus.
    nue_power_request none = zero_request();
    p_power_apply(&none, nullptr);
    nue_android_set_frame_rate(0.0f);
    nue_android_hint_session_stop();
    nue_android_enable_rotation(0);
    nue_android_audio_focus(0);
    {
        std::lock_guard<std::mutex> lock(g_pick_mu);
        g_pick_done = nullptr;
    }
}

void p_pump(void) {
    State& st = state();
    NueAndroidEvent ev;
    while (st.events.poll(&ev)) {
        switch (ev.type) {
        case NUE_ANDROID_EVENT_LIFECYCLE:
            // onResume -> FOREGROUND; onPause -> INACTIVE (still visible in multi-window); onStop -> BACKGROUND
            switch (ev.i[0]) {
            case NUE_ANDROID_LIFE_RESUME: nue_internal_set_app_state(NUE_APP_STATE_FOREGROUND); break;
            case NUE_ANDROID_LIFE_PAUSE: nue_internal_set_app_state(NUE_APP_STATE_INACTIVE); break;
            case NUE_ANDROID_LIFE_STOP: nue_internal_set_app_state(NUE_APP_STATE_BACKGROUND); break;
            case NUE_ANDROID_LIFE_DESTROY: nue_internal_set_app_state(NUE_APP_STATE_TERMINATING); break;
            default: break;
            }
            break;
        case NUE_ANDROID_EVENT_SURFACE:
            if (ev.i[0] == NUE_ANDROID_SURFACE_CREATED) push(NUE_EVENT_SURFACE_CREATED);
            break;
        case NUE_ANDROID_EVENT_ACTION:
            if (ev.i[0] == NUE_ANDROID_ACTION_RELEASE_SURFACE) {
                // Watchers run synchronously inside push: nue_gfx destroys its swapchain there. Only then is Android
                // allowed to continue with surfaceDestroyed.
                push(NUE_EVENT_SURFACE_LOST);
                nue_android_surface_released();
            }
            break;
        case NUE_ANDROID_EVENT_MEMORY:
            if (ev.i[1] > NUE_ANDROID_MEM_NONE) push_memory_pressure(ev.i[1]); // budgets are trimmed by the core on this event
            if (ev.i[0] < 0) push(NUE_EVENT_APP_LOW_MEMORY);                    // onLowMemory()
            break;
        case NUE_ANDROID_EVENT_AUDIO_FOCUS:
            // ducking has no portable event: the state stays readable through nue.android.v1 audio_focus_state
            if (ev.i[0] == NUE_ANDROID_AUDIO_FOCUS_PAUSE) push(NUE_EVENT_AUDIO_INTERRUPTED);
            else if (ev.i[0] == NUE_ANDROID_AUDIO_FOCUS_RESUME) push(NUE_EVENT_AUDIO_RESUMED);
            break;
        case NUE_ANDROID_EVENT_AUDIO_ROUTE: push(NUE_EVENT_AUDIO_ROUTE_CHANGED); break;
        case NUE_ANDROID_EVENT_THERMAL: push(NUE_EVENT_THERMAL_CHANGED); break;
        case NUE_ANDROID_EVENT_BATTERY_SAVER: push(NUE_EVENT_LOW_POWER_MODE_CHANGED); break;
        case NUE_ANDROID_EVENT_GAME_MODE: push(NUE_EVENT_GAME_MODE_CHANGED); break;
        case NUE_ANDROID_EVENT_TREE_PICKED: {
            void (*done)(void*, nue_result, const char*);
            void* user;
            {
                std::lock_guard<std::mutex> lock(g_pick_mu);
                done = g_pick_done;
                user = g_pick_user;
                g_pick_done = nullptr;
            }
            if (done) {
                char uri[1024];
                nue_android_get_picked_uri(uri, sizeof uri);
                if (ev.i[0]) done(user, NUE_OK, uri);
                else done(user, NUE_ERR_CANCELLED, nullptr);
            }
            break;
        }
        default: break; // the SDL3 backend reports focus, display, input, audio devices itself
        }
    }
    // battery level / charging changes arrive as snapshots, not events
    int pct;
    bool chg;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        pct = st.battery_percent;
        chg = st.charging != 0;
    }
    if (pct != g_last_battery || chg != g_last_charging) {
        bool first = g_last_battery == -2;
        g_last_battery = pct;
        g_last_charging = chg;
        if (!first) push(NUE_EVENT_POWER_CHANGED);
    }
}

// ---------------------------------------------------------------------------------------------------- system

nue_result p_system_info(nue_system_info* out) {
    State& st = state();
    NueAndroidDeviceInfo d;
    NueAndroidDisplayInfo disp;
    NueAndroidGpuInfo g;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        d = st.device;
        disp = st.display;
        g = st.gpu;
    }
    out->os = NUE_OS_ANDROID;
    std::snprintf(out->os_name, sizeof out->os_name, "Android (API %d)", d.sdk_int);
    std::snprintf(out->os_version, sizeof out->os_version, "%d", d.sdk_int);
    out->os_build = static_cast<uint32_t>(d.sdk_int);
    out->arch = d.is_64bit ? NUE_ARCH_KIND_ARM64 : NUE_ARCH_KIND_ARM32;
    if (std::strstr(d.abi, "x86")) out->arch = std::strstr(d.abi, "64") ? NUE_ARCH_KIND_X64 : NUE_ARCH_KIND_X86;
    out->running_under_translation = d.is_emulator ? NUE_TRUE : NUE_FALSE;
    out->device_class = NUE_DEVICE_PHONE;
    if (d.is_tv) {
        out->device_class = NUE_DEVICE_TV;
    } else if (d.is_chromeos) {
        out->device_class = NUE_DEVICE_LAPTOP;
    } else if (disp.width > 0 && disp.dpi > 0) {
        float smallest_dp = float(std::min(disp.width, disp.height)) / (disp.dpi / 160.0f);
        if (smallest_dp >= 600.0f) out->device_class = NUE_DEVICE_TABLET;
    }
    set_cstr(out->device_model, d.model);
    set_cstr(out->cpu_name, d.soc[0] ? d.soc : d.model);
    out->cpu_logical_cores = out->cpu_physical_cores = static_cast<uint32_t>(d.cpu_cores);
    std::vector<uint32_t> khz;
    for (int i = 0; i < d.cpu_cores && i < 64; ++i) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        uint32_t v = 0;
        if (FILE* f = std::fopen(path, "r")) {
            if (std::fscanf(f, "%u", &v) != 1) v = 0;
            std::fclose(f);
        }
        khz.push_back(v);
    }
    CoreMasks cm = classify_cores(khz);
    if (cm.hybrid) {
        out->cpu_performance_cores = cm.performance_count;
        out->cpu_efficiency_cores = cm.efficiency_count;
    }
    out->cpu_max_mhz = cm.max_khz / 1000;
    out->cpu_features = (d.is_64bit && !std::strstr(d.abi, "x86")) ? NUE_CPU_NEON : 0;
    out->memory_total_bytes = d.total_ram_bytes;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        out->memory_available_bytes = st.mem.available_ram;
    }
    out->gpu_count = 0;
    if (g.probed) {
        nue_gpu_info& gi = out->gpus[0];
        std::memset(&gi, 0, sizeof gi);
        set_cstr(gi.name, g.device_name);
        gi.vendor_id = g.vendor_id;
        gi.dedicated_memory_bytes = 0; // phones have unified memory
        gi.shared_memory_bytes = g.device_local_bytes ? g.device_local_bytes : d.total_ram_bytes;
        gi.is_integrated = NUE_TRUE;
        gi.is_default = NUE_TRUE;
        out->gpu_count = 1;
    }
    out->display_count = 1;
    return NUE_OK;
}

// Process / GPU memory. Android has no fixed per-process cap for native memory: the limit that matters is the low-memory
// killer, so process_limit stays 0 (unknown) unless the application configured one with nue_android_set_memory_cap, and
// process_available is what the system can still give above its kill threshold.
nue_result p_memory_info_ex(nue_memory_info* io) {
    State& st = state();
    uint64_t cap, avail, thr, rss = 0, total, shared;
    int pressure;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        cap = st.memory_cap;
        avail = st.mem.available_ram;
        thr = st.mem.low_threshold;
        total = st.mem.total_ram;
        pressure = static_cast<int>(st.pressure);
        shared = st.gpu.probed && st.gpu.device_local_bytes ? st.gpu.device_local_bytes : total;
    }
    NueAndroidMemoryBudget mb{};
    mb.struct_size = sizeof mb;
    if (nue_android_get_memory_budget(&mb)) rss = mb.rss_bytes;
    io->process_used = rss;
    io->process_limit = cap;
    uint64_t free_above = avail > thr ? avail - thr : 0;
    io->process_available = cap && cap > rss ? std::min<uint64_t>(cap - rss, free_above) : (cap ? 0 : free_above);
    io->gpu_dedicated_total = io->gpu_dedicated_available = 0; // unified memory
    io->gpu_shared_total = shared;
    io->unified_memory = NUE_TRUE;
    io->pressure = pressure_to_nue(pressure == static_cast<int>(MemPressure::Background) ? NUE_ANDROID_MEM_MODERATE : pressure);
    return NUE_OK;
}

// GPU facts without a device: from the Vulkan / EGL probe (api_info), mapped to the core's format families.
nue_result p_gpu_probe(nue_gpu_caps* out) {
    NueAndroidGpuInfo g{};
    g.struct_size = sizeof g;
    if (!nue_android_get_gpu_info(&g)) return NUE_ERR_NOT_INITIALIZED;
    const bool vk = g.prefer_vulkan != 0;
    const uint32_t caps = vk ? g.vk_tex_caps : g.gles_tex_caps;
    const nue_format_support base = NUE_FMTSUP_SAMPLE | NUE_FMTSUP_FILTER | NUE_FMTSUP_MIPMAPS;
    const nue_format_support rt = base | NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_BLEND;
    auto set = [&](nue_format_family f, nue_format_support s) { out->format[f] = s; };

    out->probed = NUE_TRUE;
    out->renderer = vk ? NUE_RENDERER_VULKAN : NUE_RENDERER_GLES;
    // Mandatory in OpenGL ES 3.0 and in Vulkan on every device that ships Android 10+ with these formats usable.
    set(NUE_FMT_RGBA8, rt | NUE_FMTSUP_SRGB);
    set(NUE_FMT_BGRA8, (caps & NUE_ANDROID_TEX_BGRA8) ? (rt | NUE_FMTSUP_SRGB) : 0);
    set(NUE_FMT_RGB565, rt);
    set(NUE_FMT_RGBA4444, rt);
    set(NUE_FMT_RGB5A1, rt);
    set(NUE_FMT_R8, rt);
    set(NUE_FMT_RG8, rt);
    set(NUE_FMT_R16F, rt);
    set(NUE_FMT_RGBA16F, rt);
    set(NUE_FMT_RGBA32F, NUE_FMTSUP_SAMPLE | NUE_FMTSUP_MIPMAPS); // not filterable without an extension: conservative
    set(NUE_FMT_RGB10A2, rt);
    set(NUE_FMT_DEPTH16, NUE_FMTSUP_SAMPLE | NUE_FMTSUP_RENDER_TARGET);
    set(NUE_FMT_DEPTH24_STENCIL8, (caps & NUE_ANDROID_TEX_D24S8) ? (NUE_FMTSUP_SAMPLE | NUE_FMTSUP_RENDER_TARGET) : 0);
    set(NUE_FMT_DEPTH32F, NUE_FMTSUP_SAMPLE | NUE_FMTSUP_RENDER_TARGET);
    set(NUE_FMT_DEPTH32F_STENCIL8, (caps & NUE_ANDROID_TEX_D32S8) ? (NUE_FMTSUP_SAMPLE | NUE_FMTSUP_RENDER_TARGET) : 0);
    const nue_format_support comp = base | NUE_FMTSUP_SRGB;
    if (caps & NUE_ANDROID_TEX_BC1_3) {
        set(NUE_FMT_BC1, comp);
        set(NUE_FMT_BC2, comp);
        set(NUE_FMT_BC3, comp);
    }
    if (caps & NUE_ANDROID_TEX_BC4_5) {
        set(NUE_FMT_BC4, base);
        set(NUE_FMT_BC5, base);
    }
    if (caps & NUE_ANDROID_TEX_BC6H_7) {
        set(NUE_FMT_BC6H, base);
        set(NUE_FMT_BC7, comp);
    }
    if (caps & NUE_ANDROID_TEX_ETC2) { // ETC2 + EAC are one feature in Vulkan and mandatory in OpenGL ES 3
        set(NUE_FMT_ETC1, base);
        set(NUE_FMT_ETC2_RGB, comp);
        set(NUE_FMT_ETC2_RGBA, comp);
        set(NUE_FMT_EAC_R11, base);
        set(NUE_FMT_EAC_RG11, base);
    }
    if (caps & NUE_ANDROID_TEX_ASTC_LDR) set(NUE_FMT_ASTC_LDR, comp); // ASTC HDR / PVRTC / ATC not measured: left 0
    out->max_texture_2d = g.max_texture_size > 0 ? static_cast<uint32_t>(g.max_texture_size) : 4096;
    out->max_texture_cube = out->max_texture_2d;
    out->max_texture_3d = std::min<uint32_t>(out->max_texture_2d, 2048);
    out->max_array_layers = 256;                          // ES 3.0 / Vulkan guaranteed minimum
    out->max_anisotropy = (caps & NUE_ANDROID_TEX_ANISO) ? 16 : 1;
    out->max_render_targets = 4;                          // guaranteed minimum on both APIs
    out->max_msaa_samples = g.max_msaa > 0 ? static_cast<uint32_t>(g.max_msaa) : 4;
    out->unified_memory = NUE_TRUE;
    out->npot_mipmaps = NUE_TRUE;
    out->texture_compare = NUE_TRUE;
    out->compute = (vk || g.gles_major > 3 || (g.gles_major == 3 && g.gles_minor >= 1)) ? NUE_TRUE : NUE_FALSE;
    out->instancing = NUE_TRUE;
    out->tessellation = NUE_FALSE;
    out->dedicated_memory_bytes = 0;
    return NUE_OK;
}

// Compositor cooperation: Surface.setFrameRate vote + the display mode that fits the frame rate best.
nue_result p_window_request_refresh(const nue_native_window*, float refresh_hz, float frame_limit_fps) {
    const float fps = frame_limit_fps > 0 ? frame_limit_fps : refresh_hz;
    if (fps <= 0) {
        nue_android_set_frame_rate(0.0f);
        return NUE_OK;
    }
    nue_android_set_frame_rate(fps);
    nue_android_set_preferred_refresh(fps);
    return NUE_OK;
}

nue_result p_memory_info(uint64_t* total, uint64_t* available) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    *total = st.device.total_ram_bytes;
    *available = st.mem.available_ram;
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------- storage

nue_result p_storage_root(nue_root_kind kind, const nue_init_desc*, char* buffer, size_t capacity, size_t* out_size) {
    State& st = state();
    std::string path;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        switch (kind) {
        case NUE_ROOT_DATA: path = st.storage.game_dir; break;
        case NUE_ROOT_USER: path = st.storage.files_dir; break;
        case NUE_ROOT_CACHE: path = st.storage.cache_dir; break;
        case NUE_ROOT_SETTINGS: path = st.storage.config_dir; break;
        case NUE_ROOT_TEMP: path = std::string(st.storage.cache_dir) + "/tmp"; break;
        default: return NUE_ERR_UNSUPPORTED;
        }
    }
    if (path.empty() || path == "/tmp") return NUE_ERR_NOT_FOUND;
    return put_string(path, buffer, capacity, out_size);
}

// Import source: PATH reads a directory with plain file access, URI / PICKER read a document tree through the Storage
// Access Framework (file list and descriptors come from Java).
struct ImportSrc {
    nue_import_source_kind kind;
    std::unique_ptr<ImportSource> dir;   // PATH
    std::unique_ptr<ByteSource> bytes;   // PATH, current entry
    int count = 0;                       // URI
    int index = -1;                      // entry returned by the last next()
    int fd = -1;                         // URI, current entry
};

nue_result java_call_int(jmethodID id, int* out, jint arg) {
    State& st = state();
    JniEnv env;
    if (!env || !id) return NUE_ERR_BACKEND;
    *out = env->CallStaticIntMethod(st.bridge, id, arg);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return NUE_ERR_BACKEND;
    }
    return NUE_OK;
}

nue_result i_open(nue_import_source_kind kind, const char* source, void** out) {
    if (!source || !*source) return NUE_ERR_INVALID_ARGUMENT;
    auto s = std::make_unique<ImportSrc>();
    s->kind = kind;
    if (kind == NUE_IMPORT_SOURCE_PATH) {
        s->dir = make_directory_source(source);
    } else if (kind == NUE_IMPORT_SOURCE_URI) {
        State& st = state();
        JniEnv env;
        if (!env || !st.m.treeOpen) return NUE_ERR_BACKEND;
        jstring uri = env->NewStringUTF(source);
        jint n = env->CallStaticIntMethod(st.bridge, st.m.treeOpen, uri);
        env->DeleteLocalRef(uri);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return NUE_ERR_BACKEND;
        }
        if (n < 0) return NUE_ERR_ACCESS_DENIED; // permission for the tree was revoked: ask the user again
        s->count = n;
    } else {
        return NUE_ERR_UNSUPPORTED;
    }
    *out = s.release();
    return NUE_OK;
}

void i_drop_current(ImportSrc* s) {
    s->bytes.reset();
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

nue_result i_next(void* source, nue_import_entry* entry) {
    ImportSrc* s = static_cast<ImportSrc*>(source);
    i_drop_current(s);
    ++s->index;
    std::string path;
    uint64_t size = 0;
    if (s->kind == NUE_IMPORT_SOURCE_PATH) {
        const std::vector<SourceFile>& files = s->dir->files();
        if (s->index >= static_cast<int>(files.size())) return NUE_ERR_END_OF_DATA;
        path = files[s->index].path;
        size = files[s->index].size;
    } else {
        if (s->index >= s->count) return NUE_ERR_END_OF_DATA;
        State& st = state();
        JniEnv env;
        if (!env) return NUE_ERR_BACKEND;
        jstring js = static_cast<jstring>(env->CallStaticObjectMethod(st.bridge, st.m.treePath, static_cast<jint>(s->index)));
        jlong sz = env->CallStaticLongMethod(st.bridge, st.m.treeSize, static_cast<jint>(s->index));
        if (env->ExceptionCheck() || !js) {
            env->ExceptionClear();
            return NUE_ERR_BACKEND;
        }
        const char* c = env->GetStringUTFChars(js, nullptr);
        if (c) path = c;
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
        size = sz < 0 ? 0 : static_cast<uint64_t>(sz);
    }
    if (path.size() >= sizeof entry->path) return NUE_ERR_OVERFLOW;
    std::memcpy(entry->path, path.c_str(), path.size() + 1);
    entry->size = size;
    return NUE_OK;
}

nue_result i_read(void* source, void* buffer, size_t capacity, size_t* out_read) {
    ImportSrc* s = static_cast<ImportSrc*>(source);
    *out_read = 0;
    if (s->index < 0) return NUE_ERR_BAD_STATE;
    if (s->kind == NUE_IMPORT_SOURCE_PATH) {
        if (!s->bytes) {
            s->bytes = s->dir->open(static_cast<size_t>(s->index));
            if (!s->bytes) return NUE_ERR_IO;
        }
        int64_t n = s->bytes->read(buffer, capacity);
        if (n < 0) return NUE_ERR_IO;
        *out_read = static_cast<size_t>(n);
        return NUE_OK;
    }
    if (s->fd < 0) {
        int fd = -1;
        if (java_call_int(state().m.treeFd, &fd, s->index) != NUE_OK || fd < 0) return NUE_ERR_IO;
        s->fd = fd;
    }
    for (;;) {
        ssize_t n = read(s->fd, buffer, capacity);
        if (n >= 0) {
            *out_read = static_cast<size_t>(n);
            return NUE_OK;
        }
        if (errno != EINTR) return NUE_ERR_IO;
    }
}

void i_close(void* source) {
    ImportSrc* s = static_cast<ImportSrc*>(source);
    if (!s) return;
    i_drop_current(s);
    if (s->kind == NUE_IMPORT_SOURCE_URI) {
        State& st = state();
        JniEnv env;
        if (env && st.m.treeClose) {
            env->CallStaticVoidMethod(st.bridge, st.m.treeClose);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    delete s;
}

nue_result i_pick_folder(void (*done)(void*, nue_result, const char*), void* userdata) {
    if (!done) return NUE_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(g_pick_mu);
        if (g_pick_done) return NUE_ERR_BUSY;
        g_pick_done = done;
        g_pick_user = userdata;
    }
    nue_android_pick_game_folder();
    return NUE_OK_PENDING;
}

const nue_import_source_ops g_import_ops = {i_open, i_next, i_read, i_close, i_pick_folder};

// ---------------------------------------------------------------------------------------------------- power

nue_result p_power_info(nue_power_info* out) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    const ThermalAdvice& a = st.thermal.advice();
    out->state = power_state_now(st.battery_percent, st.charging != 0);
    out->battery_percent = st.battery_percent;
    out->seconds_left = -1;
    out->on_ac_power = st.charging ? NUE_TRUE : NUE_FALSE;
    out->low_power_mode = st.power_save ? NUE_TRUE : NUE_FALSE;
    out->thermal = thermal_from_level(st.thermal_status, static_cast<int>(a.level));
    out->game_mode = game_mode_from(st.game_mode);
    out->thermal_headroom = std::isnan(st.thermal_headroom) ? -1.0f : st.thermal_headroom;
    out->display_keep_awake_active = (g_request.keep_awake & NUE_KEEP_AWAKE_DISPLAY) ? NUE_TRUE : NUE_FALSE;
    return NUE_OK;
}

nue_result p_power_apply(const nue_power_request* r, nue_power_applied* out_applied) {
    State& st = state();
    int sdk;
    bool sustained_ok;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        sdk = st.device.sdk_int;
        sustained_ok = st.sustained_supported != 0;
    }
    uint32_t requested = 0, applied = 0;
    auto want = [&](uint32_t bit, bool honoured) {
        requested |= bit;
        if (honoured) applied |= bit;
    };

    // profile: SUSTAINED -> Window.setSustainedPerformanceMode (stable clocks). The other profiles have no OS lever on
    // Android; DEFAULT / BALANCED are honoured by doing nothing, HIGH and POWER_SAVER are not.
    const bool sustained = r->profile == NUE_PERF_SUSTAINED;
    if (r->profile != NUE_PERF_DEFAULT) {
        bool ok = r->profile == NUE_PERF_BALANCED || (sustained && sustained_ok);
        want(NUE_POWER_APPLIED_PROFILE, ok);
    }
    nue_android_set_sustained_performance(sustained && sustained_ok ? 1 : 0);

    // game hint -> GameManager.setGameState (API 33)
    if (r->game_hint != NUE_GAME_HINT_NONE || g_request.game_hint != NUE_GAME_HINT_NONE) {
        bool loading = r->game_hint == NUE_GAME_HINT_LOADING;
        int mode = 0; // GameState.MODE_NONE
        if (r->game_hint == NUE_GAME_HINT_GAMEPLAY) mode = 1;      // MODE_GAMEPLAY_UNINTERRUPTIBLE
        else if (r->game_hint == NUE_GAME_HINT_MENU) mode = 3;     // MODE_CONTENT
        nue_android_set_game_state(loading ? 1 : 0, mode);
        if (r->game_hint != NUE_GAME_HINT_NONE) want(NUE_POWER_APPLIED_GAME_HINT, sdk >= 33);
    }

    // keep awake: the display flag only (a system wake lock is only held during the data import)
    if (r->keep_awake) want(NUE_POWER_APPLIED_KEEP_AWAKE, (r->keep_awake & ~NUE_KEEP_AWAKE_DISPLAY) == 0);
    nue_android_set_keep_screen_on((r->keep_awake & NUE_KEEP_AWAKE_DISPLAY) ? 1 : 0);

    // frame time -> compositor frame-rate vote (API 30) + display mode choice
    if (r->target_frame_time_us) {
        float fps = 1e6f / float(r->target_frame_time_us);
        if (std::fabs(fps - g_requested_fps) > 0.5f) {
            g_requested_fps = fps;
            nue_android_set_frame_rate(fps);
            nue_android_set_preferred_refresh(fps);
        }
        want(NUE_POWER_APPLIED_FRAME_TIME, sdk >= 30);
    } else if (g_requested_fps > 0) {
        g_requested_fps = 0;
        nue_android_set_frame_rate(0.0f);
    }

    // things Android does not offer
    if (r->gpu != NUE_GPU_DEFAULT) want(NUE_POWER_APPLIED_GPU, false);
    if (r->cpu != NUE_CPU_DEFAULT) want(NUE_POWER_APPLIED_CPU, false); // per thread only: nue_thread_set_core_class
    if (r->timer_resolution_us) want(NUE_POWER_APPLIED_TIMER, false);
    if (r->disable_power_throttling) want(NUE_POWER_APPLIED_THROTTLING, false);
    if (r->prefer_low_latency_audio) want(NUE_POWER_APPLIED_AUDIO, false); // SDL3 opens AAudio in low latency mode already

    g_request = *r;
    if (out_applied) *out_applied = applied;
    if (requested == 0) return NUE_OK_NOOP;
    return applied == requested ? NUE_OK : NUE_OK_PARTIAL;
}

// ---------------------------------------------------------------------------------------------------- threads

pid_t tid_of(const nue_native_thread_ref* t) {
    if (!t || (t->id == 0 && t->handle == 0)) return static_cast<pid_t>(syscall(SYS_gettid));
    if (t->id) return static_cast<pid_t>(t->id);
    return pthread_gettid_np(static_cast<pthread_t>(t->handle));
}

nue_result p_thread_set_priority(const nue_native_thread_ref* t, nue_thread_priority p) {
    int nice = nice_from_priority(static_cast<int>(p));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid_of(t)), nice) != 0) {
        // raising priority can be refused on some devices: lower to what is allowed, report partial
        if (nice < 0 && setpriority(PRIO_PROCESS, static_cast<id_t>(tid_of(t)), 0) == 0) return NUE_OK_PARTIAL;
        return NUE_ERR_ACCESS_DENIED;
    }
    return NUE_OK;
}

nue_result p_thread_get_priority(const nue_native_thread_ref* t, nue_thread_priority* out) {
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, static_cast<id_t>(tid_of(t)));
    if (nice == -1 && errno != 0) return NUE_ERR_FAILED;
    *out = static_cast<nue_thread_priority>(priority_from_nice(nice));
    return NUE_OK;
}

nue_result set_affinity_mask(const nue_native_thread_ref* t, uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n > 64) n = 64;
    for (long i = 0; i < n; ++i)
        if (mask == 0 || (mask >> i) & 1u) CPU_SET(static_cast<int>(i), &set);
    if (sched_setaffinity(tid_of(t), sizeof set, &set) != 0) return NUE_ERR_ACCESS_DENIED;
    return NUE_OK;
}

nue_result p_thread_set_affinity(const nue_native_thread_ref* t, uint64_t mask) { return set_affinity_mask(t, mask); }

CoreMasks read_core_masks() {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n > 64) n = 64;
    std::vector<uint32_t> khz;
    for (long i = 0; i < n; ++i) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
        uint32_t v = 0;
        if (FILE* f = std::fopen(path, "r")) {
            if (std::fscanf(f, "%u", &v) != 1) v = 0;
            std::fclose(f);
        }
        khz.push_back(v);
    }
    return classify_cores(khz);
}

nue_result p_thread_set_core_class(const nue_native_thread_ref* t, nue_cpu_preference pref) {
    if (pref == NUE_CPU_DEFAULT) return set_affinity_mask(t, 0);
    CoreMasks m = read_core_masks();
    if (!m.hybrid) return NUE_ERR_UNSUPPORTED; // symmetric CPU, or sysfs not readable by apps on this device
    return set_affinity_mask(t, pref == NUE_CPU_PERFORMANCE_CORES ? m.performance : m.efficiency);
}

nue_result p_thread_set_name(const char* name) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", name ? name : "");
    return prctl(PR_SET_NAME, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(buf)), 0, 0, 0) == 0 ? NUE_OK : NUE_ERR_FAILED;
}

nue_result p_perf_hint_create(const nue_native_thread_ref* threads, uint32_t count, int64_t target_ns, void** out) {
    if (!adpf_available()) return NUE_ERR_UNSUPPORTED;
    std::vector<int32_t> tids;
    if (!threads || count == 0) tids.push_back(static_cast<int32_t>(tid_of(nullptr)));
    for (uint32_t i = 0; threads && i < count; ++i) tids.push_back(static_cast<int32_t>(tid_of(&threads[i])));
    HintSession* s = adpf_create(tids.data(), tids.size(), target_ns);
    if (!s) return NUE_ERR_BACKEND;
    *out = s;
    return NUE_OK;
}
void p_perf_hint_destroy(void* impl) { adpf_destroy(static_cast<HintSession*>(impl)); }
nue_result p_perf_hint_update_target(void* impl, int64_t ns) {
    adpf_update(static_cast<HintSession*>(impl), ns);
    return NUE_OK;
}
nue_result p_perf_hint_report(void* impl, int64_t ns) {
    adpf_report(static_cast<HintSession*>(impl), ns);
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------- haptics

nue_bool p_haptics_available(void) { return nue_android_has_vibrator() ? NUE_TRUE : NUE_FALSE; }

nue_result p_haptics_vibrate(uint32_t duration_ms, float intensity) {
    int amp = intensity <= 0.0f ? -1 : static_cast<int>(std::lround(std::min(1.0f, intensity) * 255.0f));
    nue_android_vibrate(static_cast<int>(duration_ms), amp);
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------- crash reports

nue_crash_sink g_sink;
char g_signal_reason[32];

const char* sink_begin(int sig, uint64_t fault_addr, uint64_t fault_pc, void*) {
    // async-signal-safe: tiny hand-made formatting of the reason string
    const char* name = "signal";
    switch (sig) {
    case 11: name = "SIGSEGV"; break;
    case 6: name = "SIGABRT"; break;
    case 7: name = "SIGBUS"; break;
    case 8: name = "SIGFPE"; break;
    case 4: name = "SIGILL"; break;
    case 5: name = "SIGTRAP"; break;
    case 31: name = "SIGSYS"; break;
    }
    size_t i = 0;
    while (name[i] && i < sizeof g_signal_reason - 1) {
        g_signal_reason[i] = name[i];
        ++i;
    }
    g_signal_reason[i] = '\0';
    static nue_crash_info info;
    info.struct_size = sizeof info;
    info.reason = g_signal_reason;
    info.fault_address = fault_addr;
    info.fault_pc = fault_pc;
    info.signal_or_code = static_cast<uint32_t>(sig);
    info.report_dir = nullptr;
    return g_sink.begin(&info, g_sink.userdata);
}

bool sink_end(int sig, uint64_t fault_addr, uint64_t fault_pc, const char* dir, void*) {
    static nue_crash_info info;
    info.struct_size = sizeof info;
    info.reason = g_signal_reason;
    info.fault_address = fault_addr;
    info.fault_pc = fault_pc;
    info.signal_or_code = static_cast<uint32_t>(sig);
    info.report_dir = dir;
    return g_sink.end(&info, g_sink.userdata) != 0;
}

std::string g_report_root_native;

nue_result p_crash_install(const nue_crash_desc* desc, const nue_crash_sink* sink) {
    if (!sink || !sink->begin || !sink->end) return NUE_ERR_INVALID_ARGUMENT;
    g_sink = *sink;
    // reports of earlier runs: turn raw records into readable stack.txt files
    char root[512];
    size_t need = 0;
    const char* vfs = desc && desc->report_root ? desc->report_root : "user:crash";
    if (nue_vfs_resolve(vfs, NUE_TRUE, root, sizeof root, &need) >= 0) {
        g_report_root_native = root;
        crash_convert_tree(g_report_root_native);
    }
    CrashHooks hooks;
    hooks.begin = sink_begin;
    hooks.end = sink_end;
    State& st = state();
    char version[32];
    {
        std::lock_guard<std::mutex> lock(st.mu);
        std::snprintf(version, sizeof version, "nue %s", "0.1");
    }
    crash_install(hooks, version);
    return NUE_OK;
}

void p_crash_uninstall(void) {
    crash_uninstall();
}

nue_result p_crash_write_dump(const char* directory, nue_crash_flags) {
    if (!directory) return NUE_ERR_INVALID_ARGUMENT;
    return crash_write_stack_text(directory) ? NUE_OK : NUE_ERR_IO;
}

// ---------------------------------------------------------------------------------------------------- logging

void p_log_write(nue_log_level level, const char* tag, const char* message) {
    int prio = ANDROID_LOG_INFO;
    switch (level) {
    case NUE_LOG_TRACE: prio = ANDROID_LOG_VERBOSE; break;
    case NUE_LOG_DEBUG: prio = ANDROID_LOG_DEBUG; break;
    case NUE_LOG_INFO: prio = ANDROID_LOG_INFO; break;
    case NUE_LOG_WARN: prio = ANDROID_LOG_WARN; break;
    case NUE_LOG_ERROR: prio = ANDROID_LOG_ERROR; break;
    case NUE_LOG_FATAL: prio = ANDROID_LOG_FATAL; break;
    default: break;
    }
    __android_log_write(prio, tag && *tag ? tag : NUE_LOG_TAG, message ? message : "");
}

} // namespace

extern "C" const nue_platform_ops* nue_platform_android_get_ops(void) {
    static bool built = false;
    if (!built) {
        built = true;
        std::memset(&g_ops, 0, sizeof g_ops);
        g_ops.struct_size = sizeof g_ops;
        g_ops.name = "android";
        g_ops.init = p_init;
        g_ops.shutdown = p_shutdown;
        g_ops.pump = p_pump;
        g_ops.system_info = p_system_info;
        g_ops.memory_info = p_memory_info;
        g_ops.memory_info_ex = p_memory_info_ex;
        g_ops.gpu_probe = p_gpu_probe;
        g_ops.window_request_refresh = p_window_request_refresh;
        g_ops.owns_lifecycle = NUE_TRUE; // Activity callbacks push the app state; the SDL3 backend must not derive it
        g_ops.storage_root = p_storage_root;
        g_ops.import = &g_import_ops;
        g_ops.power_info = p_power_info;
        g_ops.power_apply = p_power_apply;
        g_ops.thread_set_priority = p_thread_set_priority;
        g_ops.thread_get_priority = p_thread_get_priority;
        g_ops.thread_set_affinity = p_thread_set_affinity;
        g_ops.thread_set_core_class = p_thread_set_core_class;
        g_ops.thread_set_name = p_thread_set_name;
        g_ops.perf_hint_create = p_perf_hint_create;
        g_ops.perf_hint_destroy = p_perf_hint_destroy;
        g_ops.perf_hint_update_target = p_perf_hint_update_target;
        g_ops.perf_hint_report = p_perf_hint_report;
        g_ops.haptics_available = p_haptics_available;
        g_ops.haptics_vibrate = p_haptics_vibrate;
        g_ops.crash_install = p_crash_install;
        g_ops.crash_uninstall = p_crash_uninstall;
        g_ops.crash_write_dump = p_crash_write_dump;
        g_ops.log_write = p_log_write;
        // capabilities[] are completed in init(), after Java pushed the device facts
    }
    return &g_ops;
}
