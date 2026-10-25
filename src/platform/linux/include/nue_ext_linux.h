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

/* Which glyph set a HUD should draw for a controller (superset of nue_glyph_family: STEAM is the Steam Deck / Steam Controller set). */
typedef enum nue_linux_glyph_family {
    NUE_LINUX_GLYPHS_KEYBOARD_MOUSE = 0, NUE_LINUX_GLYPHS_XBOX, NUE_LINUX_GLYPHS_PLAYSTATION, NUE_LINUX_GLYPHS_NINTENDO,
    NUE_LINUX_GLYPHS_STEAM, NUE_LINUX_GLYPHS_GENERIC
} nue_linux_glyph_family;

typedef enum nue_linux_steam_hardware {
    NUE_LINUX_STEAM_HW_NONE = 0, NUE_LINUX_STEAM_HW_DECK_LCD, NUE_LINUX_STEAM_HW_DECK_OLED, NUE_LINUX_STEAM_HW_STEAM_MACHINE,
    NUE_LINUX_STEAM_HW_STEAM_FRAME, NUE_LINUX_STEAM_HW_OTHER_VALVE
} nue_linux_steam_hardware;

typedef enum nue_linux_session_mode { NUE_LINUX_SESSION_UNKNOWN = 0, NUE_LINUX_SESSION_GAMING, NUE_LINUX_SESSION_DESKTOP } nue_linux_session_mode;

typedef struct nue_linux_steamos_info {
    uint32_t struct_size;
    nue_linux_steam_hardware hardware;
    nue_bool hardware_is_heuristic;         /* inferred (Steam Machine / Frame, or SteamDeck=1 without DMI), not matched to a known product */
    nue_bool steamos_like;                  /* SteamOS or a derivative (Bazzite, ChimeraOS, HoloISO, Nobara, SteamFork, Jovian, ...) */
    char flavor[48];                        /* "SteamOS", "Bazzite", ... or "" */
    char distro_id[32];
    char variant_id[32];
    nue_bool immutable_root;                /* read-only / ostree root: install under $HOME or a Steam library */
    nue_linux_session_mode session_mode;    /* gaming (gamescope session) or desktop */
    nue_bool launched_by_steam, non_steam_shortcut, proton, steam_in_flatpak, steam_runtime_container;
    uint64_t steam_game_id;
    char steam_root[256];
    uint32_t sd_card_count;
    char first_sd_card[160];
    int32_t panel_width, panel_height, panel_min_hz, panel_max_hz;   /* 0 when the panel is not known */
    nue_bool panel_oled, hdr_available;
    nue_bool steam_input_active;            /* a Steam virtual gamepad exists */
    nue_bool physical_pads_hidden;          /* SDL_GAMECONTROLLER_IGNORE_DEVICES is set */
    int32_t visible_gamepads;
    nue_bool raw_deck_access, gyro_via_sdl, back_buttons_via_sdl;
    nue_linux_glyph_family default_glyphs;
    uint32_t reserved[8];
} nue_linux_steamos_info;

typedef struct nue_linux_frame_pacing {
    uint32_t struct_size;
    int32_t target_fps;
    nue_bool present_vsync;                 /* present with FIFO / vsync */
    nue_bool own_limiter;                   /* sleep to target_fps (nue_sleep_until_ns) */
    nue_bool judder_expected;
    int32_t suggested_steam_limit;          /* frame limit to select in the Steam quick access menu, 0 = none */
    char notes[256];
    uint32_t reserved[4];
} nue_linux_frame_pacing;

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

    /* ---- SteamOS and Steam (appended; check struct_size >= offsetof before use) ------------------------------------- */
    nue_result (*get_steamos_info)(nue_linux_steamos_info* out);
    /* Frame pacing next to gamescope for a game that runs at `game_fps`; display_hz is the refresh rate the window reports (0 unknown). */
    nue_result (*plan_frame_pacing)(int32_t game_fps, double display_hz, nue_linux_frame_pacing* out);
    /* A lower frame rate to use on low battery / thermal pressure (Deck panels step through 45 / 40 / 30); returns wanted_fps when no cap is needed. */
    nue_result (*suggest_fps_cap)(int32_t wanted_fps, int32_t* out_fps);
    /* Glyph family for a controller (nue_linux_glyph_family). */
    nue_result (*controller_glyph)(uint32_t vendor_id, uint32_t product_id, int32_t* out_family);
    /* Opens Steam's on-screen keyboard (steam://open/keyboard) when Steam is present. NUE_ERR_UNSUPPORTED without Steam. */
    nue_result (*show_onscreen_keyboard)(void);
    /* Directory for the engine's own shader / pipeline caches (inside Steam's per-app cache when launched by Steam). NUE_STRING_OUT. */
    nue_result (*shader_cache_dir)(char* buffer, size_t capacity, size_t* out_size);
    /* Install directory of a Steam app in any library (internal drive or SD card). NUE_ERR_NOT_FOUND if not installed. NUE_STRING_OUT. */
    nue_result (*find_steam_app)(uint64_t app_id, char* buffer, size_t capacity, size_t* out_size);
    /* Read-only snapshot of steamos-manager (TDP, GPU performance level, charge limit) and amdgpu sysfs as "key=value\n" lines. */
    nue_result (*describe_power_tuning)(char* buffer, size_t capacity, size_t* out_size);
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
