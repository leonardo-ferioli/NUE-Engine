// System information and capability flags.
#include "internal/core_internal.h"

#include <cstdio>
#include <cstring>
#include <thread>

namespace {

const char* const kCapNames[NUE_CAP_COUNT] = {
    "NUE_CAP_WINDOWS", "NUE_CAP_MULTIPLE_WINDOWS", "NUE_CAP_EXCLUSIVE_FULLSCREEN", "NUE_CAP_DISPLAY_MODE_CHANGE", "NUE_CAP_ATTACH_NATIVE_WINDOW",
    "NUE_CAP_HIGH_DPI", "NUE_CAP_SAFE_AREA", "NUE_CAP_CURSOR_CLIP", "NUE_CAP_RELATIVE_MOUSE", "NUE_CAP_KEYBOARD", "NUE_CAP_MOUSE", "NUE_CAP_GAMEPAD",
    "NUE_CAP_GAMEPAD_RUMBLE", "NUE_CAP_GAMEPAD_LED", "NUE_CAP_GAMEPAD_SENSORS", "NUE_CAP_TOUCH", "NUE_CAP_SCREEN_KEYBOARD", "NUE_CAP_ACCELEROMETER",
    "NUE_CAP_GYROSCOPE", "NUE_CAP_HAPTICS", "NUE_CAP_AUDIO_OUTPUT", "NUE_CAP_AUDIO_CAPTURE", "NUE_CAP_AUDIO_LOW_LATENCY", "NUE_CAP_AUDIO_DEVICE_SELECTION",
    "NUE_CAP_MMAP_FILES", "NUE_CAP_ASYNC_IO", "NUE_CAP_DATA_IMPORT", "NUE_CAP_DATA_IMPORT_PICKER", "NUE_CAP_GAME_MODE", "NUE_CAP_THERMAL_STATE",
    "NUE_CAP_BATTERY", "NUE_CAP_LOW_POWER_MODE", "NUE_CAP_KEEP_AWAKE", "NUE_CAP_TIMER_RESOLUTION", "NUE_CAP_THREAD_PRIORITY", "NUE_CAP_THREAD_AFFINITY",
    "NUE_CAP_CORE_CLASS", "NUE_CAP_PERF_HINT", "NUE_CAP_GPU_PREFERENCE", "NUE_CAP_CRASH_HANDLER", "NUE_CAP_MINIDUMP", "NUE_CAP_LIFECYCLE_PAUSE",
    "NUE_CAP_GFX_D3D11", "NUE_CAP_GFX_D3D12", "NUE_CAP_GFX_VULKAN", "NUE_CAP_GFX_METAL", "NUE_CAP_GFX_OPENGL", "NUE_CAP_GFX_GLES", "NUE_CAP_GFX_D3D9_LAYER"};
static_assert(sizeof(kCapNames) / sizeof(kCapNames[0]) == NUE_CAP_COUNT, "capability name table out of sync");

void set_bit(uint64_t* words, nue_capability c) { words[c / 64] |= 1ull << (c % 64); }

void compute_caps(uint64_t out[2]) {
    out[0] = out[1] = 0;
    const nue_backend* b = nuei::backend();
    const nue_platform_ops* p = nuei::platform();
    if (b) { out[0] |= b->capabilities[0]; out[1] |= b->capabilities[1]; }
    if (p) { out[0] |= p->capabilities[0]; out[1] |= p->capabilities[1]; }
    // capabilities the core implements itself
    set_bit(out, NUE_CAP_ASYNC_IO);
    set_bit(out, NUE_CAP_DATA_IMPORT);
    // derived from entries the platform layer filled in
    if (p) {
        if (p->power_apply) { set_bit(out, NUE_CAP_KEEP_AWAKE); }
        if (p->thread_set_priority) set_bit(out, NUE_CAP_THREAD_PRIORITY);
        if (p->thread_set_affinity) set_bit(out, NUE_CAP_THREAD_AFFINITY);
        if (p->thread_set_core_class) set_bit(out, NUE_CAP_CORE_CLASS);
        if (p->perf_hint_create) set_bit(out, NUE_CAP_PERF_HINT);
        if (p->crash_install) set_bit(out, NUE_CAP_CRASH_HANDLER);
        if (p->import && p->import->pick_folder) set_bit(out, NUE_CAP_DATA_IMPORT_PICKER);
        if (p->haptics_available && p->haptics_available()) set_bit(out, NUE_CAP_HAPTICS);
        if (p->window_safe_area) set_bit(out, NUE_CAP_SAFE_AREA);
        if (NUE_PLATFORM_ENTRY(p, screen_keyboard)) set_bit(out, NUE_CAP_SCREEN_KEYBOARD);
    }
}

}  // namespace

extern "C" {

NUE_API nue_result NUE_CALL nue_system_get_memory(uint64_t* total, uint64_t* avail) {
    uint64_t t = 0, a = 0;
    const nue_platform_ops* p = nuei::platform();
    if (p && p->memory_info && NUE_SUCCEEDED(p->memory_info(&t, &a))) { if (total) *total = t; if (avail) *avail = a; return NUE_OK; }
    if (total) *total = 0;
    if (avail) *avail = 0;
    return NUE_ERR_UNSUPPORTED;
}

NUE_API nue_result NUE_CALL nue_system_get_info(nue_system_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    nue_system_info s;
    std::memset(&s, 0, sizeof(s));
    s.struct_size = sizeof(s);
#if defined(NUE_PLATFORM_WINDOWS)
    s.os = NUE_OS_WINDOWS; std::snprintf(s.os_name, sizeof(s.os_name), "Windows");
#elif defined(NUE_PLATFORM_LINUX)
    s.os = NUE_OS_LINUX; std::snprintf(s.os_name, sizeof(s.os_name), "Linux");
#elif defined(NUE_PLATFORM_ANDROID)
    s.os = NUE_OS_ANDROID; std::snprintf(s.os_name, sizeof(s.os_name), "Android");
#elif defined(NUE_PLATFORM_IOS)
    s.os = NUE_OS_IOS; std::snprintf(s.os_name, sizeof(s.os_name), "iOS");
#elif defined(NUE_PLATFORM_MACOS)
    s.os = NUE_OS_MACOS; std::snprintf(s.os_name, sizeof(s.os_name), "macOS");
#endif
#if defined(NUE_ARCH_X64)
    s.arch = NUE_ARCH_KIND_X64;
#elif defined(NUE_ARCH_X86)
    s.arch = NUE_ARCH_KIND_X86;
#elif defined(NUE_ARCH_ARM64)
    s.arch = NUE_ARCH_KIND_ARM64;
#elif defined(NUE_ARCH_ARM32)
    s.arch = NUE_ARCH_KIND_ARM32;
#endif
    s.device_class = NUE_DEVICE_UNKNOWN;
#if defined(NUE_PLATFORM_ANDROID) || defined(NUE_PLATFORM_IOS)
    s.device_class = NUE_DEVICE_PHONE;
#elif defined(NUE_PLATFORM_WINDOWS) || defined(NUE_PLATFORM_LINUX) || defined(NUE_PLATFORM_MACOS)
    s.device_class = NUE_DEVICE_DESKTOP;
#endif
    s.cpu_logical_cores = std::thread::hardware_concurrency();
    s.cpu_physical_cores = s.cpu_logical_cores;
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    s.cpu_features |= NUE_CPU_SSE2;
#endif
#if defined(__SSE4_1__)
    s.cpu_features |= NUE_CPU_SSE41;
#endif
#if defined(__AVX__)
    s.cpu_features |= NUE_CPU_AVX;
#endif
#if defined(__AVX2__)
    s.cpu_features |= NUE_CPU_AVX2;
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
    s.cpu_features |= NUE_CPU_NEON;
#endif
    std::snprintf(s.locale, sizeof(s.locale), "en-US");
    const nue_platform_ops* p = nuei::platform();
    if (p && p->system_info) p->system_info(&s);   // may refine everything above
    if (p && p->system_locale) p->system_locale(s.locale, sizeof(s.locale));
    if (p && p->memory_info) p->memory_info(&s.memory_total_bytes, &s.memory_available_bytes);
    uint32_t dc = 0;
    if (nuei::initialized() && NUE_SUCCEEDED(nue_display_list(nullptr, 0, &dc))) s.display_count = dc;
    s.struct_size = sizeof(s);
    uint32_t keep = out->struct_size;
    std::memcpy(out, &s, keep < sizeof(s) ? keep : sizeof(s));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_platform_flags NUE_CALL nue_system_get_platform_flags(void) {
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    nue_system_get_info(&si);
    return si.platform_flags;
}

NUE_API nue_bool NUE_CALL nue_has_capability(nue_capability cap) {
    if (cap < 0 || cap >= NUE_CAP_COUNT) return NUE_FALSE;
    uint64_t w[2];
    compute_caps(w);
    return (w[cap / 64] >> (cap % 64)) & 1 ? NUE_TRUE : NUE_FALSE;
}

NUE_API const char* NUE_CALL nue_capability_name(nue_capability cap) { return (cap >= 0 && cap < NUE_CAP_COUNT) ? kCapNames[cap] : "NUE_CAP_UNKNOWN"; }

NUE_API uint32_t NUE_CALL nue_get_capabilities(uint64_t* bits, uint32_t capacity_words) {
    uint64_t w[2];
    compute_caps(w);
    for (uint32_t i = 0; bits && i < capacity_words && i < 2; ++i) bits[i] = w[i];
    return 2;
}

}  // extern "C"
