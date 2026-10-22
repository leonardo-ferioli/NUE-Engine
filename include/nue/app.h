/*
 * NUE engine - application lifecycle and the event queue.
 *
 * Two loop models (nue_init_desc.loop_model):
 *
 *  HOST loop   The caller owns the loop. It calls nue_pump_events() on the EVENT THREAD (the thread that called nue_init and
 *              created the windows; on Windows the thread that owns the window's message queue) and then drains the queue with
 *              nue_poll_event(). The recompiled game runs this way: its own loop calls nue_pump_events()/nue_poll_event() and
 *              presents through nue_gfx / the Direct3D 9 layer from its own thread.
 *  ENGINE loop The engine owns the loop (mobile, or desktop apps that prefer it): the caller hands callbacks to nue_app_run()
 *              from main(); the engine calls on_init once, then on_event for every event and on_frame once per iteration,
 *              until quit. On Android/iOS the same loop runs inside the OS entry point's thread (SDL_main); link nue::entry and define nue_main
 *              (see entry.h).
 *
 * Events are produced by the backend (SDL3, null) and by the platform layer (power, thermal, game mode). The queue is
 * a bounded multi-producer queue; when full the oldest MOUSE_MOTION/GAMEPAD_AXIS/SENSOR/FINGER_MOTION event is dropped first.
 * Lifecycle events that must be handled synchronously (PAUSING, LOW_MEMORY, TERMINATING) are additionally delivered to
 * watchers (nue_event_add_watcher) on the thread that received them, before they are queued.
 */
#ifndef NUE_APP_H
#define NUE_APP_H

#include "nue/base.h"
#include "nue/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nue_event_type {
    NUE_EVENT_NONE = 0,

    /* application lifecycle */
    NUE_EVENT_QUIT = 0x100,             /* user/OS asked to quit (last window closed on request, SIGTERM, Alt+F4 on the last window) */
    NUE_EVENT_APP_TERMINATING,          /* OS is ending the process: save now (mobile) */
    NUE_EVENT_APP_LOW_MEMORY,           /* release caches */
    NUE_EVENT_APP_PAUSING,              /* about to go to background (watchers get it synchronously): stop audio/GPU work */
    NUE_EVENT_APP_PAUSED,               /* in background */
    NUE_EVENT_APP_RESUMING,             /* about to return to foreground */
    NUE_EVENT_APP_RESUMED,              /* in foreground and interactive */
    NUE_EVENT_LOCALE_CHANGED,
    NUE_EVENT_SYSTEM_THEME_CHANGED,

    /* displays */
    NUE_EVENT_DISPLAY_ADDED = 0x150,
    NUE_EVENT_DISPLAY_REMOVED,
    NUE_EVENT_DISPLAY_CHANGED,          /* mode, orientation, scale or refresh rate changed (payload.display) */

    /* windows (window_id identifies the window) */
    NUE_EVENT_WINDOW_SHOWN = 0x200,
    NUE_EVENT_WINDOW_HIDDEN,
    NUE_EVENT_WINDOW_MOVED,             /* payload.window.x/y */
    NUE_EVENT_WINDOW_RESIZED,           /* logical size: payload.window.x/y = w/h */
    NUE_EVENT_WINDOW_PIXEL_SIZE_CHANGED,/* backbuffer pixel size: payload.window.x/y = w/h, .scale = pixel density; the renderer must resize */
    NUE_EVENT_WINDOW_MINIMIZED,
    NUE_EVENT_WINDOW_MAXIMIZED,
    NUE_EVENT_WINDOW_RESTORED,
    NUE_EVENT_WINDOW_FOCUS_GAINED,
    NUE_EVENT_WINDOW_FOCUS_LOST,
    NUE_EVENT_WINDOW_MOUSE_ENTER,
    NUE_EVENT_WINDOW_MOUSE_LEAVE,
    NUE_EVENT_WINDOW_CLOSE_REQUESTED,   /* the window is NOT closed by the engine; the app decides */
    NUE_EVENT_WINDOW_DISPLAY_CHANGED,   /* moved to another display: payload.window.x = display id */
    NUE_EVENT_WINDOW_CONTENT_SCALE_CHANGED, /* DPI scale: payload.window.scale */
    NUE_EVENT_WINDOW_SAFE_AREA_CHANGED, /* notch/cutout/insets changed: query nue_window_get_safe_area */
    NUE_EVENT_WINDOW_ENTER_FULLSCREEN,
    NUE_EVENT_WINDOW_LEAVE_FULLSCREEN,
    NUE_EVENT_WINDOW_OCCLUDED,
    NUE_EVENT_SURFACE_CREATED,          /* the native surface exists (Android: surfaceCreated; elsewhere right after window creation) */
    NUE_EVENT_SURFACE_LOST,             /* the native surface was destroyed (Android background): destroy the swapchain, keep the device */

    /* keyboard / text */
    NUE_EVENT_KEY_DOWN = 0x300,
    NUE_EVENT_KEY_UP,
    NUE_EVENT_TEXT_INPUT,
    NUE_EVENT_TEXT_EDITING,             /* IME composition */

    /* mouse */
    NUE_EVENT_MOUSE_MOTION = 0x400,
    NUE_EVENT_MOUSE_BUTTON_DOWN,
    NUE_EVENT_MOUSE_BUTTON_UP,
    NUE_EVENT_MOUSE_WHEEL,

    /* gamepad / joystick */
    NUE_EVENT_GAMEPAD_ADDED = 0x500,
    NUE_EVENT_GAMEPAD_REMOVED,
    NUE_EVENT_GAMEPAD_BUTTON_DOWN,
    NUE_EVENT_GAMEPAD_BUTTON_UP,
    NUE_EVENT_GAMEPAD_AXIS,
    NUE_EVENT_GAMEPAD_BATTERY_CHANGED,
    NUE_EVENT_GAMEPAD_REMAPPED,

    /* touch */
    NUE_EVENT_FINGER_DOWN = 0x600,
    NUE_EVENT_FINGER_UP,
    NUE_EVENT_FINGER_MOTION,
    NUE_EVENT_FINGER_CANCELED,

    /* sensors */
    NUE_EVENT_SENSOR_UPDATE = 0x700,

    /* "last used device" for dynamic glyphs */
    NUE_EVENT_INPUT_DEVICE_CHANGED = 0x780,

    /* audio devices */
    NUE_EVENT_AUDIO_DEVICE_ADDED = 0x800,
    NUE_EVENT_AUDIO_DEVICE_REMOVED,
    NUE_EVENT_AUDIO_DEFAULT_CHANGED,    /* streams opened on the default device are rerouted by the engine; this is informational */

    /* power / thermal / game mode */
    NUE_EVENT_POWER_CHANGED = 0x900,    /* battery level, charging state or power source */
    NUE_EVENT_THERMAL_CHANGED,          /* payload.power.thermal */
    NUE_EVENT_LOW_POWER_MODE_CHANGED,
    NUE_EVENT_GAME_MODE_CHANGED,

    /* asynchronous operations */
    NUE_EVENT_IO_COMPLETE = 0xA00,      /* payload.user.data1 = request id */
    NUE_EVENT_IMPORT_PROGRESS,          /* payload.user.data1 = import id, data2 = bytes done */
    NUE_EVENT_IMPORT_FINISHED,          /* payload.user.data1 = import id, code = nue_result */

    /* application defined: NUE_EVENT_USER .. NUE_EVENT_USER + 0xFFFF */
    NUE_EVENT_USER = 0x8000
} nue_event_type;

typedef uint32_t nue_window_id;         /* 0 = none */

typedef struct nue_event_window    { int32_t x, y; float scale; int32_t reserved[3]; } nue_event_window;
typedef struct nue_event_display   { uint32_t display_id; int32_t data1, data2; } nue_event_display;
typedef struct nue_event_key       { uint32_t device_id; uint32_t scancode; int32_t keycode; uint16_t mods; uint8_t down; uint8_t repeat; } nue_event_key;
typedef struct nue_event_text      { char text[32]; uint8_t editing_start; uint8_t editing_length; } nue_event_text;   /* UTF-8, may be a chunk */
typedef struct nue_event_motion    { uint32_t device_id; float x, y; float dx, dy; uint32_t buttons; uint8_t from_touch; } nue_event_motion;
typedef struct nue_event_button    { uint32_t device_id; float x, y; uint8_t button; uint8_t down; uint8_t clicks; } nue_event_button;
typedef struct nue_event_wheel     { uint32_t device_id; float x, y; float mouse_x, mouse_y; uint8_t flipped; } nue_event_wheel;
typedef struct nue_event_gamepad   { uint32_t gamepad_id; int32_t control; int32_t value; uint32_t extra; } nue_event_gamepad;   /* control = nue_gamepad_button / nue_gamepad_axis; value: axis -32768..32767, button 0/1; extra: BATTERY: nue_power_state<<8 | percent */
typedef struct nue_event_finger    { uint64_t touch_id; uint64_t finger_id; float x, y; float dx, dy; float pressure; } nue_event_finger;   /* x,y normalised 0..1 in the window */
typedef struct nue_event_sensor    { uint32_t sensor_id; uint32_t kind; float values[3]; int64_t sensor_timestamp_ns; } nue_event_sensor;
typedef struct nue_event_input_dev { uint32_t device_kind; uint32_t glyph_family; uint32_t device_id; } nue_event_input_dev;   /* nue_input_device_kind / nue_glyph_family */
typedef struct nue_event_audio_dev { uint32_t device_id; uint8_t is_capture; } nue_event_audio_dev;
typedef struct nue_event_power     { uint32_t thermal; uint32_t power_state; int32_t percent; uint8_t low_power; uint8_t game_mode; } nue_event_power;
typedef struct nue_event_user      { int32_t code; uint64_t data1; uint64_t data2; uint64_t data3; } nue_event_user;

/* Fixed 64-byte event. The union never grows; new event kinds fit in 40 bytes or are fetched through a follow-up query. */
typedef struct nue_event {
    uint32_t type;                      /* nue_event_type */
    nue_window_id window_id;
    int64_t timestamp_ns;               /* nue_time_ns() domain */
    union {
        nue_event_window     window;
        nue_event_display    display;
        nue_event_key        key;
        nue_event_text       text;
        nue_event_motion     motion;
        nue_event_button     button;
        nue_event_wheel      wheel;
        nue_event_gamepad    gamepad;
        nue_event_finger     finger;
        nue_event_sensor     sensor;
        nue_event_input_dev  input_device;
        nue_event_audio_dev  audio_device;
        nue_event_power      power;
        nue_event_user       user;
        uint8_t              raw[48];
    } payload;
} nue_event;

/* ---------------------------------------------------------------------------- host loop ---- */

/* Collect pending OS events into the queue. [event] Non-blocking. Also runs delayed engine work (device hot-plug,
 * async completions, config autosave). Returns the number of events added. */
NUE_API int32_t NUE_CALL nue_pump_events(void);
/* Remove the oldest event; returns NUE_TRUE and fills *out, or NUE_FALSE when the queue is empty. Does NOT pump. [any] */
NUE_API nue_bool NUE_CALL nue_poll_event(nue_event* out);
/* Pump, then wait up to timeout_ms (< 0 forever) for an event. [event] */
NUE_API nue_bool NUE_CALL nue_wait_event(nue_event* out, int32_t timeout_ms);
/* Like poll but leaves the events queued: copies up to max_count of the oldest events. [any] */
NUE_API int32_t NUE_CALL nue_peek_events(nue_event* out, int32_t max_count);
NUE_API nue_result NUE_CALL nue_push_event(const nue_event* event);                    /* [any] application events, wake-ups */
NUE_API nue_result NUE_CALL nue_push_user_event(int32_t code, uint64_t data1, uint64_t data2);
NUE_API void NUE_CALL nue_flush_events(uint32_t first_type, uint32_t last_type);       /* [any] drop queued events in the range */
NUE_API void NUE_CALL nue_set_event_enabled(uint32_t type, nue_bool enabled);          /* disabled types are never queued */

/* A watcher sees every event synchronously on the producing thread, before queueing. Return NUE_TRUE to also queue it,
 * NUE_FALSE to swallow it. It must be fast, must not call nue_pump_events/nue_wait_event, and must not block. */
typedef nue_bool (NUE_CALL *nue_event_watcher)(void* userdata, const nue_event* event);
NUE_API nue_result NUE_CALL nue_event_add_watcher(nue_event_watcher fn, void* userdata);
NUE_API nue_result NUE_CALL nue_event_remove_watcher(nue_event_watcher fn, void* userdata);

/* Re-bind the event thread (the thread that must call nue_pump_events). Needed when the host creates its windows on a
 * thread other than the one that called nue_init. [any] */
NUE_API nue_result NUE_CALL nue_set_event_thread(void);
NUE_API nue_bool NUE_CALL nue_is_event_thread(void);

/* ---------------------------------------------------------------------------- engine loop ---- */

typedef enum nue_app_result {
    NUE_APP_CONTINUE = 0,
    NUE_APP_QUIT_OK,                    /* end the loop, exit code 0 */
    NUE_APP_QUIT_FAILURE                /* end the loop, exit code 1 */
} nue_app_result;

typedef struct nue_app_callbacks {
    uint32_t struct_size;
    /* Called once on the event thread after the engine is initialised. A negative result aborts startup. */
    nue_result (NUE_CALL *on_init)(void* userdata, int argc, char** argv);
    /* Called for every event on the event thread. */
    nue_app_result (NUE_CALL *on_event)(void* userdata, const nue_event* event);
    /* Called once per loop iteration on the event thread (desktop: as fast as events allow, or paced by on_frame's own waits;
     * mobile: once per display refresh). */
    nue_app_result (NUE_CALL *on_frame)(void* userdata);
    /* Called once at the end with the final result, before the engine shuts down. */
    void (NUE_CALL *on_quit)(void* userdata, nue_result result);
    void* userdata;
} nue_app_callbacks;

/* Run the engine-owned loop: calls nue_init(init) itself (init->loop_model is forced to NUE_LOOP_ENGINE), runs on_init, loops,
 * runs on_quit and nue_shutdown(). Returns the process exit code. The loop keeps running while the app is in the background
 * (events keep flowing; on_frame is not called until it returns to the foreground). [call from main / nue_main] */
NUE_API int32_t NUE_CALL nue_app_run(const nue_init_desc* init, const nue_app_callbacks* callbacks, int argc, char** argv);

NUE_API void NUE_CALL nue_app_request_quit(void);                  /* [any] queues NUE_EVENT_QUIT; ends nue_app_run's loop */
NUE_API nue_bool NUE_CALL nue_app_quit_requested(void);

typedef enum nue_app_state {
    NUE_APP_STATE_FOREGROUND = 0,       /* visible and interactive */
    NUE_APP_STATE_INACTIVE,             /* visible, no input focus (iOS control center, desktop focus lost) */
    NUE_APP_STATE_BACKGROUND,           /* not visible; GPU and audio must be quiet */
    NUE_APP_STATE_TERMINATING
} nue_app_state;
NUE_API nue_app_state NUE_CALL nue_app_get_state(void);            /* [any] */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_APP_H */
