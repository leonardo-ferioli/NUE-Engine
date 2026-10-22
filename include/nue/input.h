/*
 * NUE engine - input: keyboard, mouse, gamepads/joysticks, touch, sensors, text input, "last used device".
 *
 * Two ways to read input, usable together: the event stream (app.h) and polled state (this header). Polled state is a
 * snapshot updated by nue_pump_events(); all polling functions are [any] and read the snapshot of the last pump.
 *
 * Scancodes are USB HID usage ids (layout independent, physical key positions), so mapping to DirectInput DIK_*, Win32 VK_*
 * or SDL scancodes is a table. Keycodes are layout dependent: the Unicode code point for printable keys, or
 * (scancode | NUE_KEYCODE_SCANCODE_MASK) for the others.
 */
#ifndef NUE_INPUT_H
#define NUE_INPUT_H

#include "nue/base.h"
#include "nue/window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== last used device */

typedef enum nue_input_device_kind {
    NUE_INPUT_DEVICE_NONE = 0,
    NUE_INPUT_DEVICE_KEYBOARD_MOUSE,
    NUE_INPUT_DEVICE_GAMEPAD,
    NUE_INPUT_DEVICE_TOUCH
} nue_input_device_kind;

/* Which glyph set a HUD should show for the device the player used last. */
typedef enum nue_glyph_family {
    NUE_GLYPHS_KEYBOARD_MOUSE = 0,
    NUE_GLYPHS_XBOX,
    NUE_GLYPHS_PLAYSTATION,
    NUE_GLYPHS_NINTENDO,
    NUE_GLYPHS_GENERIC_GAMEPAD,
    NUE_GLYPHS_TOUCH,
    NUE_GLYPHS_STEAM                    /* Steam Deck / Steam Controller / Steam Input layouts (Steam Input reports virtual Xbox pads: the platform layer overrides the family) */
} nue_glyph_family;

typedef struct nue_last_input {
    uint32_t struct_size;
    nue_input_device_kind kind;
    nue_glyph_family glyphs;
    uint32_t device_id;                     /* gamepad id when kind == GAMEPAD */
    int64_t time_ns;                        /* when the last meaningful input happened */
} nue_last_input;

/* The device that produced the last meaningful input (key press, button, stick beyond deadzone, touch); mouse motion of a few
 * pixels and stick drift do not count. A change raises NUE_EVENT_INPUT_DEVICE_CHANGED. */
NUE_API nue_result NUE_CALL nue_input_get_last_device(nue_last_input* out);
NUE_API nue_result NUE_CALL nue_input_set_last_device_deadzone(float stick_deadzone_0_1, float mouse_pixels);
/* Force the glyph family reported for gamepads (a platform layer or the host knows better than the device name: Steam Input, remapped pads).
 * family < 0 removes the override. Affects nue_last_input.glyphs, NUE_EVENT_INPUT_DEVICE_CHANGED and nue_gamepad_info.glyphs. [any] */
NUE_API nue_result NUE_CALL nue_input_set_glyph_override(int32_t family_or_minus_one);

/* ============================================================================================== keyboard */

typedef uint32_t nue_scancode;
#define NUE_KEYCODE_SCANCODE_MASK (1 << 30)
#define NUE_SCANCODE_COUNT 512
enum {
    NUE_SC_UNKNOWN = 0,
    NUE_SC_A = 4, NUE_SC_B, NUE_SC_C, NUE_SC_D, NUE_SC_E, NUE_SC_F, NUE_SC_G, NUE_SC_H, NUE_SC_I, NUE_SC_J, NUE_SC_K, NUE_SC_L, NUE_SC_M,
    NUE_SC_N, NUE_SC_O, NUE_SC_P, NUE_SC_Q, NUE_SC_R, NUE_SC_S, NUE_SC_T, NUE_SC_U, NUE_SC_V, NUE_SC_W, NUE_SC_X, NUE_SC_Y, NUE_SC_Z,
    NUE_SC_1 = 30, NUE_SC_2, NUE_SC_3, NUE_SC_4, NUE_SC_5, NUE_SC_6, NUE_SC_7, NUE_SC_8, NUE_SC_9, NUE_SC_0,
    NUE_SC_RETURN = 40, NUE_SC_ESCAPE, NUE_SC_BACKSPACE, NUE_SC_TAB, NUE_SC_SPACE, NUE_SC_MINUS, NUE_SC_EQUALS,
    NUE_SC_LEFTBRACKET, NUE_SC_RIGHTBRACKET, NUE_SC_BACKSLASH, NUE_SC_NONUSHASH, NUE_SC_SEMICOLON, NUE_SC_APOSTROPHE,
    NUE_SC_GRAVE, NUE_SC_COMMA, NUE_SC_PERIOD, NUE_SC_SLASH, NUE_SC_CAPSLOCK,
    NUE_SC_F1 = 58, NUE_SC_F2, NUE_SC_F3, NUE_SC_F4, NUE_SC_F5, NUE_SC_F6, NUE_SC_F7, NUE_SC_F8, NUE_SC_F9, NUE_SC_F10, NUE_SC_F11, NUE_SC_F12,
    NUE_SC_PRINTSCREEN = 70, NUE_SC_SCROLLLOCK, NUE_SC_PAUSE, NUE_SC_INSERT, NUE_SC_HOME, NUE_SC_PAGEUP, NUE_SC_DELETE, NUE_SC_END,
    NUE_SC_PAGEDOWN, NUE_SC_RIGHT, NUE_SC_LEFT, NUE_SC_DOWN, NUE_SC_UP,
    NUE_SC_NUMLOCK = 83, NUE_SC_KP_DIVIDE, NUE_SC_KP_MULTIPLY, NUE_SC_KP_MINUS, NUE_SC_KP_PLUS, NUE_SC_KP_ENTER,
    NUE_SC_KP_1 = 89, NUE_SC_KP_2, NUE_SC_KP_3, NUE_SC_KP_4, NUE_SC_KP_5, NUE_SC_KP_6, NUE_SC_KP_7, NUE_SC_KP_8, NUE_SC_KP_9, NUE_SC_KP_0,
    NUE_SC_KP_PERIOD = 99, NUE_SC_NONUSBACKSLASH = 100, NUE_SC_APPLICATION = 101, NUE_SC_KP_EQUALS = 103,
    NUE_SC_F13 = 104, NUE_SC_F14, NUE_SC_F15, NUE_SC_F16, NUE_SC_F17, NUE_SC_F18, NUE_SC_F19, NUE_SC_F20, NUE_SC_F21, NUE_SC_F22, NUE_SC_F23, NUE_SC_F24,
    NUE_SC_MUTE = 127, NUE_SC_VOLUMEUP = 128, NUE_SC_VOLUMEDOWN = 129,
    NUE_SC_LCTRL = 224, NUE_SC_LSHIFT, NUE_SC_LALT, NUE_SC_LGUI, NUE_SC_RCTRL, NUE_SC_RSHIFT, NUE_SC_RALT, NUE_SC_RGUI,
    NUE_SC_AC_BACK = 270                    /* Android back button / browser back */
};

typedef uint16_t nue_keymod;
enum {
    NUE_KMOD_LSHIFT = 0x0001, NUE_KMOD_RSHIFT = 0x0002, NUE_KMOD_LCTRL = 0x0040, NUE_KMOD_RCTRL = 0x0080,
    NUE_KMOD_LALT = 0x0100, NUE_KMOD_RALT = 0x0200, NUE_KMOD_LGUI = 0x0400, NUE_KMOD_RGUI = 0x0800,
    NUE_KMOD_NUM = 0x1000, NUE_KMOD_CAPS = 0x2000,
    NUE_KMOD_SHIFT = NUE_KMOD_LSHIFT | NUE_KMOD_RSHIFT, NUE_KMOD_CTRL = NUE_KMOD_LCTRL | NUE_KMOD_RCTRL,
    NUE_KMOD_ALT = NUE_KMOD_LALT | NUE_KMOD_RALT, NUE_KMOD_GUI = NUE_KMOD_LGUI | NUE_KMOD_RGUI
};

NUE_API nue_bool NUE_CALL nue_key_is_down(nue_scancode scancode);
NUE_API nue_keymod NUE_CALL nue_key_get_mods(void);
NUE_API int32_t NUE_CALL nue_key_from_scancode(nue_scancode scancode);          /* layout aware keycode */
NUE_API nue_scancode NUE_CALL nue_key_to_scancode(int32_t keycode);
NUE_API const char* NUE_CALL nue_scancode_name(nue_scancode scancode);          /* "Space", "Left Shift"; static, English */
/* Pointer to NUE_SCANCODE_COUNT bytes of 0/1 states (no copy). Valid until shutdown; updated by pumps. */
NUE_API const uint8_t* NUE_CALL nue_key_get_state_array(void);

/* ============================================================================================== mouse */

typedef enum nue_mouse_button { NUE_MOUSE_LEFT = 1, NUE_MOUSE_MIDDLE, NUE_MOUSE_RIGHT, NUE_MOUSE_X1, NUE_MOUSE_X2 } nue_mouse_button;
#define NUE_MOUSE_BUTTON_MASK(b) (1u << ((b) - 1))

/* Position in the focused window's logical coordinates, and the button mask (NUE_MOUSE_BUTTON_MASK). */
NUE_API uint32_t NUE_CALL nue_mouse_get_state(float* x, float* y);
NUE_API uint32_t NUE_CALL nue_mouse_get_relative_state(float* dx, float* dy);   /* motion since the last call (consumes) */
NUE_API nue_window NUE_CALL nue_mouse_get_focus(void);
NUE_API nue_bool NUE_CALL nue_mouse_is_present(void);
/* Relative mode and clip: window.h. */

/* ============================================================================================== gamepads */

typedef uint32_t nue_gamepad_id;        /* instance id, never reused within a run; 0 = invalid */

typedef enum nue_gamepad_button {
    NUE_PAD_SOUTH = 0,                  /* A / Cross */
    NUE_PAD_EAST,                       /* B / Circle */
    NUE_PAD_WEST,                       /* X / Square */
    NUE_PAD_NORTH,                      /* Y / Triangle */
    NUE_PAD_BACK, NUE_PAD_GUIDE, NUE_PAD_START,
    NUE_PAD_LEFT_STICK, NUE_PAD_RIGHT_STICK,
    NUE_PAD_LEFT_SHOULDER, NUE_PAD_RIGHT_SHOULDER,
    NUE_PAD_DPAD_UP, NUE_PAD_DPAD_DOWN, NUE_PAD_DPAD_LEFT, NUE_PAD_DPAD_RIGHT,
    NUE_PAD_MISC1,                      /* share / mic / capture */
    NUE_PAD_PADDLE1, NUE_PAD_PADDLE2, NUE_PAD_PADDLE3, NUE_PAD_PADDLE4,
    NUE_PAD_TOUCHPAD,
    NUE_PAD_BUTTON_COUNT
} nue_gamepad_button;

typedef enum nue_gamepad_axis {
    NUE_PAD_AXIS_LEFTX = 0, NUE_PAD_AXIS_LEFTY, NUE_PAD_AXIS_RIGHTX, NUE_PAD_AXIS_RIGHTY,
    NUE_PAD_AXIS_LEFT_TRIGGER, NUE_PAD_AXIS_RIGHT_TRIGGER,
    NUE_PAD_AXIS_COUNT
} nue_gamepad_axis;

typedef enum nue_gamepad_type {
    NUE_PAD_TYPE_UNKNOWN = 0, NUE_PAD_TYPE_STANDARD, NUE_PAD_TYPE_XBOX360, NUE_PAD_TYPE_XBOXONE, NUE_PAD_TYPE_PS3, NUE_PAD_TYPE_PS4,
    NUE_PAD_TYPE_PS5, NUE_PAD_TYPE_SWITCH_PRO, NUE_PAD_TYPE_SWITCH_JOYCON_LEFT, NUE_PAD_TYPE_SWITCH_JOYCON_RIGHT, NUE_PAD_TYPE_SWITCH_JOYCON_PAIR
} nue_gamepad_type;

typedef enum nue_power_state_gamepad { NUE_PAD_POWER_UNKNOWN = 0, NUE_PAD_POWER_WIRED, NUE_PAD_POWER_ON_BATTERY, NUE_PAD_POWER_CHARGING, NUE_PAD_POWER_FULL } nue_pad_power_state;

typedef uint32_t nue_gamepad_caps;
enum {
    NUE_PADCAP_RUMBLE = 1u << 0, NUE_PADCAP_TRIGGER_RUMBLE = 1u << 1, NUE_PADCAP_LED = 1u << 2, NUE_PADCAP_TOUCHPAD = 1u << 3,
    NUE_PADCAP_GYRO = 1u << 4, NUE_PADCAP_ACCEL = 1u << 5, NUE_PADCAP_HAS_MAPPING = 1u << 6   /* recognised as a "standard" gamepad; else only the raw joystick API works */
};

typedef struct nue_gamepad_info {
    uint32_t struct_size;
    nue_gamepad_id id;
    char name[128];
    nue_guid guid;
    uint16_t vendor_id, product_id, product_version;
    nue_gamepad_type type;
    nue_glyph_family glyphs;
    nue_gamepad_caps caps;
    int32_t player_index;               /* -1 unassigned */
    nue_pad_power_state power_state;
    int32_t battery_percent;            /* -1 unknown */
    uint32_t joystick_axes, joystick_buttons, joystick_hats;   /* raw layout */
    nue_bool connected;
    uint32_t reserved[4];
} nue_gamepad_info;

typedef struct nue_gamepad_state {
    uint32_t struct_size;
    uint32_t buttons;                   /* bit i = nue_gamepad_button i, i < 32 */
    int16_t axes[NUE_PAD_AXIS_COUNT];   /* sticks -32768..32767 (up/left negative), triggers 0..32767; raw, no deadzone applied */
    int64_t timestamp_ns;
} nue_gamepad_state;

/* [any] */
NUE_API nue_result NUE_CALL nue_gamepad_list(nue_gamepad_id* ids, uint32_t capacity, uint32_t* out_count);
NUE_API nue_result NUE_CALL nue_gamepad_get_info(nue_gamepad_id id, nue_gamepad_info* out);
NUE_API nue_result NUE_CALL nue_gamepad_get_state(nue_gamepad_id id, nue_gamepad_state* out);
/* Deadzone helper: radial deadzone on a stick pair, result in [-1,1] with the live range rescaled. */
NUE_API void NUE_CALL nue_gamepad_apply_deadzone(int16_t x, int16_t y, float deadzone_0_1, float* out_x, float* out_y);
NUE_API const char* NUE_CALL nue_gamepad_button_name(nue_gamepad_button button, nue_glyph_family family);   /* "A", "Cross", "B"... for HUD text; static */

/* Mapping database in SDL's gamecontrollerdb line format. Adding a mapping may emit NUE_EVENT_GAMEPAD_REMAPPED / ADDED. */
NUE_API nue_result NUE_CALL nue_gamepad_add_mapping(const char* mapping_line);
NUE_API nue_result NUE_CALL nue_gamepad_add_mappings_from_text(const char* text, size_t length, uint32_t* out_added);
NUE_API nue_result NUE_CALL nue_gamepad_add_mappings_from_file(const char* vfs_path, uint32_t* out_added);
NUE_API nue_result NUE_CALL nue_gamepad_get_mapping(nue_gamepad_id id, char* buffer, size_t capacity, size_t* out_size);
NUE_API nue_result NUE_CALL nue_gamepad_set_player_index(nue_gamepad_id id, int32_t player_index);

/* Feedback. Durations in ms (0 = stop); amplitudes 0..65535. Fire and forget; replaced by the next call. */
NUE_API nue_result NUE_CALL nue_gamepad_rumble(nue_gamepad_id id, uint16_t low_frequency, uint16_t high_frequency, uint32_t duration_ms);
NUE_API nue_result NUE_CALL nue_gamepad_rumble_triggers(nue_gamepad_id id, uint16_t left, uint16_t right, uint32_t duration_ms);
NUE_API nue_result NUE_CALL nue_gamepad_set_led(nue_gamepad_id id, uint8_t r, uint8_t g, uint8_t b);
/* Gamepad built-in sensors. */
NUE_API nue_result NUE_CALL nue_gamepad_set_sensor_enabled(nue_gamepad_id id, uint32_t sensor_kind /* nue_sensor_kind */, nue_bool enabled);
NUE_API nue_result NUE_CALL nue_gamepad_get_sensor(nue_gamepad_id id, uint32_t sensor_kind, float out_values[3]);

/* Raw joystick access for games that expect a DirectInput-style device (axes/buttons/hats, no standard layout). Valid for every
 * attached device, including those with a gamepad mapping. Axes -32768..32767; hats are bit masks 1=up 2=right 4=down 8=left. */
NUE_API int16_t NUE_CALL nue_joystick_get_axis(nue_gamepad_id id, uint32_t axis);
NUE_API nue_bool NUE_CALL nue_joystick_get_button(nue_gamepad_id id, uint32_t button);
NUE_API uint8_t NUE_CALL nue_joystick_get_hat(nue_gamepad_id id, uint32_t hat);

/* ============================================================================================== touch */

#define NUE_MAX_FINGERS 16

typedef struct nue_finger {
    uint64_t touch_id;                  /* device */
    uint64_t finger_id;                 /* stable for the life of the touch */
    float x, y;                         /* normalised 0..1 of the focused window */
    float pressure;
} nue_finger;

/* Fingers currently down, up to `capacity`; *out_count is the total. [any] */
NUE_API nue_result NUE_CALL nue_touch_get_fingers(nue_finger* fingers, uint32_t capacity, uint32_t* out_count);
NUE_API nue_bool NUE_CALL nue_touch_is_present(void);
/* When enabled (default off) touches also generate left-button mouse events, and mouse events carry from_touch for the reverse. */
NUE_API nue_result NUE_CALL nue_touch_set_mouse_emulation(nue_bool enabled);

/* ============================================================================================== sensors (device) */

typedef enum nue_sensor_kind {
    NUE_SENSOR_ACCELEROMETER = 1,       /* m/s^2 including gravity, device frame (x right, y up, z out of screen) */
    NUE_SENSOR_GYROSCOPE,               /* rad/s */
    NUE_SENSOR_GRAVITY,                 /* m/s^2, gravity only, when fused by the OS */
    NUE_SENSOR_ROTATION_VECTOR          /* unit quaternion x,y,z (w derivable) */
} nue_sensor_kind;

NUE_API nue_bool NUE_CALL nue_sensor_is_available(nue_sensor_kind kind);
NUE_API nue_result NUE_CALL nue_sensor_set_enabled(nue_sensor_kind kind, nue_bool enabled, float rate_hz /* 0 = default */);
NUE_API nue_result NUE_CALL nue_sensor_get(nue_sensor_kind kind, float out_values[3], int64_t* out_timestamp_ns);

/* ============================================================================================== text input / IME */

/* Starts delivering NUE_EVENT_TEXT_INPUT/TEXT_EDITING for the window and (on mobile) shows the soft keyboard. */
NUE_API nue_result NUE_CALL nue_text_input_start(nue_window window);
NUE_API nue_result NUE_CALL nue_text_input_stop(nue_window window);
NUE_API nue_bool NUE_CALL nue_text_input_is_active(nue_window window);
NUE_API nue_result NUE_CALL nue_text_input_set_area(nue_window window, const nue_rect_i* rect, int32_t cursor_offset);   /* where the IME popup may appear */
NUE_API nue_bool NUE_CALL nue_text_input_has_screen_keyboard(void);
/* Ask for the system on-screen keyboard explicitly (Steam Deck / gamescope, TV UIs, tablets with a keyboard attached) without waiting for focus on a text field.
 * NUE_ERR_UNSUPPORTED where the OS has none. Hiding it is always allowed. [event] */
NUE_API nue_result NUE_CALL nue_text_input_show_screen_keyboard(nue_window window, nue_bool show);

/* ============================================================================================== haptics (device) */

/* Phone/tablet vibration. Not for gamepads (see nue_gamepad_rumble). */
NUE_API nue_bool NUE_CALL nue_haptics_is_available(void);
NUE_API nue_result NUE_CALL nue_haptics_vibrate(uint32_t duration_ms, float intensity_0_1);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_INPUT_H */
