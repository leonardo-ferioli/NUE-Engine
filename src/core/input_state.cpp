// Polled input state (keyboard, mouse, touch) maintained from the event stream, "last used device" tracking, scancode and gamepad helpers.
#include "internal/core_internal.h"

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace {

struct InputState {
    std::mutex mutex;
    uint8_t keys[NUE_SCANCODE_COUNT];
    nue_keymod mods = 0;
    float mouse_x = 0, mouse_y = 0;
    float rel_dx = 0, rel_dy = 0;
    uint32_t buttons = 0;
    nue_window_id mouse_focus = 0;
    nue_finger fingers[NUE_MAX_FINGERS];
    uint32_t finger_count = 0;
    bool mouse_emulation = false;

    nue_last_input last{};
    float stick_deadzone = 0.35f;     // fraction of full deflection that counts as "intentional"
    float mouse_pixels = 12.0f;       // accumulated motion that counts as "intentional"
    float mouse_accum = 0;
    std::map<uint32_t, nue_glyph_family> pad_family;
};

InputState& st() {
    static InputState* s = new InputState();
    return *s;
}

void set_last(InputState& s, nue_input_device_kind kind, nue_glyph_family glyphs, uint32_t device, int64_t t, std::vector<nue_event>& derived) {
    bool changed = s.last.kind != kind || s.last.glyphs != glyphs || (kind == NUE_INPUT_DEVICE_GAMEPAD && s.last.device_id != device);
    s.last.kind = kind;
    s.last.glyphs = glyphs;
    s.last.device_id = device;
    s.last.time_ns = t;
    if (!changed) return;
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.type = NUE_EVENT_INPUT_DEVICE_CHANGED;
    e.timestamp_ns = t;
    e.payload.input_device.device_kind = kind;
    e.payload.input_device.glyph_family = glyphs;
    e.payload.input_device.device_id = device;
    derived.push_back(e);
}

nue_glyph_family family_of_pad_locked(InputState& s, uint32_t id) {
    auto it = s.pad_family.find(id);
    if (it != s.pad_family.end()) return it->second;
    return NUE_GLYPHS_GENERIC_GAMEPAD;
}

}  // namespace

namespace nuei {

void input_state_reset() {
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::memset(s.keys, 0, sizeof(s.keys));
    s.mods = 0;
    s.mouse_x = s.mouse_y = s.rel_dx = s.rel_dy = 0;
    s.buttons = 0;
    s.mouse_focus = 0;
    s.finger_count = 0;
    s.mouse_emulation = false;
    std::memset(&s.last, 0, sizeof(s.last));
    s.last.struct_size = sizeof(s.last);
    s.last.glyphs = NUE_GLYPHS_KEYBOARD_MOUSE;
    s.mouse_accum = 0;
    s.pad_family.clear();
}

void input_on_event(const nue_event& e, std::vector<nue_event>& derived) {
    InputState& s = st();
    // Gamepad family lookup talks to the backend: do it before taking our lock.
    nue_glyph_family fam_for_added = NUE_GLYPHS_GENERIC_GAMEPAD;
    bool have_added = false;
    if (e.type == NUE_EVENT_GAMEPAD_ADDED) {
        const nue_backend* b = backend();
        if (b && b->input && b->input->gamepad_enum) {
            nue_gamepad_info infos[16];
            for (auto& i : infos) NUE_STRUCT_INIT(&i);
            uint32_t count = 0;
            if (NUE_SUCCEEDED(b->input->gamepad_enum(infos, 16, &count)))
                for (uint32_t i = 0; i < count && i < 16; ++i)
                    if (infos[i].id == e.payload.gamepad.gamepad_id) { fam_for_added = infos[i].glyphs; have_added = true; }
        }
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    const int64_t t = e.timestamp_ns;
    switch (e.type) {
        case NUE_EVENT_KEY_DOWN:
        case NUE_EVENT_KEY_UP:
            if (e.payload.key.scancode < NUE_SCANCODE_COUNT) s.keys[e.payload.key.scancode] = e.type == NUE_EVENT_KEY_DOWN ? 1 : 0;
            s.mods = e.payload.key.mods;
            if (e.type == NUE_EVENT_KEY_DOWN) set_last(s, NUE_INPUT_DEVICE_KEYBOARD_MOUSE, NUE_GLYPHS_KEYBOARD_MOUSE, 0, t, derived);
            break;
        case NUE_EVENT_MOUSE_MOTION:
            s.mouse_x = e.payload.motion.x;
            s.mouse_y = e.payload.motion.y;
            s.rel_dx += e.payload.motion.dx;
            s.rel_dy += e.payload.motion.dy;
            s.buttons = e.payload.motion.buttons;
            if (!e.payload.motion.from_touch) {
                s.mouse_accum += std::fabs(e.payload.motion.dx) + std::fabs(e.payload.motion.dy);
                if (s.mouse_accum >= s.mouse_pixels) set_last(s, NUE_INPUT_DEVICE_KEYBOARD_MOUSE, NUE_GLYPHS_KEYBOARD_MOUSE, 0, t, derived);
            }
            break;
        case NUE_EVENT_MOUSE_BUTTON_DOWN:
        case NUE_EVENT_MOUSE_BUTTON_UP:
            if (e.payload.button.button >= 1 && e.payload.button.button <= 32) {
                uint32_t bit = 1u << (e.payload.button.button - 1);
                if (e.type == NUE_EVENT_MOUSE_BUTTON_DOWN) s.buttons |= bit; else s.buttons &= ~bit;
            }
            s.mouse_x = e.payload.button.x;
            s.mouse_y = e.payload.button.y;
            if (e.type == NUE_EVENT_MOUSE_BUTTON_DOWN) set_last(s, NUE_INPUT_DEVICE_KEYBOARD_MOUSE, NUE_GLYPHS_KEYBOARD_MOUSE, 0, t, derived);
            break;
        case NUE_EVENT_MOUSE_WHEEL:
            set_last(s, NUE_INPUT_DEVICE_KEYBOARD_MOUSE, NUE_GLYPHS_KEYBOARD_MOUSE, 0, t, derived);
            break;
        case NUE_EVENT_WINDOW_MOUSE_ENTER: s.mouse_focus = e.window_id; break;
        case NUE_EVENT_WINDOW_MOUSE_LEAVE: if (s.mouse_focus == e.window_id) s.mouse_focus = 0; break;
        case NUE_EVENT_WINDOW_FOCUS_LOST:
            // keys held while focus leaves would otherwise stay "down" forever
            std::memset(s.keys, 0, sizeof(s.keys));
            s.buttons = 0;
            s.mods = 0;
            break;
        case NUE_EVENT_GAMEPAD_ADDED:
            s.pad_family[e.payload.gamepad.gamepad_id] = have_added ? fam_for_added : NUE_GLYPHS_GENERIC_GAMEPAD;
            break;
        case NUE_EVENT_GAMEPAD_REMOVED:
            s.pad_family.erase(e.payload.gamepad.gamepad_id);
            if (s.last.kind == NUE_INPUT_DEVICE_GAMEPAD && s.last.device_id == e.payload.gamepad.gamepad_id)
                set_last(s, NUE_INPUT_DEVICE_KEYBOARD_MOUSE, NUE_GLYPHS_KEYBOARD_MOUSE, 0, t, derived);   // fall back so HUD glyphs never point at a missing pad
            break;
        case NUE_EVENT_GAMEPAD_BUTTON_DOWN:
            set_last(s, NUE_INPUT_DEVICE_GAMEPAD, family_of_pad_locked(s, e.payload.gamepad.gamepad_id), e.payload.gamepad.gamepad_id, t, derived);
            break;
        case NUE_EVENT_GAMEPAD_AXIS: {
            float v = std::fabs(float(e.payload.gamepad.value)) / 32767.0f;
            if (v >= s.stick_deadzone)
                set_last(s, NUE_INPUT_DEVICE_GAMEPAD, family_of_pad_locked(s, e.payload.gamepad.gamepad_id), e.payload.gamepad.gamepad_id, t, derived);
            break;
        }
        case NUE_EVENT_FINGER_DOWN:
        case NUE_EVENT_FINGER_MOTION: {
            nue_finger* f = nullptr;
            for (uint32_t i = 0; i < s.finger_count; ++i) if (s.fingers[i].finger_id == e.payload.finger.finger_id) f = &s.fingers[i];
            if (!f && s.finger_count < NUE_MAX_FINGERS) f = &s.fingers[s.finger_count++];
            if (f) {
                f->touch_id = e.payload.finger.touch_id;
                f->finger_id = e.payload.finger.finger_id;
                f->x = e.payload.finger.x;
                f->y = e.payload.finger.y;
                f->pressure = e.payload.finger.pressure;
            }
            if (e.type == NUE_EVENT_FINGER_DOWN) set_last(s, NUE_INPUT_DEVICE_TOUCH, NUE_GLYPHS_TOUCH, 0, t, derived);
            break;
        }
        case NUE_EVENT_FINGER_UP:
        case NUE_EVENT_FINGER_CANCELED:
            for (uint32_t i = 0; i < s.finger_count; ++i)
                if (s.fingers[i].finger_id == e.payload.finger.finger_id) { s.fingers[i] = s.fingers[--s.finger_count]; break; }
            break;
        default: break;
    }
    if (e.type == NUE_EVENT_GAMEPAD_BUTTON_DOWN || e.type == NUE_EVENT_KEY_DOWN || e.type == NUE_EVENT_MOUSE_BUTTON_DOWN) s.mouse_accum = 0;
}

}  // namespace nuei

extern "C" {

NUE_API nue_result NUE_CALL nue_input_get_last_device(nue_last_input* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 3) return NUE_ERR_INVALID_ARGUMENT;
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    nue_last_input v = s.last;
    uint32_t keep = out->struct_size;
    std::memcpy(out, &v, keep < sizeof(v) ? keep : sizeof(v));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_input_set_last_device_deadzone(float stick, float mouse_pixels) {
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (stick >= 0 && stick <= 1) s.stick_deadzone = stick;
    if (mouse_pixels >= 0) s.mouse_pixels = mouse_pixels;
    return NUE_OK;
}

NUE_API nue_bool NUE_CALL nue_key_is_down(nue_scancode sc) {
    if (sc >= NUE_SCANCODE_COUNT) return NUE_FALSE;
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.keys[sc] ? NUE_TRUE : NUE_FALSE;
}

NUE_API nue_keymod NUE_CALL nue_key_get_mods(void) { InputState& s = st(); std::lock_guard<std::mutex> lock(s.mutex); return s.mods; }

NUE_API const uint8_t* NUE_CALL nue_key_get_state_array(void) { return st().keys; }

NUE_API int32_t NUE_CALL nue_key_from_scancode(nue_scancode sc) {
    // US layout fallback; backends with real layout support may be consulted here in the future.
    if (sc >= NUE_SC_A && sc <= NUE_SC_Z) return 'a' + int32_t(sc - NUE_SC_A);
    if (sc >= NUE_SC_1 && sc <= NUE_SC_9) return '1' + int32_t(sc - NUE_SC_1);
    switch (sc) {
        case NUE_SC_0: return '0';
        case NUE_SC_RETURN: return '\r';
        case NUE_SC_ESCAPE: return 27;
        case NUE_SC_BACKSPACE: return 8;
        case NUE_SC_TAB: return 9;
        case NUE_SC_SPACE: return ' ';
        case NUE_SC_MINUS: return '-';
        case NUE_SC_EQUALS: return '=';
        case NUE_SC_LEFTBRACKET: return '[';
        case NUE_SC_RIGHTBRACKET: return ']';
        case NUE_SC_BACKSLASH: return '\\';
        case NUE_SC_SEMICOLON: return ';';
        case NUE_SC_APOSTROPHE: return '\'';
        case NUE_SC_GRAVE: return '`';
        case NUE_SC_COMMA: return ',';
        case NUE_SC_PERIOD: return '.';
        case NUE_SC_SLASH: return '/';
        case NUE_SC_DELETE: return 127;
        default: return int32_t(sc | NUE_KEYCODE_SCANCODE_MASK);
    }
}

NUE_API nue_scancode NUE_CALL nue_key_to_scancode(int32_t keycode) {
    if (keycode & NUE_KEYCODE_SCANCODE_MASK) return nue_scancode(keycode & ~NUE_KEYCODE_SCANCODE_MASK);
    for (nue_scancode sc = 4; sc < 130; ++sc) if (nue_key_from_scancode(sc) == keycode) return sc;
    if (keycode >= 'A' && keycode <= 'Z') return NUE_SC_A + nue_scancode(keycode - 'A');
    return NUE_SC_UNKNOWN;
}

NUE_API const char* NUE_CALL nue_scancode_name(nue_scancode sc) {
    static const char* const letters[26] = {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
    static const char* const digits[10] = {"1","2","3","4","5","6","7","8","9","0"};
    static const char* const fkeys[24] = {"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12","F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24"};
    static const char* const kp[10] = {"Keypad 1","Keypad 2","Keypad 3","Keypad 4","Keypad 5","Keypad 6","Keypad 7","Keypad 8","Keypad 9","Keypad 0"};
    if (sc >= NUE_SC_A && sc <= NUE_SC_Z) return letters[sc - NUE_SC_A];
    if (sc >= NUE_SC_1 && sc <= NUE_SC_0) return digits[sc - NUE_SC_1];
    if (sc >= NUE_SC_F1 && sc <= NUE_SC_F12) return fkeys[sc - NUE_SC_F1];
    if (sc >= NUE_SC_F13 && sc <= NUE_SC_F24) return fkeys[12 + sc - NUE_SC_F13];
    if (sc >= NUE_SC_KP_1 && sc <= NUE_SC_KP_0) return kp[sc - NUE_SC_KP_1];
    switch (sc) {
        case NUE_SC_RETURN: return "Return";
        case NUE_SC_ESCAPE: return "Escape";
        case NUE_SC_BACKSPACE: return "Backspace";
        case NUE_SC_TAB: return "Tab";
        case NUE_SC_SPACE: return "Space";
        case NUE_SC_MINUS: return "-";
        case NUE_SC_EQUALS: return "=";
        case NUE_SC_LEFTBRACKET: return "[";
        case NUE_SC_RIGHTBRACKET: return "]";
        case NUE_SC_BACKSLASH: return "\\";
        case NUE_SC_SEMICOLON: return ";";
        case NUE_SC_APOSTROPHE: return "'";
        case NUE_SC_GRAVE: return "`";
        case NUE_SC_COMMA: return ",";
        case NUE_SC_PERIOD: return ".";
        case NUE_SC_SLASH: return "/";
        case NUE_SC_CAPSLOCK: return "CapsLock";
        case NUE_SC_PRINTSCREEN: return "PrintScreen";
        case NUE_SC_SCROLLLOCK: return "ScrollLock";
        case NUE_SC_PAUSE: return "Pause";
        case NUE_SC_INSERT: return "Insert";
        case NUE_SC_HOME: return "Home";
        case NUE_SC_PAGEUP: return "PageUp";
        case NUE_SC_DELETE: return "Delete";
        case NUE_SC_END: return "End";
        case NUE_SC_PAGEDOWN: return "PageDown";
        case NUE_SC_RIGHT: return "Right";
        case NUE_SC_LEFT: return "Left";
        case NUE_SC_DOWN: return "Down";
        case NUE_SC_UP: return "Up";
        case NUE_SC_NUMLOCK: return "NumLock";
        case NUE_SC_KP_DIVIDE: return "Keypad /";
        case NUE_SC_KP_MULTIPLY: return "Keypad *";
        case NUE_SC_KP_MINUS: return "Keypad -";
        case NUE_SC_KP_PLUS: return "Keypad +";
        case NUE_SC_KP_ENTER: return "Keypad Enter";
        case NUE_SC_KP_PERIOD: return "Keypad .";
        case NUE_SC_APPLICATION: return "Application";
        case NUE_SC_LCTRL: return "Left Ctrl";
        case NUE_SC_LSHIFT: return "Left Shift";
        case NUE_SC_LALT: return "Left Alt";
        case NUE_SC_LGUI: return "Left GUI";
        case NUE_SC_RCTRL: return "Right Ctrl";
        case NUE_SC_RSHIFT: return "Right Shift";
        case NUE_SC_RALT: return "Right Alt";
        case NUE_SC_RGUI: return "Right GUI";
        case NUE_SC_AC_BACK: return "AC Back";
        default: return "";
    }
}

NUE_API uint32_t NUE_CALL nue_mouse_get_state(float* x, float* y) {
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (x) *x = s.mouse_x;
    if (y) *y = s.mouse_y;
    return s.buttons;
}

NUE_API uint32_t NUE_CALL nue_mouse_get_relative_state(float* dx, float* dy) {
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (dx) *dx = s.rel_dx;
    if (dy) *dy = s.rel_dy;
    s.rel_dx = s.rel_dy = 0;
    return s.buttons;
}

NUE_API nue_window NUE_CALL nue_mouse_get_focus(void) {
    InputState& s = st();
    nue_window_id id;
    { std::lock_guard<std::mutex> lock(s.mutex); id = s.mouse_focus; }
    return id ? nue_window_from_id(id) : nullptr;
}

NUE_API nue_result NUE_CALL nue_touch_get_fingers(nue_finger* fingers, uint32_t capacity, uint32_t* out_count) {
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (out_count) *out_count = s.finger_count;
    for (uint32_t i = 0; fingers && i < capacity && i < s.finger_count; ++i) fingers[i] = s.fingers[i];
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_touch_set_mouse_emulation(nue_bool enabled) {
    InputState& s = st();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.mouse_emulation = enabled != 0;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_gamepad_apply_deadzone(int16_t x, int16_t y, float deadzone, float* out_x, float* out_y) {
    float fx = float(x) / 32767.0f, fy = float(y) / 32767.0f;
    if (fx < -1) fx = -1;
    if (fy < -1) fy = -1;
    float mag = std::sqrt(fx * fx + fy * fy);
    if (deadzone < 0) deadzone = 0;
    if (deadzone >= 1) deadzone = 0.999f;
    if (mag <= deadzone || mag <= 0) { if (out_x) *out_x = 0; if (out_y) *out_y = 0; return; }
    float scaled = (mag - deadzone) / (1.0f - deadzone);
    if (scaled > 1) scaled = 1;
    if (out_x) *out_x = fx / mag * scaled;
    if (out_y) *out_y = fy / mag * scaled;
}

NUE_API const char* NUE_CALL nue_gamepad_button_name(nue_gamepad_button b, nue_glyph_family fam) {
    const bool ps = fam == NUE_GLYPHS_PLAYSTATION, nin = fam == NUE_GLYPHS_NINTENDO;
    switch (b) {
        case NUE_PAD_SOUTH: return ps ? "Cross" : nin ? "B" : "A";
        case NUE_PAD_EAST: return ps ? "Circle" : nin ? "A" : "B";
        case NUE_PAD_WEST: return ps ? "Square" : nin ? "Y" : "X";
        case NUE_PAD_NORTH: return ps ? "Triangle" : nin ? "X" : "Y";
        case NUE_PAD_BACK: return ps ? "Share" : nin ? "Minus" : "View";
        case NUE_PAD_GUIDE: return ps ? "PS" : nin ? "Home" : "Guide";
        case NUE_PAD_START: return ps ? "Options" : nin ? "Plus" : "Menu";
        case NUE_PAD_LEFT_STICK: return ps ? "L3" : "Left Stick";
        case NUE_PAD_RIGHT_STICK: return ps ? "R3" : "Right Stick";
        case NUE_PAD_LEFT_SHOULDER: return ps ? "L1" : nin ? "L" : "LB";
        case NUE_PAD_RIGHT_SHOULDER: return ps ? "R1" : nin ? "R" : "RB";
        case NUE_PAD_DPAD_UP: return "D-Pad Up";
        case NUE_PAD_DPAD_DOWN: return "D-Pad Down";
        case NUE_PAD_DPAD_LEFT: return "D-Pad Left";
        case NUE_PAD_DPAD_RIGHT: return "D-Pad Right";
        case NUE_PAD_MISC1: return "Misc";
        case NUE_PAD_PADDLE1: return "Paddle 1";
        case NUE_PAD_PADDLE2: return "Paddle 2";
        case NUE_PAD_PADDLE3: return "Paddle 3";
        case NUE_PAD_PADDLE4: return "Paddle 4";
        case NUE_PAD_TOUCHPAD: return "Touchpad";
        default: return "";
    }
}

}  // extern "C"
