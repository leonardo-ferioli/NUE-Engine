/*
 * NUE engine - system information and capability flags.
 *
 * Capabilities answer "can I rely on X here?" without platform #ifdefs in game code. The set is fixed per run; a few
 * (touch, gamepad rumble) can change when devices are plugged, and those have dedicated queries in input.h.
 */
#ifndef NUE_SYSTEM_H
#define NUE_SYSTEM_H

#include "nue/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nue_os_kind { NUE_OS_UNKNOWN = 0, NUE_OS_WINDOWS, NUE_OS_LINUX, NUE_OS_ANDROID, NUE_OS_IOS, NUE_OS_MACOS } nue_os_kind;
typedef enum nue_arch_kind { NUE_ARCH_KIND_UNKNOWN = 0, NUE_ARCH_KIND_X86, NUE_ARCH_KIND_X64, NUE_ARCH_KIND_ARM32, NUE_ARCH_KIND_ARM64 } nue_arch_kind;
typedef enum nue_device_class { NUE_DEVICE_UNKNOWN = 0, NUE_DEVICE_DESKTOP, NUE_DEVICE_LAPTOP, NUE_DEVICE_PHONE, NUE_DEVICE_TABLET, NUE_DEVICE_HANDHELD, NUE_DEVICE_TV } nue_device_class;

typedef uint32_t nue_cpu_features;
enum {
    NUE_CPU_SSE2 = 1u << 0, NUE_CPU_SSE41 = 1u << 1, NUE_CPU_SSE42 = 1u << 2, NUE_CPU_AVX = 1u << 3, NUE_CPU_AVX2 = 1u << 4,
    NUE_CPU_AVX512 = 1u << 5, NUE_CPU_NEON = 1u << 6, NUE_CPU_FMA = 1u << 7, NUE_CPU_POPCNT = 1u << 8
};

typedef struct nue_gpu_info {
    char name[128];
    uint32_t vendor_id, device_id;          /* PCI ids where known, 0 otherwise */
    uint64_t dedicated_memory_bytes;        /* 0 unknown */
    uint64_t shared_memory_bytes;
    nue_bool is_integrated;
    nue_bool is_default;                    /* the one the app will use unless nue_power_request.gpu says otherwise */
} nue_gpu_info;

#define NUE_MAX_GPUS 8

typedef struct nue_system_info {
    uint32_t struct_size;
    nue_os_kind os;
    char os_name[64];                       /* "Windows 11", "Ubuntu 24.04", "Android 15", "iOS 18.2" */
    char os_version[64];                    /* "10.0.26100", "6.8.0", "35", "18.2" */
    uint32_t os_build;                      /* Windows build number, Android API level, 0 otherwise */
    nue_arch_kind arch;
    nue_bool running_under_translation;     /* Rosetta, Windows-on-ARM x86 emulation, Wine/Proton */
    nue_device_class device_class;
    char device_model[96];                  /* "Pixel 8", "iPhone16,2", "" on PCs */
    char cpu_name[128];
    uint32_t cpu_logical_cores, cpu_physical_cores;
    uint32_t cpu_performance_cores, cpu_efficiency_cores;   /* 0 when not a hybrid design/unknown */
    uint32_t cpu_max_mhz;
    nue_cpu_features cpu_features;
    uint64_t memory_total_bytes, memory_available_bytes;
    uint32_t gpu_count;
    nue_gpu_info gpus[NUE_MAX_GPUS];
    uint32_t display_count;
    char locale[32];                        /* BCP-47 "en-US" */
    nue_bool dark_mode;
    uint32_t reserved[8];
    /* appended in 0.1.1 */
    uint32_t platform_flags;                /* nue_platform_flags */
    char platform_variant[64];              /* "SteamOS 3.6", "Bazzite", "ChimeraOS", "Steam Deck OLED", "" when none applies */
    char runtime_name[48];                  /* "Steam Linux Runtime 3.0 (sniper)", "Flatpak", "Wine 9.0", "" */
} nue_system_info;

typedef uint32_t nue_platform_flags;
enum {
    NUE_PLATFORM_HANDHELD         = 1u << 0,    /* handheld form factor (Steam Deck, ROG Ally, phones count through device_class) */
    NUE_PLATFORM_STEAMOS          = 1u << 1,    /* SteamOS or a SteamOS-like distribution (Bazzite, ChimeraOS, HoloISO, Nobara Steam edition...) */
    NUE_PLATFORM_STEAM_DECK       = 1u << 2,    /* the hardware is a Steam Deck (LCD or OLED: see platform_variant) */
    NUE_PLATFORM_STEAM_MACHINE    = 1u << 3,
    NUE_PLATFORM_STEAM_FRAME      = 1u << 4,
    NUE_PLATFORM_GAMESCOPE        = 1u << 5,    /* running inside a gamescope session (nested or as the compositor) */
    NUE_PLATFORM_STEAM_GAMING_MODE= 1u << 6,    /* the Steam gaming-mode session (console-like UI): fullscreen mode switches are the compositor's job */
    NUE_PLATFORM_STEAM_RUNTIME    = 1u << 7,    /* inside a Steam Linux Runtime container (pressure-vessel) */
    NUE_PLATFORM_FLATPAK          = 1u << 8,
    NUE_PLATFORM_READ_ONLY_ROOT   = 1u << 9,    /* the root file system is immutable (SteamOS, Bazzite): write only below user/cache/settings roots */
    NUE_PLATFORM_REMOVABLE_STORAGE= 1u << 10,   /* an SD card / external library path exists (see the "nue.linux.v1" extension for the list) */
    NUE_PLATFORM_COMPAT_LAYER     = 1u << 11,   /* Wine / Proton / Rosetta-like layer */
    NUE_PLATFORM_TV_UI            = 1u << 12    /* couch / TV session: assume controller-only navigation and large text */
};

NUE_API nue_result NUE_CALL nue_system_get_info(nue_system_info* out);
NUE_API nue_platform_flags NUE_CALL nue_system_get_platform_flags(void);                  /* [any] cheap; same value as nue_system_info.platform_flags */       /* [any] cached; memory fields refreshed on each call */
NUE_API nue_result NUE_CALL nue_system_get_memory(uint64_t* out_total, uint64_t* out_available);   /* [any] cheap refresh */

/* Capability flags (64 bit mask pair). Names are stable identifiers; values are never reused. */
typedef enum nue_capability {
    NUE_CAP_WINDOWS = 0,                    /* real windows (false in headless/null) */
    NUE_CAP_MULTIPLE_WINDOWS,
    NUE_CAP_EXCLUSIVE_FULLSCREEN,
    NUE_CAP_DISPLAY_MODE_CHANGE,
    NUE_CAP_ATTACH_NATIVE_WINDOW,
    NUE_CAP_HIGH_DPI,
    NUE_CAP_SAFE_AREA,
    NUE_CAP_CURSOR_CLIP,
    NUE_CAP_RELATIVE_MOUSE,
    NUE_CAP_KEYBOARD,
    NUE_CAP_MOUSE,
    NUE_CAP_GAMEPAD,
    NUE_CAP_GAMEPAD_RUMBLE,
    NUE_CAP_GAMEPAD_LED,
    NUE_CAP_GAMEPAD_SENSORS,
    NUE_CAP_TOUCH,
    NUE_CAP_SCREEN_KEYBOARD,
    NUE_CAP_ACCELEROMETER,
    NUE_CAP_GYROSCOPE,
    NUE_CAP_HAPTICS,
    NUE_CAP_AUDIO_OUTPUT,
    NUE_CAP_AUDIO_CAPTURE,
    NUE_CAP_AUDIO_LOW_LATENCY,
    NUE_CAP_AUDIO_DEVICE_SELECTION,
    NUE_CAP_MMAP_FILES,
    NUE_CAP_ASYNC_IO,
    NUE_CAP_DATA_IMPORT,
    NUE_CAP_DATA_IMPORT_PICKER,
    NUE_CAP_GAME_MODE,                      /* OS game mode can be read and/or requested */
    NUE_CAP_THERMAL_STATE,
    NUE_CAP_BATTERY,
    NUE_CAP_LOW_POWER_MODE,
    NUE_CAP_KEEP_AWAKE,
    NUE_CAP_TIMER_RESOLUTION,
    NUE_CAP_THREAD_PRIORITY,
    NUE_CAP_THREAD_AFFINITY,
    NUE_CAP_CORE_CLASS,
    NUE_CAP_PERF_HINT,
    NUE_CAP_GPU_PREFERENCE,
    NUE_CAP_CRASH_HANDLER,
    NUE_CAP_MINIDUMP,
    NUE_CAP_LIFECYCLE_PAUSE,                /* real pause/resume events (mobile) */
    NUE_CAP_GFX_D3D11, NUE_CAP_GFX_D3D12, NUE_CAP_GFX_VULKAN, NUE_CAP_GFX_METAL, NUE_CAP_GFX_OPENGL, NUE_CAP_GFX_GLES,
    NUE_CAP_GFX_D3D9_LAYER,                 /* nue_d3d9 translator linked and usable */
    NUE_CAP_COUNT
} nue_capability;

NUE_API nue_bool NUE_CALL nue_has_capability(nue_capability cap);              /* [any] */
NUE_API const char* NUE_CALL nue_capability_name(nue_capability cap);          /* "NUE_CAP_TOUCH" */
/* Fill `bits` (capacity words of 64 bits) with all flags, bit i = capability i. Returns the number of words needed. */
NUE_API uint32_t NUE_CALL nue_get_capabilities(uint64_t* bits, uint32_t capacity_words);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_SYSTEM_H */
