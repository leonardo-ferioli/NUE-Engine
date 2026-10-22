/*
 * NUE engine - Linux platform extension ("nue.linux.v1").
 *
 * Linux-only functionality that is not part of the portable API:
 *   - the case-insensitive path resolver the engine uses for Windows-style game paths,
 *   - what kind of session the game runs in (Steam Deck, gamescope, Flatpak, Wayland...),
 *   - the renderer/GPU plan (Vulkan first, GL/GLES fallback, GPU choice on hybrid machines),
 *   - a text report of the machine for logs and bug reports,
 *   - nue_linux_apply_early_policy(): audio/video/input environment for SDL, usable BEFORE nue_init.
 *
 *   const nue_ext_linux_v1* linux_ext = (const nue_ext_linux_v1*)nue_get_extension("nue.linux.v1");   // NULL on other OSes
 *
 * Pure C99 so it can be used from C hosts. All strings are UTF-8, output buffers follow the NUE_STRING_OUT convention
 * (always NUL terminated, *out_size = bytes needed including the terminator).
 */
#ifndef NUE_EXT_LINUX_H
#define NUE_EXT_LINUX_H

#include "nue/nue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nue_linux_session_info {
    uint32_t struct_size;
    nue_bool wayland_session, x11_only, xwayland, headless;
    nue_bool steam_deck, steamos, gamescope, steam_client, steam_runtime;
    nue_bool flatpak, snap, appimage, container, wsl;
    char deck_model[8];                     /* "lcd", "oled" or "" */
    char distro_id[32];                     /* os-release ID */
    char steam_runtime_name[32];            /* "sniper", "soldier" or "" */
    uint32_t reserved[8];
} nue_linux_session_info;

typedef struct nue_linux_renderer_plan {
    uint32_t struct_size;
    char api[16];                           /* "vulkan", "opengl", "opengles" or "none" */
    char bgfx_renderer[16];                 /* bgfx::RendererType name */
    uint32_t vendor_id, device_id;          /* bgfx::Init vendorId/deviceId; 0 = let bgfx choose */
    char device_name[128];
    nue_bool software;                      /* llvmpipe / lavapipe: expect single-digit frame rates */
    nue_bool hybrid_gpu;
    char fallbacks[48];                     /* comma separated api names */
    char environment[512];                  /* "NAME=VALUE\n" lines to set before the loader/GL is initialised */
    char warnings[512];                     /* '\n' separated */
    uint32_t reserved[8];
} nue_linux_renderer_plan;

/* resolve_path flags */
enum {
    NUE_LINUX_PATH_FOUND         = 1u << 0, /* the file or directory exists */
    NUE_LINUX_PATH_CASE_CHANGED  = 1u << 1, /* matched only after case folding */
    NUE_LINUX_PATH_AMBIGUOUS     = 1u << 2, /* several names differ only by case; the byte-wise smallest was chosen */
    NUE_LINUX_PATH_PARENT_EXISTS = 1u << 3  /* every directory before the last component exists (the file can be created) */
};

typedef struct nue_ext_linux_v1 {
    uint32_t struct_size;
    /* Windows-style path ("C:\Games\X\Data\a.BIN", ".\data\a.bin") -> real POSIX path below base_dir (NULL: the process working
     * directory). Drive letters are dropped unless mapped; ".." cannot climb out of base_dir. Directory listings are cached. */
    nue_result (*resolve_path)(const char* base_dir, const char* windows_path, char* buffer, size_t capacity, size_t* out_size, uint32_t* out_flags);
    nue_result (*get_session)(nue_linux_session_info* out);
    /* live_probe: also load libvulkan/libEGL and enumerate devices (slower, ~50 ms). */
    nue_result (*plan_renderer)(nue_bool live_probe, nue_linux_renderer_plan* out);
    /* Multi-line text, the same as the linux_probe sample prints. */
    nue_result (*describe)(nue_bool live_probe, char* buffer, size_t capacity, size_t* out_size);
    uint32_t reserved[8];
} nue_ext_linux_v1;

/* Policy flags for nue_linux_apply_early_policy */
enum {
    NUE_LINUX_POLICY_HEADLESS    = 1u << 0, /* SDL offscreen video + dummy audio: no window, no sound */
    NUE_LINUX_POLICY_NO_AUDIO    = 1u << 1,
    NUE_LINUX_POLICY_HIGH_LATENCY_AUDIO = 1u << 2  /* do not request a small audio buffer */
};

/* Sets environment variables (never overwriting ones the user already set) that SDL3 and the Vulkan/GL loaders read at
 * initialisation: SDL_AUDIO_DRIVER (PipeWire > PulseAudio > ALSA by what is running), SDL_VIDEO_DRIVER, PIPEWIRE_LATENCY,
 * PULSE_LATENCY_MSEC, GPU selection variables on hybrid machines, joystick hints. Call it BEFORE nue_init (or before SDL_Init
 * when SDL is used directly). Returns the number of variables set. Safe to call twice. Exported with default visibility. */
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
int32_t nue_linux_apply_early_policy(uint32_t flags);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_EXT_LINUX_H */
