/*
 * NUE engine - INTERNAL backend interface (not installed, not part of the ABI).
 *
 * A backend provides raw devices: displays and windows, input devices, audio devices. The core (src/core) layers everything else on
 * top: event queue and state tables, "last used device", audio stream queues/conversion, the mixer, storage, config, log.
 * Two backends exist: null (src/core/backend_null.cpp, always built, drives the tests) and sdl3 (src/platform/sdl3).
 * Per-OS extras that SDL3 does not cover (power, game mode, storage rules, crash handlers...) are NOT backend functions: they are
 * platform ops (platform_ops.h) implemented in src/platform/<os>.
 *
 * Rules for backend authors
 *   - Every function is called with the core's API lock NOT held unless stated; the backend may call nue_internal_* functions.
 *   - Entries are optional (NULL) unless marked REQUIRED; the core answers NUE_ERR_UNSUPPORTED for a missing optional entry.
 *   - Backends report asynchronous changes by pushing events (nue_internal_push_event) and refreshing window snapshots.
 *   - Threading: init, shutdown, pump, the window_ and cursor_ entries run on the EVENT THREAD. the audio entries may be called from any thread; the
 *     audio callback you receive is yours to call from the device's thread. input_* queries run on any thread but are only
 *     reading snapshots that pump() maintains, except gamepad_rumble/led which may call the OS directly.
 */
#ifndef NUE_INTERNAL_BACKEND_H
#define NUE_INTERNAL_BACKEND_H

#include "nue/nue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ init */

typedef struct nue_backend_init {
    uint32_t struct_size;
    const nue_init_desc* desc;              /* the caller's descriptor (flags, loop model, app strings) */
    nue_bool headless;
    nue_bool deterministic;
} nue_backend_init;

/* ------------------------------------------------------------------------------------------------ video */

/* Everything the core caches about a window; the backend fills it on creation and on every change it detects. */
typedef struct nue_window_snapshot {
    uint32_t struct_size;
    int32_t x, y, width, height;            /* logical */
    int32_t pixel_width, pixel_height;
    float content_scale;
    float pixel_density;
    nue_display_id display;
    float refresh_rate_hz;
    nue_window_mode mode;
    nue_window_flags flags;                 /* current: RESIZABLE, BORDERLESS, ALWAYS_ON_TOP, MAXIMIZED, MINIMIZED, HIDDEN, HIGH_DPI */
    nue_bool focused, visible, occluded, mouse_inside;
    nue_rect_i safe_area;
    nue_native_window native;
} nue_window_snapshot;

typedef struct nue_backend_video {
    /* Displays. enum fills infos (up to capacity), *out_count = total. REQUIRED when video is supported. */
    nue_result (*display_enum)(nue_display_info* infos, uint32_t capacity, uint32_t* out_count);
    nue_result (*display_modes)(nue_display_id id, nue_display_mode* modes, uint32_t capacity, uint32_t* out_count);

    /* Windows. `impl` is the backend's private pointer, stored by the core and passed back. */
    nue_result (*window_create)(const nue_window_desc* desc, nue_window_id id, void** out_impl, nue_window_snapshot* out_snapshot);   /* REQUIRED */
    void (*window_destroy)(void* impl);                                                                                              /* REQUIRED; attached windows are only detached */
    nue_result (*window_refresh)(void* impl, nue_window_snapshot* out_snapshot);          /* re-read everything (called after pump and on demand) */
    nue_result (*window_set_title)(void* impl, const char* title);
    nue_result (*window_get_title)(void* impl, char* buffer, size_t capacity, size_t* out_size);
    nue_result (*window_set_size)(void* impl, int32_t w, int32_t h);
    nue_result (*window_set_position)(void* impl, int32_t x, int32_t y);
    nue_result (*window_set_size_limits)(void* impl, uint32_t min_w, uint32_t min_h, uint32_t max_w, uint32_t max_h);
    nue_result (*window_set_flag)(void* impl, nue_window_flags flag, nue_bool value);     /* RESIZABLE, BORDERLESS, ALWAYS_ON_TOP */
    nue_result (*window_show)(void* impl, nue_bool show);
    nue_result (*window_raise)(void* impl);
    nue_result (*window_minimize)(void* impl);
    nue_result (*window_maximize)(void* impl);
    nue_result (*window_restore)(void* impl);
    nue_result (*window_flash)(void* impl, nue_bool until_focused);
    nue_result (*window_set_mode)(void* impl, nue_window_mode mode, const nue_display_mode* exclusive_mode);
    nue_result (*window_move_to_display)(void* impl, nue_display_id display);
    nue_result (*window_set_cursor_clip)(void* impl, const nue_rect_i* rect_or_null, nue_bool enabled);
    nue_result (*window_set_relative_mouse)(void* impl, nue_bool enabled);
    nue_result (*window_warp_mouse)(void* impl, float x, float y);
    nue_result (*window_text_input)(void* impl, nue_bool start, const nue_rect_i* area, int32_t cursor);

    /* Global cursor. */
    nue_result (*cursor_set_visible)(nue_bool visible);
    nue_result (*cursor_set_kind)(nue_cursor_kind kind);
} nue_backend_video;

/* ------------------------------------------------------------------------------------------------ input */

typedef struct nue_backend_input {
    /* Gamepads/joysticks that are currently attached. Hot-plug is reported with GAMEPAD_ADDED/REMOVED events from pump(). */
    nue_result (*gamepad_enum)(nue_gamepad_info* infos, uint32_t capacity, uint32_t* out_count);
    nue_result (*gamepad_state)(nue_gamepad_id id, nue_gamepad_state* out);
    nue_result (*gamepad_rumble)(nue_gamepad_id id, uint16_t low, uint16_t high, uint32_t duration_ms);
    nue_result (*gamepad_rumble_triggers)(nue_gamepad_id id, uint16_t left, uint16_t right, uint32_t duration_ms);
    nue_result (*gamepad_led)(nue_gamepad_id id, uint8_t r, uint8_t g, uint8_t b);
    nue_result (*gamepad_sensor_enable)(nue_gamepad_id id, uint32_t kind, nue_bool enabled);
    nue_result (*gamepad_sensor_get)(nue_gamepad_id id, uint32_t kind, float out_values[3]);
    nue_result (*gamepad_add_mapping)(const char* line);
    nue_result (*gamepad_get_mapping)(nue_gamepad_id id, char* buffer, size_t capacity, size_t* out_size);
    nue_result (*gamepad_set_player_index)(nue_gamepad_id id, int32_t index);
    int16_t (*joystick_axis)(nue_gamepad_id id, uint32_t axis);
    nue_bool (*joystick_button)(nue_gamepad_id id, uint32_t button);
    uint8_t (*joystick_hat)(nue_gamepad_id id, uint32_t hat);
    /* Device sensors, haptics, touch presence. */
    nue_bool (*sensor_available)(nue_sensor_kind kind);
    nue_result (*sensor_enable)(nue_sensor_kind kind, nue_bool enabled, float rate_hz);
    nue_result (*sensor_get)(nue_sensor_kind kind, float out_values[3], int64_t* out_timestamp_ns);
    nue_bool (*haptics_available)(void);
    nue_result (*haptics_vibrate)(uint32_t duration_ms, float intensity);
    nue_bool (*touch_present)(void);
    nue_bool (*mouse_present)(void);
    nue_bool (*screen_keyboard)(void);
} nue_backend_input;

/* ------------------------------------------------------------------------------------------------ audio */

typedef struct nue_backend_device_open {
    uint32_t struct_size;
    nue_audio_device_id device;             /* 0 = default */
    nue_audio_spec spec;                    /* wanted device format; the backend may pick another and report it in `actual` */
    uint32_t buffer_frames;                 /* wanted callback size, 0 = backend default */
    nue_bool low_latency;
    nue_bool capture;
    /* [audio thread] Called by the device when it wants `frames` frames in the ACTUAL spec. Never blocks. */
    void (NUE_CALL *callback)(void* userdata, void* out, uint32_t frames);
    void* userdata;
    const char* stream_name;
} nue_backend_device_open;

typedef struct nue_backend_device_info {
    uint32_t struct_size;
    nue_audio_spec actual;
    uint32_t buffer_frames;
    uint64_t latency_ns;                    /* estimate */
} nue_backend_device_info;

typedef struct nue_backend_audio {
    nue_result (*list_devices)(nue_bool capture, nue_audio_device_info* infos, uint32_t capacity, uint32_t* out_count);
    nue_result (*open)(const nue_backend_device_open* open, void** out_device, nue_backend_device_info* out_info);   /* REQUIRED for audio */
    void (*close)(void* device);                                                                                      /* waits until the callback has returned */
    nue_result (*pause)(void* device, nue_bool paused);
    nue_result (*get_info)(void* device, nue_backend_device_info* out_info);       /* latency/buffer may change after reroutes */
    /* Frames the device has consumed since open and the nue_time_ns() of the sample (optional; the core estimates from callbacks otherwise). */
    nue_result (*get_clock)(void* device, uint64_t* out_frames_played, int64_t* out_sample_time_ns);
} nue_backend_audio;

/* ------------------------------------------------------------------------------------------------ the backend */

typedef struct nue_backend {
    uint32_t struct_size;
    const char* name;                       /* "null", "sdl3" */
    nue_result (*init)(const nue_backend_init* init);                    /* REQUIRED */
    void (*shutdown)(void);                                              /* REQUIRED */
    /* Collect OS events and push them with nue_internal_push_event. REQUIRED. Returns how many were pushed. [event thread] */
    int32_t (*pump)(void);
    /* Optional blocking wait for an OS event (timeout_ms < 0 forever); wake() interrupts it from another thread. */
    nue_result (*wait)(int32_t timeout_ms);
    void (*wake)(void);
    /* The backend's clock if it must replace the core's (null backend, deterministic mode). NULL = core uses the OS monotonic clock. */
    int64_t (*time_ns)(void);
    const nue_backend_video* video;         /* NULL: no video (headless without windows) */
    const nue_backend_input* input;
    const nue_backend_audio* audio;
    uint64_t capabilities[2];               /* bit i = nue_capability i that THE BACKEND provides; the platform layer may add more */
} nue_backend;

/* Entry points of the compiled-in backends. */
const nue_backend* nue_backend_null_get(void);
#if defined(NUE_HAS_SDL3)
const nue_backend* nue_backend_sdl3_get(void);
#endif

/* ------------------------------------------------------------------------------------------------ services the core offers to backends and platform layers */

void nue_internal_push_event(const nue_event* event);                    /* [any] stamps timestamp_ns if 0, updates input state tables, runs watchers, queues */
void nue_internal_window_changed(nue_window_id id, const nue_window_snapshot* snapshot);   /* update the cached snapshot without an event */
nue_window_id nue_internal_window_id_from_impl(void* impl);
void nue_internal_set_app_state(nue_app_state state);                    /* lifecycle: also emits PAUSING/PAUSED/RESUMING/RESUMED */
void nue_internal_register_extension(const char* name, const void* table);   /* see nue_get_extension */
void* nue_internal_alloc(size_t size);                                   /* same allocator as nue_alloc */
void nue_internal_free(void* ptr);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_INTERNAL_BACKEND_H */
