/*
 * NUE engine - displays and windows.
 *
 * Coordinates: window position and logical size are in "screen coordinates" (points on macOS/iOS/Android-dp-like units,
 * pixels on Windows/Linux at 100% scale). The backbuffer size is the PIXEL size (nue_window_get_pixel_size); renderers
 * must use it. pixel density = pixel size / logical size; content scale = the OS UI scale (125% -> 1.25).
 *
 * A window is either created by the engine (nue_window_create) or ATTACHED to a native window the host already owns
 * (desc.attach): the recompiled game creates its own HWND; the engine then only observes it (events, size, display queries)
 * and lends the handle to the renderer. Attached windows are never destroyed by the engine, and the host's window procedure
 * keeps priority (the engine hooks, never replaces). Thread: [event] for everything that changes a window; the pure queries
 * marked [any] may be called from any thread (the values are a cached snapshot updated when events are pumped).
 */
#ifndef NUE_WINDOW_H
#define NUE_WINDOW_H

#include "nue/base.h"
#include "nue/app.h"

#ifdef __cplusplus
extern "C" {
#endif

NUE_DEFINE_HANDLE(nue_window);

/* ============================================================================================== displays */

typedef uint32_t nue_display_id;            /* stable while the display is connected; 0 = invalid */

typedef enum nue_orientation {
    NUE_ORIENTATION_UNKNOWN = 0,
    NUE_ORIENTATION_LANDSCAPE, NUE_ORIENTATION_LANDSCAPE_FLIPPED,
    NUE_ORIENTATION_PORTRAIT,  NUE_ORIENTATION_PORTRAIT_FLIPPED
} nue_orientation;

typedef struct nue_display_mode {
    uint32_t struct_size;
    int32_t width, height;                  /* pixels */
    float refresh_rate_hz;                  /* 0 = unknown; fractional rates are exact (59.94) */
    float pixel_density;                    /* 1.0 = one pixel per logical unit */
    uint32_t format;                        /* backend specific pixel format id, informational */
} nue_display_mode;

typedef struct nue_display_info {
    uint32_t struct_size;
    nue_display_id id;
    char name[128];                         /* UTF-8 */
    nue_rect_i bounds;                      /* desktop coordinates */
    nue_rect_i usable_bounds;               /* without taskbar/dock/menu bar */
    nue_display_mode current_mode;
    nue_display_mode desktop_mode;
    float content_scale;                    /* OS UI scale */
    nue_orientation orientation;
    nue_orientation natural_orientation;
    nue_bool is_primary;
    nue_bool hdr_enabled;
    uint32_t reserved[8];
} nue_display_info;

/* [any] Enumerate connected displays. Fills up to `capacity` ids; *out_count gets the total number. */
NUE_API nue_result NUE_CALL nue_display_list(nue_display_id* ids, uint32_t capacity, uint32_t* out_count);
NUE_API nue_display_id NUE_CALL nue_display_primary(void);
NUE_API nue_result NUE_CALL nue_display_get_info(nue_display_id id, nue_display_info* out);
/* All fullscreen modes of a display, best first (sorted by size then refresh, descending). */
NUE_API nue_result NUE_CALL nue_display_list_modes(nue_display_id id, nue_display_mode* modes, uint32_t capacity, uint32_t* out_count);
/* Closest mode to the request (0 refresh = any). NUE_ERR_NOT_FOUND if the display has no modes. */
NUE_API nue_result NUE_CALL nue_display_find_mode(nue_display_id id, int32_t width, int32_t height, float refresh_rate_hz, nue_display_mode* out);
NUE_API nue_display_id NUE_CALL nue_display_from_point(nue_point_i desktop_point);

/* ============================================================================================== windows */

typedef enum nue_window_mode {
    NUE_WINDOW_MODE_WINDOWED = 0,
    NUE_WINDOW_MODE_BORDERLESS,             /* borderless window covering the display at the desktop mode ("fullscreen windowed") */
    NUE_WINDOW_MODE_EXCLUSIVE               /* real mode change to `fullscreen_mode`; where the OS has no exclusive fullscreen this is BORDERLESS */
} nue_window_mode;

typedef uint32_t nue_window_flags;
enum {
    NUE_WINDOW_RESIZABLE      = 1u << 0,
    NUE_WINDOW_HIDDEN         = 1u << 1,    /* create hidden (also the default for headless) */
    NUE_WINDOW_BORDERLESS     = 1u << 2,    /* no decoration while WINDOWED */
    NUE_WINDOW_ALWAYS_ON_TOP  = 1u << 3,
    NUE_WINDOW_HIGH_DPI       = 1u << 4,    /* backbuffer at native pixel density; without it the pixel size equals the logical size (scaled by the OS) */
    NUE_WINDOW_MAXIMIZED      = 1u << 5,
    NUE_WINDOW_MINIMIZED      = 1u << 6,
    NUE_WINDOW_NOT_FOCUSABLE  = 1u << 7,
    NUE_WINDOW_UTILITY        = 1u << 8,    /* tool window: no taskbar entry */
    NUE_WINDOW_GFX_VULKAN     = 1u << 16,   /* create the native surface suitable for the API (Vulkan surface support, Metal layer, GL/GLES context-ready) */
    NUE_WINDOW_GFX_METAL      = 1u << 17,
    NUE_WINDOW_GFX_OPENGL     = 1u << 18,   /* bgfx creates its own context on the native handle; the flag only picks the pixel format/visual */
    NUE_WINDOW_GFX_NONE       = 0           /* default: Direct3D, or the renderer attaches later */
};

typedef struct nue_window_desc {
    uint32_t struct_size;
    const char* title;
    int32_t x, y;                           /* NUE_WINDOW_POS_UNDEFINED / NUE_WINDOW_POS_CENTERED or a desktop coordinate */
    int32_t width, height;                  /* logical size; 0 = 1280x720 windowed, display size for fullscreen modes */
    nue_window_flags flags;
    nue_window_mode mode;
    nue_display_id display;                 /* 0 = primary (or the display under x,y) */
    nue_display_mode fullscreen_mode;       /* EXCLUSIVE: requested mode (zeros = desktop mode) */
    const nue_native_window* attach;        /* non-NULL: wrap this native window instead of creating one (flags/size/mode ignored except mode changes the host allows) */
    uint32_t min_width, min_height, max_width, max_height;   /* 0 = unconstrained */
    uint32_t reserved[8];
} nue_window_desc;

#define NUE_WINDOW_POS_UNDEFINED ((int32_t)0x1FFF0000)
#define NUE_WINDOW_POS_CENTERED  ((int32_t)0x2FFF0000)

/* [event] */
NUE_API nue_result NUE_CALL nue_window_create(const nue_window_desc* desc, nue_window* out_window);
NUE_API void NUE_CALL nue_window_destroy(nue_window window);             /* attached windows are only detached */
NUE_API nue_window_id NUE_CALL nue_window_get_id(nue_window window);      /* [any] */
NUE_API nue_window NUE_CALL nue_window_from_id(nue_window_id id);         /* [any] NULL if gone */
NUE_API nue_window NUE_CALL nue_window_get_focused(void);                 /* [any] */

NUE_API nue_result NUE_CALL nue_window_set_title(nue_window window, const char* title);
NUE_API nue_result NUE_CALL nue_window_get_title(nue_window window, char* buffer, size_t capacity, size_t* out_size);
NUE_API nue_result NUE_CALL nue_window_set_size(nue_window window, int32_t width, int32_t height);
NUE_API nue_result NUE_CALL nue_window_set_position(nue_window window, int32_t x, int32_t y);
NUE_API nue_result NUE_CALL nue_window_set_size_limits(nue_window window, uint32_t min_w, uint32_t min_h, uint32_t max_w, uint32_t max_h);
NUE_API nue_result NUE_CALL nue_window_set_resizable(nue_window window, nue_bool resizable);
NUE_API nue_result NUE_CALL nue_window_set_bordered(nue_window window, nue_bool bordered);
NUE_API nue_result NUE_CALL nue_window_set_always_on_top(nue_window window, nue_bool on_top);
NUE_API nue_result NUE_CALL nue_window_show(nue_window window);
NUE_API nue_result NUE_CALL nue_window_hide(nue_window window);
NUE_API nue_result NUE_CALL nue_window_raise(nue_window window);          /* bring to front and request focus */
NUE_API nue_result NUE_CALL nue_window_minimize(nue_window window);
NUE_API nue_result NUE_CALL nue_window_maximize(nue_window window);
NUE_API nue_result NUE_CALL nue_window_restore(nue_window window);
NUE_API nue_result NUE_CALL nue_window_flash(nue_window window, nue_bool until_focused);   /* attention request */
/* Switch mode; for EXCLUSIVE give the wanted display mode (NULL = desktop mode). Completion is reported by
 * NUE_EVENT_WINDOW_ENTER/LEAVE_FULLSCREEN and a PIXEL_SIZE_CHANGED event; the result is NUE_OK_PENDING if it may be asynchronous. */
NUE_API nue_result NUE_CALL nue_window_set_mode(nue_window window, nue_window_mode mode, const nue_display_mode* exclusive_mode);
NUE_API nue_result NUE_CALL nue_window_move_to_display(nue_window window, nue_display_id display);

/* Snapshot queries [any]. */
NUE_API nue_window_mode NUE_CALL nue_window_get_mode(nue_window window);
NUE_API nue_window_flags NUE_CALL nue_window_get_flags(nue_window window);
NUE_API nue_result NUE_CALL nue_window_get_size(nue_window window, int32_t* width, int32_t* height);          /* logical */
NUE_API nue_result NUE_CALL nue_window_get_pixel_size(nue_window window, int32_t* width, int32_t* height);    /* backbuffer pixels */
NUE_API nue_result NUE_CALL nue_window_get_position(nue_window window, int32_t* x, int32_t* y);
NUE_API float NUE_CALL nue_window_get_content_scale(nue_window window);        /* OS UI scale (DPI / 96 on Windows) */
NUE_API float NUE_CALL nue_window_get_pixel_density(nue_window window);
NUE_API nue_display_id NUE_CALL nue_window_get_display(nue_window window);
NUE_API float NUE_CALL nue_window_get_refresh_rate(nue_window window);         /* Hz of the display it is on (current mode), 0 = unknown */
NUE_API nue_bool NUE_CALL nue_window_has_focus(nue_window window);
NUE_API nue_bool NUE_CALL nue_window_is_visible(nue_window window);
/* Insets, in logical units, that must stay free of essential UI (notch, rounded corners, gesture bars, taskbar). Zero on desktop windowed. */
NUE_API nue_result NUE_CALL nue_window_get_safe_area(nue_window window, nue_rect_i* out_rect);

/* Native handle(s) for renderers and interop. [any] The handle is valid until the window is destroyed (Android: until SURFACE_LOST). */
NUE_API nue_result NUE_CALL nue_window_get_native(nue_window window, nue_native_window* out);

/* ============================================================================================== cursor */

typedef enum nue_cursor_kind {
    NUE_CURSOR_DEFAULT = 0, NUE_CURSOR_TEXT, NUE_CURSOR_WAIT, NUE_CURSOR_CROSSHAIR, NUE_CURSOR_HAND, NUE_CURSOR_MOVE,
    NUE_CURSOR_RESIZE_NS, NUE_CURSOR_RESIZE_EW, NUE_CURSOR_RESIZE_NESW, NUE_CURSOR_RESIZE_NWSE, NUE_CURSOR_NOT_ALLOWED
} nue_cursor_kind;

NUE_API nue_result NUE_CALL nue_cursor_set_visible(nue_bool visible);                   /* [event] global; counts are NOT used */
NUE_API nue_bool NUE_CALL nue_cursor_is_visible(void);
NUE_API nue_result NUE_CALL nue_cursor_set_kind(nue_cursor_kind kind);
/* Keep the pointer inside the window (or inside `rect` in window logical coordinates when non-NULL). Released automatically when the
 * window loses focus and re-applied on focus gain; ALWAYS released on destroy/shutdown/crash. */
NUE_API nue_result NUE_CALL nue_window_set_cursor_clip(nue_window window, const nue_rect_i* rect_or_null, nue_bool enabled);
/* Relative mode: pointer hidden and captured, MOUSE_MOTION carries only dx/dy. */
NUE_API nue_result NUE_CALL nue_window_set_relative_mouse(nue_window window, nue_bool enabled);
NUE_API nue_bool NUE_CALL nue_window_get_relative_mouse(nue_window window);
NUE_API nue_result NUE_CALL nue_window_warp_mouse(nue_window window, float x, float y);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_WINDOW_H */
