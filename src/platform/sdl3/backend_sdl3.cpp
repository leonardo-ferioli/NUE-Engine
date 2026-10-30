// The shared SDL3 backend: displays, windows, cursor, keyboard/mouse/touch/gamepad events, device sensors, audio devices.
// It implements nue_backend (internal/backend.h). OS-specific policy (power, game mode, storage rules, crash handling...) is NOT here: it lives in the
// platform layers (src/platform/<os>) behind nue_platform_ops.
#include "internal/core_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

nue_result sdl_window_refresh(void* impl, nue_window_snapshot* s);   // defined below, global (the translator in the anonymous namespace calls it)

namespace {

struct Win {
    SDL_Window* win = nullptr;
    nue_window_id id = 0;
    bool attached = false;
    bool relative = false;
    nue_window_mode mode = NUE_WINDOW_MODE_WINDOWED;
};

struct SdlState {
    std::mutex mutex;
    bool active = false;
    bool owns_lifecycle = false;
    std::map<SDL_WindowID, Win*> by_sdl;     // SDL window id -> our window
    std::map<nue_window_id, Win*> by_nue;
    std::map<SDL_JoystickID, SDL_Gamepad*> pads;
    std::map<int, SDL_Sensor*> sensors;       // nue_sensor_kind -> opened device sensor
    SDL_Cursor* cursor = nullptr;
    nue_backend_init init{};
    uint32_t sdl_subsystems = 0;
};
SdlState g;

// ---- conversions ------------------------------------------------------------------------------------------------------------------
nue_orientation conv_orientation(SDL_DisplayOrientation o) {
    switch (o) {
        case SDL_ORIENTATION_LANDSCAPE: return NUE_ORIENTATION_LANDSCAPE;
        case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return NUE_ORIENTATION_LANDSCAPE_FLIPPED;
        case SDL_ORIENTATION_PORTRAIT: return NUE_ORIENTATION_PORTRAIT;
        case SDL_ORIENTATION_PORTRAIT_FLIPPED: return NUE_ORIENTATION_PORTRAIT_FLIPPED;
        default: return NUE_ORIENTATION_UNKNOWN;
    }
}

void fill_mode(const SDL_DisplayMode* m, nue_display_mode* out) {
    NUE_STRUCT_INIT(out);
    if (!m) return;
    out->width = m->w;
    out->height = m->h;
    out->refresh_rate_hz = m->refresh_rate;
    out->pixel_density = m->pixel_density;
    out->format = uint32_t(m->format);
}

nue_gamepad_type conv_pad_type(SDL_GamepadType t) {
    switch (t) {
        case SDL_GAMEPAD_TYPE_STANDARD: return NUE_PAD_TYPE_STANDARD;
        case SDL_GAMEPAD_TYPE_XBOX360: return NUE_PAD_TYPE_XBOX360;
        case SDL_GAMEPAD_TYPE_XBOXONE: return NUE_PAD_TYPE_XBOXONE;
        case SDL_GAMEPAD_TYPE_PS3: return NUE_PAD_TYPE_PS3;
        case SDL_GAMEPAD_TYPE_PS4: return NUE_PAD_TYPE_PS4;
        case SDL_GAMEPAD_TYPE_PS5: return NUE_PAD_TYPE_PS5;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return NUE_PAD_TYPE_SWITCH_PRO;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return NUE_PAD_TYPE_SWITCH_JOYCON_LEFT;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return NUE_PAD_TYPE_SWITCH_JOYCON_RIGHT;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return NUE_PAD_TYPE_SWITCH_JOYCON_PAIR;
        default: return NUE_PAD_TYPE_UNKNOWN;
    }
}

nue_glyph_family glyphs_of(nue_gamepad_type t) {
    switch (t) {
        case NUE_PAD_TYPE_XBOX360: case NUE_PAD_TYPE_XBOXONE: return NUE_GLYPHS_XBOX;
        case NUE_PAD_TYPE_PS3: case NUE_PAD_TYPE_PS4: case NUE_PAD_TYPE_PS5: return NUE_GLYPHS_PLAYSTATION;
        case NUE_PAD_TYPE_SWITCH_PRO: case NUE_PAD_TYPE_SWITCH_JOYCON_LEFT: case NUE_PAD_TYPE_SWITCH_JOYCON_RIGHT: case NUE_PAD_TYPE_SWITCH_JOYCON_PAIR: return NUE_GLYPHS_NINTENDO;
        default: return NUE_GLYPHS_GENERIC_GAMEPAD;
    }
}

// SDL gamepad button <-> nue_gamepad_button (the first 15 are in the same order; the rest are mapped by hand)
int nue_button_from_sdl(int b) {
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return NUE_PAD_SOUTH;
        case SDL_GAMEPAD_BUTTON_EAST: return NUE_PAD_EAST;
        case SDL_GAMEPAD_BUTTON_WEST: return NUE_PAD_WEST;
        case SDL_GAMEPAD_BUTTON_NORTH: return NUE_PAD_NORTH;
        case SDL_GAMEPAD_BUTTON_BACK: return NUE_PAD_BACK;
        case SDL_GAMEPAD_BUTTON_GUIDE: return NUE_PAD_GUIDE;
        case SDL_GAMEPAD_BUTTON_START: return NUE_PAD_START;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return NUE_PAD_LEFT_STICK;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return NUE_PAD_RIGHT_STICK;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return NUE_PAD_LEFT_SHOULDER;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return NUE_PAD_RIGHT_SHOULDER;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return NUE_PAD_DPAD_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return NUE_PAD_DPAD_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return NUE_PAD_DPAD_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return NUE_PAD_DPAD_RIGHT;
        case SDL_GAMEPAD_BUTTON_MISC1: return NUE_PAD_MISC1;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return NUE_PAD_PADDLE1;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return NUE_PAD_PADDLE2;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return NUE_PAD_PADDLE3;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return NUE_PAD_PADDLE4;
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return NUE_PAD_TOUCHPAD;
        default: return -1;
    }
}


int nue_axis_from_sdl(int a) {
    switch (a) {
        case SDL_GAMEPAD_AXIS_LEFTX: return NUE_PAD_AXIS_LEFTX;
        case SDL_GAMEPAD_AXIS_LEFTY: return NUE_PAD_AXIS_LEFTY;
        case SDL_GAMEPAD_AXIS_RIGHTX: return NUE_PAD_AXIS_RIGHTX;
        case SDL_GAMEPAD_AXIS_RIGHTY: return NUE_PAD_AXIS_RIGHTY;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return NUE_PAD_AXIS_LEFT_TRIGGER;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return NUE_PAD_AXIS_RIGHT_TRIGGER;
        default: return -1;
    }
}

SDL_Gamepad* pad_of(nue_gamepad_id id) {
    std::lock_guard<std::mutex> lock(g.mutex);
    auto it = g.pads.find(SDL_JoystickID(id));
    if (it != g.pads.end()) return it->second;
    SDL_Gamepad* p = SDL_OpenGamepad(SDL_JoystickID(id));
    if (p) g.pads[SDL_JoystickID(id)] = p;
    return p;
}

Win* win_of_sdl(SDL_WindowID id) {
    std::lock_guard<std::mutex> lock(g.mutex);
    auto it = g.by_sdl.find(id);
    return it == g.by_sdl.end() ? nullptr : it->second;
}

nue_result sdl_fail(const char* what) { return nuei::fail(NUE_ERR_BACKEND, "%s failed: %s", what, SDL_GetError()); }

bool compositor_owns_modes() {
    // gamescope / Steam gaming mode: the compositor owns the display mode, applications must stay borderless
    return (nuei::platform() && nue_system_get_platform_flags() & (NUE_PLATFORM_GAMESCOPE | NUE_PLATFORM_STEAM_GAMING_MODE)) != 0;
}

// ---- event translation ------------------------------------------------------------------------------------------------------------------
void push(nue_event_type type, nue_window_id win, nue_event& e) {
    e.type = type;
    e.window_id = win;
    nue_internal_push_event(&e);
}

void zero(nue_event& e) { std::memset(&e, 0, sizeof(e)); }

nue_window_id nue_win_from_sdl(SDL_WindowID id) { Win* w = win_of_sdl(id); return w ? w->id : 0; }

void translate(const SDL_Event& ev) {
    nue_event e;
    zero(e);
    switch (ev.type) {
        case SDL_EVENT_QUIT: push(NUE_EVENT_QUIT, 0, e); break;
        case SDL_EVENT_TERMINATING: if (!g.owns_lifecycle) nue_internal_set_app_state(NUE_APP_STATE_TERMINATING); break;
        case SDL_EVENT_LOW_MEMORY: push(NUE_EVENT_APP_LOW_MEMORY, 0, e); break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND: if (!g.owns_lifecycle) nue_internal_set_app_state(NUE_APP_STATE_BACKGROUND); break;
        case SDL_EVENT_DID_ENTER_FOREGROUND: if (!g.owns_lifecycle) nue_internal_set_app_state(NUE_APP_STATE_FOREGROUND); break;
        case SDL_EVENT_LOCALE_CHANGED: push(NUE_EVENT_LOCALE_CHANGED, 0, e); break;
        case SDL_EVENT_SYSTEM_THEME_CHANGED: push(NUE_EVENT_SYSTEM_THEME_CHANGED, 0, e); break;

        case SDL_EVENT_DISPLAY_ADDED: e.payload.display.display_id = ev.display.displayID; push(NUE_EVENT_DISPLAY_ADDED, 0, e); break;
        case SDL_EVENT_DISPLAY_REMOVED: e.payload.display.display_id = ev.display.displayID; push(NUE_EVENT_DISPLAY_REMOVED, 0, e); break;
        case SDL_EVENT_DISPLAY_ORIENTATION: case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED: case SDL_EVENT_DISPLAY_MOVED:
            e.payload.display.display_id = ev.display.displayID;
            e.payload.display.data1 = ev.display.data1;
            e.payload.display.data2 = ev.display.data2;
            push(NUE_EVENT_DISPLAY_CHANGED, 0, e);
            break;

        case SDL_EVENT_WINDOW_SHOWN: case SDL_EVENT_WINDOW_HIDDEN: case SDL_EVENT_WINDOW_MOVED: case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_MINIMIZED: case SDL_EVENT_WINDOW_MAXIMIZED: case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MOUSE_ENTER: case SDL_EVENT_WINDOW_MOUSE_LEAVE: case SDL_EVENT_WINDOW_FOCUS_GAINED: case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: case SDL_EVENT_WINDOW_DISPLAY_CHANGED: case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: case SDL_EVENT_WINDOW_OCCLUDED: case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
            Win* w = win_of_sdl(ev.window.windowID);
            if (!w) break;
            nue_event_type t = NUE_EVENT_NONE;
            switch (ev.type) {
                case SDL_EVENT_WINDOW_SHOWN: t = NUE_EVENT_WINDOW_SHOWN; break;
                case SDL_EVENT_WINDOW_HIDDEN: t = NUE_EVENT_WINDOW_HIDDEN; break;
                case SDL_EVENT_WINDOW_MOVED: t = NUE_EVENT_WINDOW_MOVED; break;
                case SDL_EVENT_WINDOW_RESIZED: t = NUE_EVENT_WINDOW_RESIZED; break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: t = NUE_EVENT_WINDOW_PIXEL_SIZE_CHANGED; break;
                case SDL_EVENT_WINDOW_MINIMIZED: t = NUE_EVENT_WINDOW_MINIMIZED; break;
                case SDL_EVENT_WINDOW_MAXIMIZED: t = NUE_EVENT_WINDOW_MAXIMIZED; break;
                case SDL_EVENT_WINDOW_RESTORED: t = NUE_EVENT_WINDOW_RESTORED; break;
                case SDL_EVENT_WINDOW_MOUSE_ENTER: t = NUE_EVENT_WINDOW_MOUSE_ENTER; break;
                case SDL_EVENT_WINDOW_MOUSE_LEAVE: t = NUE_EVENT_WINDOW_MOUSE_LEAVE; break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED: t = NUE_EVENT_WINDOW_FOCUS_GAINED; break;
                case SDL_EVENT_WINDOW_FOCUS_LOST: t = NUE_EVENT_WINDOW_FOCUS_LOST; break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: t = NUE_EVENT_WINDOW_CLOSE_REQUESTED; break;
                case SDL_EVENT_WINDOW_DISPLAY_CHANGED: t = NUE_EVENT_WINDOW_DISPLAY_CHANGED; break;
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: t = NUE_EVENT_WINDOW_CONTENT_SCALE_CHANGED; break;
                case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: t = NUE_EVENT_WINDOW_SAFE_AREA_CHANGED; break;
                case SDL_EVENT_WINDOW_OCCLUDED: t = NUE_EVENT_WINDOW_OCCLUDED; break;
                case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: t = NUE_EVENT_WINDOW_ENTER_FULLSCREEN; break;
                default: t = NUE_EVENT_WINDOW_LEAVE_FULLSCREEN; break;
            }
            if (t == NUE_EVENT_WINDOW_FOCUS_GAINED || t == NUE_EVENT_WINDOW_FOCUS_LOST || t == NUE_EVENT_WINDOW_ENTER_FULLSCREEN || t == NUE_EVENT_WINDOW_LEAVE_FULLSCREEN)
                NUE_LOGD("sdl3", "window %u: %s", unsigned(w->id), t == NUE_EVENT_WINDOW_FOCUS_GAINED ? "keyboard focus gained" : t == NUE_EVENT_WINDOW_FOCUS_LOST ? "keyboard focus lost" :
                         t == NUE_EVENT_WINDOW_ENTER_FULLSCREEN ? "entered fullscreen" : "left fullscreen");
            e.payload.window.x = ev.window.data1;
            e.payload.window.y = ev.window.data2;
            if (ev.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED || ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) e.payload.window.scale = SDL_GetWindowPixelDensity(w->win);
            // refresh the cached snapshot first so listeners that query the window see the new state
            nue_window_snapshot snap;
            NUE_STRUCT_INIT(&snap);
            if (NUE_SUCCEEDED(sdl_window_refresh(w, &snap))) nue_internal_window_changed(w->id, &snap);
            push(t, w->id, e);
            break;
        }

        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            e.payload.key.device_id = uint32_t(ev.key.which);
            e.payload.key.scancode = uint32_t(ev.key.scancode);
            e.payload.key.keycode = int32_t(ev.key.key);
            e.payload.key.mods = uint16_t(ev.key.mod);
            e.payload.key.down = ev.key.down ? 1 : 0;
            e.payload.key.repeat = ev.key.repeat ? 1 : 0;
            if (!ev.key.repeat) NUE_LOGD("sdl3", "key %s: scancode %u, window %u", ev.key.down ? "down" : "up", unsigned(ev.key.scancode), unsigned(nue_win_from_sdl(ev.key.windowID)));
            push(ev.key.down ? NUE_EVENT_KEY_DOWN : NUE_EVENT_KEY_UP, nue_win_from_sdl(ev.key.windowID), e);
            break;
        case SDL_EVENT_TEXT_INPUT: {
            const char* t = ev.text.text;
            size_t len = t ? std::strlen(t) : 0, off = 0;
            do {   // text events carry up to 31 bytes: longer input is split on character boundaries
                size_t n = std::min<size_t>(len - off, 31);
                while (off + n < len && n > 0 && (static_cast<unsigned char>(t[off + n]) & 0xC0) == 0x80) --n;
                nue_event te;
                zero(te);
                std::memcpy(te.payload.text.text, t + off, n);
                push(NUE_EVENT_TEXT_INPUT, nue_win_from_sdl(ev.text.windowID), te);
                off += n;
                if (n == 0) break;
            } while (off < len);
            break;
        }
        case SDL_EVENT_TEXT_EDITING: {
            const char* t = ev.edit.text;
            size_t n = std::min<size_t>(t ? std::strlen(t) : 0, 31);
            std::memcpy(e.payload.text.text, t ? t : "", n);
            e.payload.text.editing_start = uint8_t(std::max(0, std::min(255, int(ev.edit.start))));
            e.payload.text.editing_length = uint8_t(std::max(0, std::min(255, int(ev.edit.length))));
            push(NUE_EVENT_TEXT_EDITING, nue_win_from_sdl(ev.edit.windowID), e);
            break;
        }

        case SDL_EVENT_MOUSE_MOTION:
            e.payload.motion.device_id = uint32_t(ev.motion.which);
            e.payload.motion.x = ev.motion.x; e.payload.motion.y = ev.motion.y;
            e.payload.motion.dx = ev.motion.xrel; e.payload.motion.dy = ev.motion.yrel;
            e.payload.motion.buttons = uint32_t(ev.motion.state);
            e.payload.motion.from_touch = ev.motion.which == SDL_TOUCH_MOUSEID ? 1 : 0;
            push(NUE_EVENT_MOUSE_MOTION, nue_win_from_sdl(ev.motion.windowID), e);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
            e.payload.button.device_id = uint32_t(ev.button.which);
            e.payload.button.x = ev.button.x; e.payload.button.y = ev.button.y;
            e.payload.button.button = ev.button.button;
            e.payload.button.down = ev.button.down ? 1 : 0;
            e.payload.button.clicks = ev.button.clicks;
            push(ev.button.down ? NUE_EVENT_MOUSE_BUTTON_DOWN : NUE_EVENT_MOUSE_BUTTON_UP, nue_win_from_sdl(ev.button.windowID), e);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            e.payload.wheel.device_id = uint32_t(ev.wheel.which);
            e.payload.wheel.x = ev.wheel.x; e.payload.wheel.y = ev.wheel.y;
            e.payload.wheel.mouse_x = ev.wheel.mouse_x; e.payload.wheel.mouse_y = ev.wheel.mouse_y;
            e.payload.wheel.flipped = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? 1 : 0;
            push(NUE_EVENT_MOUSE_WHEEL, nue_win_from_sdl(ev.wheel.windowID), e);
            break;

        case SDL_EVENT_GAMEPAD_ADDED: {
            if (!pad_of(nue_gamepad_id(ev.gdevice.which))) break;
            e.payload.gamepad.gamepad_id = uint32_t(ev.gdevice.which);
            push(NUE_EVENT_GAMEPAD_ADDED, 0, e);
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED: {
            e.payload.gamepad.gamepad_id = uint32_t(ev.gdevice.which);
            push(NUE_EVENT_GAMEPAD_REMOVED, 0, e);   // pushed first so listeners can still query the pad
            std::lock_guard<std::mutex> lock(g.mutex);
            auto it = g.pads.find(ev.gdevice.which);
            if (it != g.pads.end()) { SDL_CloseGamepad(it->second); g.pads.erase(it); }
            break;
        }
        case SDL_EVENT_GAMEPAD_REMAPPED: e.payload.gamepad.gamepad_id = uint32_t(ev.gdevice.which); push(NUE_EVENT_GAMEPAD_REMAPPED, 0, e); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            int b = nue_button_from_sdl(ev.gbutton.button);
            if (b < 0) break;
            e.payload.gamepad.gamepad_id = uint32_t(ev.gbutton.which);
            e.payload.gamepad.control = b;
            e.payload.gamepad.value = ev.gbutton.down ? 1 : 0;
            push(ev.gbutton.down ? NUE_EVENT_GAMEPAD_BUTTON_DOWN : NUE_EVENT_GAMEPAD_BUTTON_UP, 0, e);
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            int a = nue_axis_from_sdl(ev.gaxis.axis);
            if (a < 0) break;
            e.payload.gamepad.gamepad_id = uint32_t(ev.gaxis.which);
            e.payload.gamepad.control = a;
            e.payload.gamepad.value = ev.gaxis.value;
            push(NUE_EVENT_GAMEPAD_AXIS, 0, e);
            break;
        }

        case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_CANCELED:
            e.payload.finger.touch_id = ev.tfinger.touchID;
            e.payload.finger.finger_id = ev.tfinger.fingerID;
            e.payload.finger.x = ev.tfinger.x; e.payload.finger.y = ev.tfinger.y;
            e.payload.finger.dx = ev.tfinger.dx; e.payload.finger.dy = ev.tfinger.dy;
            e.payload.finger.pressure = ev.tfinger.pressure;
            push(ev.type == SDL_EVENT_FINGER_DOWN ? NUE_EVENT_FINGER_DOWN : ev.type == SDL_EVENT_FINGER_UP ? NUE_EVENT_FINGER_UP
                 : ev.type == SDL_EVENT_FINGER_MOTION ? NUE_EVENT_FINGER_MOTION : NUE_EVENT_FINGER_CANCELED, nue_win_from_sdl(ev.tfinger.windowID), e);
            break;

        case SDL_EVENT_AUDIO_DEVICE_ADDED: case SDL_EVENT_AUDIO_DEVICE_REMOVED:
            e.payload.audio_device.device_id = uint32_t(ev.adevice.which);
            e.payload.audio_device.is_capture = ev.adevice.recording ? 1 : 0;
            push(ev.type == SDL_EVENT_AUDIO_DEVICE_ADDED ? NUE_EVENT_AUDIO_DEVICE_ADDED : NUE_EVENT_AUDIO_DEVICE_REMOVED, 0, e);
            break;
        default: break;
    }
}

// ---- backend entry points ---------------------------------------------------------------------------------------------------------------
nue_result b_init(const nue_backend_init* init) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.active) return NUE_OK;
    g.init = *init;
    g.owns_lifecycle = init->platform_owns_lifecycle != 0;
    if (init->headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    }
    const nue_subsystem_mask want = init->desc && init->desc->subsystems ? init->desc->subsystems : NUE_SUBSYSTEM_ALL_DEFAULT;
    uint32_t flags = SDL_INIT_EVENTS;
    if (want & NUE_SUBSYSTEM_VIDEO) flags |= SDL_INIT_VIDEO;
    if (want & NUE_SUBSYSTEM_AUDIO) flags |= SDL_INIT_AUDIO;
    if (want & NUE_SUBSYSTEM_INPUT) flags |= SDL_INIT_GAMEPAD | SDL_INIT_SENSOR;
    if (!SDL_Init(flags)) {
        // retry with what is essential: a machine without audio hardware or a joystick subsystem must still run
        const uint32_t essential = SDL_INIT_EVENTS | (flags & SDL_INIT_VIDEO);
        NUE_LOGW("sdl3", "SDL_Init(0x%x) failed (%s); retrying with the essential subsystems only", flags, SDL_GetError());
        if (!SDL_Init(essential)) return sdl_fail("SDL_Init");
        flags = essential;
        // audio/gamepads are then initialised individually below so the optional parts come up if they can
        if ((want & NUE_SUBSYSTEM_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO)) flags |= SDL_INIT_AUDIO;
        if ((want & NUE_SUBSYSTEM_INPUT) && SDL_InitSubSystem(SDL_INIT_GAMEPAD)) flags |= SDL_INIT_GAMEPAD;
    }
    g.sdl_subsystems = flags;
    g.active = true;
    NUE_LOGI("sdl3", "SDL %d.%d.%d, video driver '%s', audio driver '%s'", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
             SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "-",
             SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "-");
    return NUE_OK;
}

void b_shutdown() {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.active) return;
    for (auto& kv : g.pads) SDL_CloseGamepad(kv.second);
    g.pads.clear();
    for (auto& kv : g.sensors) SDL_CloseSensor(kv.second);
    g.sensors.clear();
    if (g.cursor) { SDL_DestroyCursor(g.cursor); g.cursor = nullptr; }
    for (auto& kv : g.by_nue) { if (!kv.second->attached && kv.second->win) SDL_DestroyWindow(kv.second->win); delete kv.second; }
    g.by_nue.clear();
    g.by_sdl.clear();
    SDL_Quit();
    g.active = false;
}

int32_t b_pump() {
    SDL_Event ev;
    int32_t n = 0;
    while (SDL_PollEvent(&ev)) { translate(ev); ++n; }
    return n;
}

nue_result b_wait(int32_t timeout_ms) {
    SDL_Event ev;
    if (SDL_WaitEventTimeout(&ev, timeout_ms)) translate(ev);
    return NUE_OK;
}

void b_wake() {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    SDL_PushEvent(&ev);   // an ignored user event: its only job is to end SDL_WaitEventTimeout
}

// ---- video -------------------------------------------------------------------------------------------------------------------------------
nue_result v_display_enum(nue_display_info* infos, uint32_t capacity, uint32_t* out_count) {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (!ids) { *out_count = 0; return NUE_OK; }
    *out_count = uint32_t(count);
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; infos && uint32_t(i) < capacity && i < count; ++i) {
        nue_display_info& d = infos[i];
        NUE_STRUCT_INIT(&d);
        const SDL_DisplayID id = ids[i];
        d.id = id;
        const char* name = SDL_GetDisplayName(id);
        std::snprintf(d.name, sizeof(d.name), "%s", name ? name : "display");
        SDL_Rect r;
        if (SDL_GetDisplayBounds(id, &r)) d.bounds = {r.x, r.y, r.w, r.h};
        if (SDL_GetDisplayUsableBounds(id, &r)) d.usable_bounds = {r.x, r.y, r.w, r.h};
        fill_mode(SDL_GetCurrentDisplayMode(id), &d.current_mode);
        fill_mode(SDL_GetDesktopDisplayMode(id), &d.desktop_mode);
        d.content_scale = SDL_GetDisplayContentScale(id);
        d.orientation = conv_orientation(SDL_GetCurrentDisplayOrientation(id));
        d.natural_orientation = conv_orientation(SDL_GetNaturalDisplayOrientation(id));
        d.is_primary = id == primary ? NUE_TRUE : NUE_FALSE;
        d.min_refresh_hz = d.max_refresh_hz = d.current_mode.refresh_rate_hz;
        int mc = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &mc);
        if (modes) {
            float lo = d.current_mode.refresh_rate_hz, hi = lo;
            for (int k = 0; k < mc; ++k) if (modes[k]->w == d.current_mode.width && modes[k]->h == d.current_mode.height) { lo = std::min(lo, modes[k]->refresh_rate); hi = std::max(hi, modes[k]->refresh_rate); }
            d.min_refresh_hz = lo; d.max_refresh_hz = hi;
            SDL_free(modes);
        }
    }
    SDL_free(ids);
    return NUE_OK;
}

nue_result v_display_modes(nue_display_id id, nue_display_mode* modes, uint32_t capacity, uint32_t* out_count) {
    int count = 0;
    SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(id, &count);
    if (!list) { *out_count = 0; return NUE_ERR_NOT_FOUND; }
    *out_count = uint32_t(count);
    for (int i = 0; modes && uint32_t(i) < capacity && i < count; ++i) fill_mode(list[i], &modes[i]);
    SDL_free(list);
    return NUE_OK;
}

}  // namespace

// Window snapshot (also used by the event translator above).
nue_result sdl_window_refresh(void* impl, nue_window_snapshot* s) {
    Win* w = static_cast<Win*>(impl);
    NUE_STRUCT_INIT(s);
    int x = 0, y = 0, ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowPosition(w->win, &x, &y);
    SDL_GetWindowSize(w->win, &ww, &wh);
    SDL_GetWindowSizeInPixels(w->win, &pw, &ph);
    s->x = x; s->y = y; s->width = ww; s->height = wh; s->pixel_width = pw; s->pixel_height = ph;
    s->content_scale = SDL_GetWindowDisplayScale(w->win);
    s->pixel_density = SDL_GetWindowPixelDensity(w->win);
    SDL_DisplayID disp = SDL_GetDisplayForWindow(w->win);
    s->display = disp;
    const SDL_DisplayMode* dm = disp ? SDL_GetCurrentDisplayMode(disp) : nullptr;
    s->refresh_rate_hz = dm ? dm->refresh_rate : 0.0f;
    const SDL_WindowFlags f = SDL_GetWindowFlags(w->win);
    if (f & SDL_WINDOW_RESIZABLE) s->flags |= NUE_WINDOW_RESIZABLE;
    if (f & SDL_WINDOW_HIDDEN) s->flags |= NUE_WINDOW_HIDDEN;
    if (f & SDL_WINDOW_BORDERLESS) s->flags |= NUE_WINDOW_BORDERLESS;
    if (f & SDL_WINDOW_ALWAYS_ON_TOP) s->flags |= NUE_WINDOW_ALWAYS_ON_TOP;
    if (f & SDL_WINDOW_MAXIMIZED) s->flags |= NUE_WINDOW_MAXIMIZED;
    if (f & SDL_WINDOW_MINIMIZED) s->flags |= NUE_WINDOW_MINIMIZED;
    if (f & SDL_WINDOW_HIGH_PIXEL_DENSITY) s->flags |= NUE_WINDOW_HIGH_DPI;
    s->mode = (f & SDL_WINDOW_FULLSCREEN) ? (SDL_GetWindowFullscreenMode(w->win) ? NUE_WINDOW_MODE_EXCLUSIVE : NUE_WINDOW_MODE_BORDERLESS) : NUE_WINDOW_MODE_WINDOWED;
    s->focused = (f & SDL_WINDOW_INPUT_FOCUS) ? NUE_TRUE : NUE_FALSE;
    s->visible = (f & SDL_WINDOW_HIDDEN) ? NUE_FALSE : NUE_TRUE;
    s->occluded = (f & SDL_WINDOW_OCCLUDED) ? NUE_TRUE : NUE_FALSE;
    s->mouse_inside = (f & SDL_WINDOW_MOUSE_FOCUS) ? NUE_TRUE : NUE_FALSE;
    s->relative_mouse = w->relative ? NUE_TRUE : NUE_FALSE;
    SDL_Rect sa;
    if (SDL_GetWindowSafeArea(w->win, &sa)) s->safe_area = {sa.x, sa.y, sa.w, sa.h}; else s->safe_area = {0, 0, ww, wh};
    NUE_STRUCT_INIT(&s->native);
    const SDL_PropertiesID p = SDL_GetWindowProperties(w->win);
#if defined(SDL_PLATFORM_WINDOWS)
    s->native.kind = NUE_NATIVE_WIN32_HWND;
    s->native.window = uint64_t(reinterpret_cast<uintptr_t>(SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)));
#elif defined(SDL_PLATFORM_ANDROID)
    s->native.kind = NUE_NATIVE_ANDROID_WINDOW;
    s->native.window = uint64_t(reinterpret_cast<uintptr_t>(SDL_GetPointerProperty(p, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)));
#elif defined(SDL_PLATFORM_IOS)
    s->native.kind = NUE_NATIVE_UIKIT_WINDOW;
    s->native.window = uint64_t(reinterpret_cast<uintptr_t>(SDL_GetPointerProperty(p, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr)));
#elif defined(SDL_PLATFORM_MACOS)
    s->native.kind = NUE_NATIVE_COCOA_WINDOW;
    s->native.window = uint64_t(reinterpret_cast<uintptr_t>(SDL_GetPointerProperty(p, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr)));
#else
    if (SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr)) {
        s->native.kind = NUE_NATIVE_WAYLAND_SURFACE;
        s->native.window = uint64_t(reinterpret_cast<uintptr_t>(SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr)));
        s->native.display = uint64_t(reinterpret_cast<uintptr_t>(SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)));
    } else if (SDL_GetNumberProperty(p, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)) {
        s->native.kind = NUE_NATIVE_X11_WINDOW;
        s->native.window = uint64_t(SDL_GetNumberProperty(p, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        s->native.display = uint64_t(reinterpret_cast<uintptr_t>(SDL_GetPointerProperty(p, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)));
    }   // the dummy/offscreen drivers have no native window: kind stays NONE
#endif
    return NUE_OK;
}

namespace {

nue_result v_window_create(const nue_window_desc* desc, nue_window_id id, void** out_impl, nue_window_snapshot* out_snapshot) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, desc->title ? desc->title : "NUE");
    bool borderless_mode = desc->mode == NUE_WINDOW_MODE_BORDERLESS;
    bool exclusive = desc->mode == NUE_WINDOW_MODE_EXCLUSIVE;
    if (exclusive && compositor_owns_modes()) { exclusive = false; borderless_mode = true; }   // gamescope / Deck: stay borderless, the compositor scales
    int width = desc->width > 0 ? desc->width : 1280, height = desc->height > 0 ? desc->height : 720;
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, desc->x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, desc->y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, (desc->flags & NUE_WINDOW_RESIZABLE) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, (desc->flags & NUE_WINDOW_HIDDEN) != 0 || g.init.headless);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, (desc->flags & NUE_WINDOW_BORDERLESS) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, (desc->flags & NUE_WINDOW_ALWAYS_ON_TOP) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, (desc->flags & NUE_WINDOW_HIGH_DPI) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN, (desc->flags & NUE_WINDOW_MAXIMIZED) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MINIMIZED_BOOLEAN, (desc->flags & NUE_WINDOW_MINIMIZED) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN, (desc->flags & NUE_WINDOW_UTILITY) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, (desc->flags & NUE_WINDOW_GFX_VULKAN) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN, (desc->flags & NUE_WINDOW_GFX_METAL) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, (desc->flags & NUE_WINDOW_GFX_OPENGL) != 0);
    if (borderless_mode) SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
    const bool attach = desc->attach && desc->attach->kind != NUE_NATIVE_NONE;
    if (attach) {
        switch (desc->attach->kind) {
#if defined(SDL_PLATFORM_WINDOWS)
            case NUE_NATIVE_WIN32_HWND: SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, reinterpret_cast<void*>(uintptr_t(desc->attach->window))); break;
#endif
#if defined(SDL_PLATFORM_LINUX)
            case NUE_NATIVE_X11_WINDOW: SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER, Sint64(desc->attach->window)); break;
#endif
            default:
                SDL_DestroyProperties(props);
                return nuei::fail(NUE_ERR_UNSUPPORTED, "attaching a native window of kind %d is not supported on this platform", int(desc->attach->kind));
        }
    }
    SDL_Window* win = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!win) return sdl_fail("SDL_CreateWindow");
    Win* w = new Win();
    w->win = win;
    w->id = id;
    w->attached = attach;
    w->mode = exclusive ? NUE_WINDOW_MODE_EXCLUSIVE : borderless_mode ? NUE_WINDOW_MODE_BORDERLESS : NUE_WINDOW_MODE_WINDOWED;
    if (exclusive) {
        SDL_DisplayID disp = desc->display ? desc->display : SDL_GetDisplayForWindow(win);
        SDL_DisplayMode mode;
        bool have = false;
        if (desc->fullscreen_mode.width > 0 && desc->fullscreen_mode.height > 0)
            have = SDL_GetClosestFullscreenDisplayMode(disp, desc->fullscreen_mode.width, desc->fullscreen_mode.height, desc->fullscreen_mode.refresh_rate_hz, true, &mode);
        if (have) SDL_SetWindowFullscreenMode(win, &mode); else SDL_SetWindowFullscreenMode(win, nullptr);
        SDL_SetWindowFullscreen(win, true);
    }
    if (desc->min_width || desc->min_height) SDL_SetWindowMinimumSize(win, int(desc->min_width), int(desc->min_height));
    if (desc->max_width || desc->max_height) SDL_SetWindowMaximumSize(win, int(desc->max_width), int(desc->max_height));
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.by_sdl[SDL_GetWindowID(win)] = w;
        g.by_nue[id] = w;
    }
    *out_impl = w;
    sdl_window_refresh(w, out_snapshot);
    return NUE_OK;
}

void v_window_destroy(void* impl) {
    Win* w = static_cast<Win*>(impl);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.by_sdl.erase(SDL_GetWindowID(w->win));
        g.by_nue.erase(w->id);
    }
    if (w->relative) SDL_SetWindowRelativeMouseMode(w->win, false);
    SDL_SetWindowMouseRect(w->win, nullptr);
    SDL_SetWindowMouseGrab(w->win, false);   // the pointer is never left captured
    if (!w->attached) SDL_DestroyWindow(w->win);
    delete w;
}

nue_result v_window_refresh(void* impl, nue_window_snapshot* s) { return sdl_window_refresh(impl, s); }
Win* W(void* impl) { return static_cast<Win*>(impl); }

nue_result v_set_title(void* impl, const char* t) { return SDL_SetWindowTitle(W(impl)->win, t) ? NUE_OK : sdl_fail("SDL_SetWindowTitle"); }
nue_result v_get_title(void* impl, char* buffer, size_t cap, size_t* out) { const char* t = SDL_GetWindowTitle(W(impl)->win); return nuei::copy_out_string(t ? t : "", std::strlen(t ? t : ""), buffer, cap, out); }
nue_result v_set_size(void* impl, int32_t w, int32_t h) {
    if (W(impl)->attached) return NUE_ERR_UNSUPPORTED;
    return SDL_SetWindowSize(W(impl)->win, w, h) ? NUE_OK : sdl_fail("SDL_SetWindowSize");
}
nue_result v_set_position(void* impl, int32_t x, int32_t y) { return SDL_SetWindowPosition(W(impl)->win, x, y) ? NUE_OK : sdl_fail("SDL_SetWindowPosition"); }
nue_result v_set_limits(void* impl, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    SDL_SetWindowMinimumSize(W(impl)->win, int(a), int(b));
    SDL_SetWindowMaximumSize(W(impl)->win, int(c), int(d));
    return NUE_OK;
}
nue_result v_set_flag(void* impl, nue_window_flags flag, nue_bool value) {
    SDL_Window* win = W(impl)->win;
    switch (flag) {
        case NUE_WINDOW_RESIZABLE: return SDL_SetWindowResizable(win, value != 0) ? NUE_OK : sdl_fail("SDL_SetWindowResizable");
        case NUE_WINDOW_BORDERLESS: return SDL_SetWindowBordered(win, value == 0) ? NUE_OK : sdl_fail("SDL_SetWindowBordered");
        case NUE_WINDOW_ALWAYS_ON_TOP: return SDL_SetWindowAlwaysOnTop(win, value != 0) ? NUE_OK : sdl_fail("SDL_SetWindowAlwaysOnTop");
        default: return NUE_ERR_UNSUPPORTED;
    }
}
nue_result v_show(void* impl, nue_bool show) { return (show ? SDL_ShowWindow(W(impl)->win) : SDL_HideWindow(W(impl)->win)) ? NUE_OK : sdl_fail("SDL_ShowWindow"); }
nue_result v_raise(void* impl) { return SDL_RaiseWindow(W(impl)->win) ? NUE_OK : sdl_fail("SDL_RaiseWindow"); }
nue_result v_minimize(void* impl) { return SDL_MinimizeWindow(W(impl)->win) ? NUE_OK : sdl_fail("SDL_MinimizeWindow"); }
nue_result v_maximize(void* impl) { return SDL_MaximizeWindow(W(impl)->win) ? NUE_OK : sdl_fail("SDL_MaximizeWindow"); }
nue_result v_restore(void* impl) { return SDL_RestoreWindow(W(impl)->win) ? NUE_OK : sdl_fail("SDL_RestoreWindow"); }
nue_result v_flash(void* impl, nue_bool until_focused) { return SDL_FlashWindow(W(impl)->win, until_focused ? SDL_FLASH_UNTIL_FOCUSED : SDL_FLASH_BRIEFLY) ? NUE_OK : sdl_fail("SDL_FlashWindow"); }

nue_result v_set_mode(void* impl, nue_window_mode mode, const nue_display_mode* m) {
    Win* w = W(impl);
    NUE_LOGD("sdl3", "window %u: set mode %d (%s, called from the %s thread)", unsigned(w->id), int(mode), w->attached ? "attached window: refused" : "own window",
             nue_is_event_thread() ? "event" : "a non-event");
    if (w->attached) return NUE_ERR_UNSUPPORTED;
    if (mode == NUE_WINDOW_MODE_EXCLUSIVE && compositor_owns_modes()) mode = NUE_WINDOW_MODE_BORDERLESS;
    if (mode == NUE_WINDOW_MODE_WINDOWED) { w->mode = mode; return SDL_SetWindowFullscreen(w->win, false) ? NUE_OK_PENDING : sdl_fail("SDL_SetWindowFullscreen"); }
    if (mode == NUE_WINDOW_MODE_BORDERLESS) SDL_SetWindowFullscreenMode(w->win, nullptr);
    else {
        SDL_DisplayMode dm;
        SDL_DisplayID disp = SDL_GetDisplayForWindow(w->win);
        if (m && m->width > 0 && SDL_GetClosestFullscreenDisplayMode(disp, m->width, m->height, m->refresh_rate_hz, true, &dm)) SDL_SetWindowFullscreenMode(w->win, &dm);
        else SDL_SetWindowFullscreenMode(w->win, nullptr);
    }
    w->mode = mode;
    return SDL_SetWindowFullscreen(w->win, true) ? NUE_OK_PENDING : sdl_fail("SDL_SetWindowFullscreen");
}

nue_result v_move_display(void* impl, nue_display_id d) {
    SDL_Rect r;
    if (!SDL_GetDisplayBounds(d, &r)) return NUE_ERR_NOT_FOUND;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(W(impl)->win, &ww, &wh);
    return SDL_SetWindowPosition(W(impl)->win, r.x + std::max(0, (r.w - ww) / 2), r.y + std::max(0, (r.h - wh) / 2)) ? NUE_OK : sdl_fail("SDL_SetWindowPosition");
}

nue_result v_clip(void* impl, const nue_rect_i* rect, nue_bool enabled) {
    SDL_Window* win = W(impl)->win;
    if (!enabled) { SDL_SetWindowMouseRect(win, nullptr); return SDL_SetWindowMouseGrab(win, false) ? NUE_OK : sdl_fail("SDL_SetWindowMouseGrab"); }
    if (rect) { SDL_Rect r = {rect->x, rect->y, rect->w, rect->h}; SDL_SetWindowMouseRect(win, &r); }
    else SDL_SetWindowMouseRect(win, nullptr);
    return SDL_SetWindowMouseGrab(win, true) ? NUE_OK : sdl_fail("SDL_SetWindowMouseGrab");
}

nue_result v_relative(void* impl, nue_bool enabled) {
    if (!SDL_SetWindowRelativeMouseMode(W(impl)->win, enabled != 0)) return sdl_fail("SDL_SetWindowRelativeMouseMode");
    W(impl)->relative = enabled != 0;
    return NUE_OK;
}

nue_result v_warp(void* impl, float x, float y) { SDL_WarpMouseInWindow(W(impl)->win, x, y); return NUE_OK; }

nue_result v_text_input(void* impl, nue_bool start, const nue_rect_i* area, int32_t cursor) {
    SDL_Window* win = W(impl)->win;
    if (!start) return SDL_StopTextInput(win) ? NUE_OK : sdl_fail("SDL_StopTextInput");
    if (!SDL_TextInputActive(win) && !SDL_StartTextInput(win)) return sdl_fail("SDL_StartTextInput");
    if (area) { SDL_Rect r = {area->x, area->y, area->w, area->h}; SDL_SetTextInputArea(win, &r, cursor); }
    return NUE_OK;
}

nue_result v_cursor_visible(nue_bool visible) { return (visible ? SDL_ShowCursor() : SDL_HideCursor()) ? NUE_OK : sdl_fail("SDL_ShowCursor"); }

nue_result v_cursor_kind(nue_cursor_kind kind) {
    SDL_SystemCursor sc = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (kind) {
        case NUE_CURSOR_TEXT: sc = SDL_SYSTEM_CURSOR_TEXT; break;
        case NUE_CURSOR_WAIT: sc = SDL_SYSTEM_CURSOR_WAIT; break;
        case NUE_CURSOR_CROSSHAIR: sc = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
        case NUE_CURSOR_HAND: sc = SDL_SYSTEM_CURSOR_POINTER; break;
        case NUE_CURSOR_MOVE: sc = SDL_SYSTEM_CURSOR_MOVE; break;
        case NUE_CURSOR_RESIZE_NS: sc = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
        case NUE_CURSOR_RESIZE_EW: sc = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
        case NUE_CURSOR_RESIZE_NESW: sc = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
        case NUE_CURSOR_RESIZE_NWSE: sc = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
        case NUE_CURSOR_NOT_ALLOWED: sc = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
        default: break;
    }
    SDL_Cursor* c = SDL_CreateSystemCursor(sc);
    if (!c) return sdl_fail("SDL_CreateSystemCursor");
    SDL_SetCursor(c);
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.cursor) SDL_DestroyCursor(g.cursor);
    g.cursor = c;
    return NUE_OK;
}

const nue_backend_video g_video = {
    v_display_enum, v_display_modes, v_window_create, v_window_destroy, v_window_refresh, v_set_title, v_get_title, v_set_size, v_set_position,
    v_set_limits, v_set_flag, v_show, v_raise, v_minimize, v_maximize, v_restore, v_flash, v_set_mode, v_move_display, v_clip, v_relative, v_warp,
    v_text_input, v_cursor_visible, v_cursor_kind};

// ---- input -------------------------------------------------------------------------------------------------------------------------------
nue_result i_gamepad_enum(nue_gamepad_info* infos, uint32_t capacity, uint32_t* out_count) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    *out_count = ids ? uint32_t(count) : 0;
    for (int i = 0; ids && infos && uint32_t(i) < capacity && i < count; ++i) {
        SDL_Gamepad* p = pad_of(nue_gamepad_id(ids[i]));
        nue_gamepad_info& d = infos[i];
        NUE_STRUCT_INIT(&d);
        d.id = nue_gamepad_id(ids[i]);
        if (!p) continue;
        const char* name = SDL_GetGamepadName(p);
        std::snprintf(d.name, sizeof(d.name), "%s", name ? name : "gamepad");
        SDL_GUID guid = SDL_GetGamepadGUIDForID(ids[i]);
        std::memcpy(d.guid.data, guid.data, 16);
        d.vendor_id = SDL_GetGamepadVendor(p); d.product_id = SDL_GetGamepadProduct(p); d.product_version = SDL_GetGamepadProductVersion(p);
        d.type = conv_pad_type(SDL_GetGamepadType(p));
        d.glyphs = glyphs_of(d.type);
        d.caps = NUE_PADCAP_HAS_MAPPING;
        const SDL_PropertiesID props = SDL_GetGamepadProperties(p);
        if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) d.caps |= NUE_PADCAP_RUMBLE;
        if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false)) d.caps |= NUE_PADCAP_TRIGGER_RUMBLE;
        if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false)) d.caps |= NUE_PADCAP_LED;
        if (SDL_GetNumGamepadTouchpads(p) > 0) d.caps |= NUE_PADCAP_TOUCHPAD;
        if (SDL_GamepadHasSensor(p, SDL_SENSOR_GYRO)) d.caps |= NUE_PADCAP_GYRO;
        if (SDL_GamepadHasSensor(p, SDL_SENSOR_ACCEL)) d.caps |= NUE_PADCAP_ACCEL;
        d.player_index = SDL_GetGamepadPlayerIndex(p);
        int percent = -1;
        SDL_PowerState ps = SDL_GetGamepadPowerInfo(p, &percent);
        d.battery_percent = percent;
        d.power_state = ps == SDL_POWERSTATE_ON_BATTERY ? NUE_PAD_POWER_ON_BATTERY : ps == SDL_POWERSTATE_CHARGING ? NUE_PAD_POWER_CHARGING
                       : ps == SDL_POWERSTATE_CHARGED ? NUE_PAD_POWER_FULL : ps == SDL_POWERSTATE_NO_BATTERY ? NUE_PAD_POWER_WIRED : NUE_PAD_POWER_UNKNOWN;
        SDL_Joystick* js = SDL_GetGamepadJoystick(p);
        if (js) { d.joystick_axes = uint32_t(std::max(0, SDL_GetNumJoystickAxes(js))); d.joystick_buttons = uint32_t(std::max(0, SDL_GetNumJoystickButtons(js))); d.joystick_hats = uint32_t(std::max(0, SDL_GetNumJoystickHats(js))); }
        d.connected = SDL_GamepadConnected(p) ? NUE_TRUE : NUE_FALSE;
    }
    SDL_free(ids);
    return NUE_OK;
}

nue_result i_gamepad_state(nue_gamepad_id id, nue_gamepad_state* out) {
    SDL_Gamepad* p = pad_of(id);
    if (!p) return NUE_ERR_NOT_FOUND;
    NUE_STRUCT_INIT(out);
    for (int s = 0; s < SDL_GAMEPAD_BUTTON_COUNT; ++s) {
        int b = nue_button_from_sdl(s);
        if (b >= 0 && b < 32 && SDL_GetGamepadButton(p, SDL_GamepadButton(s))) out->buttons |= 1u << b;
    }
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) { int n = nue_axis_from_sdl(a); if (n >= 0) out->axes[n] = SDL_GetGamepadAxis(p, SDL_GamepadAxis(a)); }
    out->timestamp_ns = int64_t(SDL_GetTicksNS());
    return NUE_OK;
}

nue_result i_rumble(nue_gamepad_id id, uint16_t low, uint16_t high, uint32_t ms) { SDL_Gamepad* p = pad_of(id); if (!p) return NUE_ERR_NOT_FOUND; return SDL_RumbleGamepad(p, low, high, ms) ? NUE_OK : NUE_ERR_UNSUPPORTED; }
nue_result i_rumble_triggers(nue_gamepad_id id, uint16_t l, uint16_t r, uint32_t ms) { SDL_Gamepad* p = pad_of(id); if (!p) return NUE_ERR_NOT_FOUND; return SDL_RumbleGamepadTriggers(p, l, r, ms) ? NUE_OK : NUE_ERR_UNSUPPORTED; }
nue_result i_led(nue_gamepad_id id, uint8_t r, uint8_t gr, uint8_t b) { SDL_Gamepad* p = pad_of(id); if (!p) return NUE_ERR_NOT_FOUND; return SDL_SetGamepadLED(p, r, gr, b) ? NUE_OK : NUE_ERR_UNSUPPORTED; }

SDL_SensorType sdl_sensor(uint32_t kind) { return kind == NUE_SENSOR_GYROSCOPE ? SDL_SENSOR_GYRO : kind == NUE_SENSOR_ACCELEROMETER ? SDL_SENSOR_ACCEL : SDL_SENSOR_UNKNOWN; }

nue_result i_pad_sensor_enable(nue_gamepad_id id, uint32_t kind, nue_bool enabled) {
    SDL_Gamepad* p = pad_of(id);
    if (!p) return NUE_ERR_NOT_FOUND;
    SDL_SensorType t = sdl_sensor(kind);
    if (t == SDL_SENSOR_UNKNOWN || !SDL_GamepadHasSensor(p, t)) return NUE_ERR_UNSUPPORTED;
    return SDL_SetGamepadSensorEnabled(p, t, enabled != 0) ? NUE_OK : sdl_fail("SDL_SetGamepadSensorEnabled");
}

nue_result i_pad_sensor_get(nue_gamepad_id id, uint32_t kind, float out[3]) {
    SDL_Gamepad* p = pad_of(id);
    if (!p) return NUE_ERR_NOT_FOUND;
    SDL_SensorType t = sdl_sensor(kind);
    if (t == SDL_SENSOR_UNKNOWN) return NUE_ERR_UNSUPPORTED;
    return SDL_GetGamepadSensorData(p, t, out, 3) ? NUE_OK : NUE_ERR_UNSUPPORTED;
}

nue_result i_add_mapping(const char* line) { int r = SDL_AddGamepadMapping(line); return r < 0 ? sdl_fail("SDL_AddGamepadMapping") : NUE_OK; }

nue_result i_get_mapping(nue_gamepad_id id, char* buffer, size_t cap, size_t* out) {
    SDL_Gamepad* p = pad_of(id);
    if (!p) return NUE_ERR_NOT_FOUND;
    char* m = SDL_GetGamepadMapping(p);
    nue_result r = nuei::copy_out_string(m ? m : "", std::strlen(m ? m : ""), buffer, cap, out);
    SDL_free(m);
    return r;
}

nue_result i_set_player(nue_gamepad_id id, int32_t index) { SDL_Gamepad* p = pad_of(id); if (!p) return NUE_ERR_NOT_FOUND; return SDL_SetGamepadPlayerIndex(p, index) ? NUE_OK : NUE_ERR_UNSUPPORTED; }

SDL_Joystick* joy_of(nue_gamepad_id id) { SDL_Gamepad* p = pad_of(id); return p ? SDL_GetGamepadJoystick(p) : nullptr; }
int16_t i_joy_axis(nue_gamepad_id id, uint32_t axis) { SDL_Joystick* j = joy_of(id); return j ? SDL_GetJoystickAxis(j, int(axis)) : 0; }
nue_bool i_joy_button(nue_gamepad_id id, uint32_t b) { SDL_Joystick* j = joy_of(id); return j && SDL_GetJoystickButton(j, int(b)) ? NUE_TRUE : NUE_FALSE; }
uint8_t i_joy_hat(nue_gamepad_id id, uint32_t h) { SDL_Joystick* j = joy_of(id); return j ? SDL_GetJoystickHat(j, int(h)) : 0; }

// device sensors (phones, tablets, laptops)
SDL_Sensor* open_sensor(nue_sensor_kind kind) {
    std::lock_guard<std::mutex> lock(g.mutex);
    auto it = g.sensors.find(int(kind));
    if (it != g.sensors.end()) return it->second;
    SDL_SensorType want = sdl_sensor(uint32_t(kind));
    if (want == SDL_SENSOR_UNKNOWN) return nullptr;
    int count = 0;
    SDL_SensorID* ids = SDL_GetSensors(&count);
    SDL_Sensor* found = nullptr;
    for (int i = 0; ids && i < count && !found; ++i) if (SDL_GetSensorTypeForID(ids[i]) == want) found = SDL_OpenSensor(ids[i]);
    SDL_free(ids);
    if (found) g.sensors[int(kind)] = found;
    return found;
}
nue_bool i_sensor_available(nue_sensor_kind kind) {
    SDL_SensorType want = sdl_sensor(uint32_t(kind));
    if (want == SDL_SENSOR_UNKNOWN) return NUE_FALSE;
    int count = 0;
    SDL_SensorID* ids = SDL_GetSensors(&count);
    bool ok = false;
    for (int i = 0; ids && i < count && !ok; ++i) ok = SDL_GetSensorTypeForID(ids[i]) == want;
    SDL_free(ids);
    return ok ? NUE_TRUE : NUE_FALSE;
}
nue_result i_sensor_enable(nue_sensor_kind kind, nue_bool enabled, float) {
    if (enabled) return open_sensor(kind) ? NUE_OK : NUE_ERR_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(g.mutex);
    auto it = g.sensors.find(int(kind));
    if (it != g.sensors.end()) { SDL_CloseSensor(it->second); g.sensors.erase(it); }
    return NUE_OK;
}
nue_result i_sensor_get(nue_sensor_kind kind, float out[3], int64_t* ts) {
    SDL_Sensor* s;
    { std::lock_guard<std::mutex> lock(g.mutex); auto it = g.sensors.find(int(kind)); s = it == g.sensors.end() ? nullptr : it->second; }
    if (!s) return NUE_ERR_BAD_STATE;
    if (!SDL_GetSensorData(s, out, 3)) return sdl_fail("SDL_GetSensorData");
    if (ts) *ts = int64_t(SDL_GetTicksNS());
    return NUE_OK;
}
nue_bool i_touch_present() { int n = 0; SDL_TouchID* t = SDL_GetTouchDevices(&n); SDL_free(t); return n > 0 ? NUE_TRUE : NUE_FALSE; }
nue_bool i_mouse_present() { return SDL_HasMouse() ? NUE_TRUE : NUE_FALSE; }
nue_bool i_screen_keyboard() { return SDL_HasScreenKeyboardSupport() ? NUE_TRUE : NUE_FALSE; }

const nue_backend_input g_input = {
    i_gamepad_enum, i_gamepad_state, i_rumble, i_rumble_triggers, i_led, i_pad_sensor_enable, i_pad_sensor_get, i_add_mapping, i_get_mapping, i_set_player,
    i_joy_axis, i_joy_button, i_joy_hat, i_sensor_available, i_sensor_enable, i_sensor_get, nullptr, nullptr, i_touch_present, i_mouse_present, i_screen_keyboard};

// ---- audio -------------------------------------------------------------------------------------------------------------------------------
struct AudioDev {
    SDL_AudioStream* stream = nullptr;
    nue_backend_device_open open;
    nue_audio_spec spec;
    uint32_t frame_size = 0;
    uint32_t buffer_frames = 0;
    std::vector<uint8_t> scratch;
};

void SDLCALL audio_cb(void* userdata, SDL_AudioStream* stream, int additional, int) {
    AudioDev* d = static_cast<AudioDev*>(userdata);
    if (additional <= 0 || !d->open.callback) return;
    // SDL asks for `additional` bytes in OUR format (it converts to the device format itself)
    uint32_t frames = uint32_t(additional) / d->frame_size;
    if (!frames) return;
    size_t bytes = size_t(frames) * d->frame_size;
    if (d->scratch.size() < bytes) d->scratch.resize(bytes);   // normally pre-sized in open(): grows only if SDL asks for more than a few buffers
    std::memset(d->scratch.data(), d->spec.format == NUE_AUDIO_U8 ? 128 : 0, bytes);
    d->open.callback(d->open.userdata, d->scratch.data(), frames);
    SDL_PutAudioStreamData(stream, d->scratch.data(), int(bytes));
}

nue_result a_list(nue_bool capture, nue_audio_device_info* infos, uint32_t capacity, uint32_t* out_count) {
    int count = 0;
    SDL_AudioDeviceID* ids = capture ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
    *out_count = ids ? uint32_t(count) : 0;
    for (int i = 0; ids && infos && uint32_t(i) < capacity && i < count; ++i) {
        nue_audio_device_info& d = infos[i];
        NUE_STRUCT_INIT(&d);
        d.id = uint32_t(ids[i]);
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        std::snprintf(d.name, sizeof(d.name), "%s", name ? name : "audio device");
        d.is_capture = capture;
        d.is_default = i == 0 ? NUE_TRUE : NUE_FALSE;   // SDL lists the system default first
        SDL_AudioSpec spec;
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(ids[i], &spec, &frames)) {
            d.preferred.format = spec.format == SDL_AUDIO_S16 ? NUE_AUDIO_S16 : NUE_AUDIO_F32;
            d.preferred.channels = uint32_t(spec.channels);
            d.preferred.sample_rate = uint32_t(spec.freq);
            d.preferred_buffer_frames = uint32_t(frames);
        }
    }
    SDL_free(ids);
    return NUE_OK;
}

nue_result a_open(const nue_backend_device_open* open, void** out_device, nue_backend_device_info* info) {
    if (open->capture) return nuei::fail(NUE_ERR_UNSUPPORTED, "capture streams are not implemented yet");
    AudioDev* d = new AudioDev();
    d->open = *open;
    d->spec = open->spec;
    d->frame_size = nue_audio_frame_size(&d->spec);
    SDL_AudioSpec want;
    SDL_zero(want);
    want.format = open->spec.format == NUE_AUDIO_S16 ? SDL_AUDIO_S16 : open->spec.format == NUE_AUDIO_U8 ? SDL_AUDIO_U8 : SDL_AUDIO_F32;
    want.channels = int(open->spec.channels);
    want.freq = int(open->spec.sample_rate);
    if (open->buffer_frames) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", open->buffer_frames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, buf);
    }
    if (open->stream_name) SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "game");
    SDL_AudioDeviceID dev = open->device ? SDL_AudioDeviceID(open->device) : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    d->stream = SDL_OpenAudioDeviceStream(dev, &want, audio_cb, d);
    if (!d->stream) { delete d; return sdl_fail("SDL_OpenAudioDeviceStream"); }
    SDL_AudioSpec devspec;
    int frames = 0;
    SDL_AudioDeviceID phys = SDL_GetAudioStreamDevice(d->stream);
    if (SDL_GetAudioDeviceFormat(phys, &devspec, &frames)) d->buffer_frames = uint32_t(std::max(frames, 1));
    else d->buffer_frames = open->buffer_frames ? open->buffer_frames : 512;
    d->scratch.resize(size_t(d->buffer_frames) * d->frame_size * 4);
    info->actual = open->spec;   // SDL converts between our format and the device's: the stream sees exactly what it asked for
    info->buffer_frames = d->buffer_frames;
    info->latency_ns = uint64_t(d->buffer_frames) * 2ull * 1000000000ull / std::max<uint32_t>(1, uint32_t(devspec.freq ? devspec.freq : want.freq));
    SDL_ResumeAudioStreamDevice(d->stream);
    *out_device = d;
    return NUE_OK;
}

void a_close(void* dev) {
    AudioDev* d = static_cast<AudioDev*>(dev);
    SDL_DestroyAudioStream(d->stream);   // closes the device it opened; the callback is not running afterwards
    delete d;
}

nue_result a_pause(void* dev, nue_bool paused) {
    AudioDev* d = static_cast<AudioDev*>(dev);
    return (paused ? SDL_PauseAudioStreamDevice(d->stream) : SDL_ResumeAudioStreamDevice(d->stream)) ? NUE_OK : sdl_fail("SDL_PauseAudioStreamDevice");
}

nue_result a_info(void* dev, nue_backend_device_info* info) {
    AudioDev* d = static_cast<AudioDev*>(dev);
    info->actual = d->spec;
    info->buffer_frames = d->buffer_frames;
    info->latency_ns = uint64_t(d->buffer_frames) * 2ull * 1000000000ull / std::max<uint32_t>(1, d->spec.sample_rate);
    return NUE_OK;
}

const nue_backend_audio g_audio = {a_list, a_open, a_close, a_pause, a_info, nullptr};

nue_backend g_backend = {};

void fill_backend() {
    std::memset(&g_backend, 0, sizeof(g_backend));
    g_backend.struct_size = sizeof(g_backend);
    g_backend.name = "sdl3";
    g_backend.init = b_init;
    g_backend.shutdown = b_shutdown;
    g_backend.pump = b_pump;
    g_backend.wait = b_wait;
    g_backend.wake = b_wake;
    g_backend.video = &g_video;
    g_backend.input = &g_input;
    g_backend.audio = &g_audio;
    const nue_capability caps[] = {NUE_CAP_WINDOWS, NUE_CAP_MULTIPLE_WINDOWS, NUE_CAP_EXCLUSIVE_FULLSCREEN, NUE_CAP_DISPLAY_MODE_CHANGE, NUE_CAP_HIGH_DPI, NUE_CAP_SAFE_AREA,
                                   NUE_CAP_CURSOR_CLIP, NUE_CAP_RELATIVE_MOUSE, NUE_CAP_KEYBOARD, NUE_CAP_MOUSE, NUE_CAP_GAMEPAD, NUE_CAP_GAMEPAD_RUMBLE, NUE_CAP_GAMEPAD_LED,
                                   NUE_CAP_GAMEPAD_SENSORS, NUE_CAP_AUDIO_OUTPUT, NUE_CAP_AUDIO_DEVICE_SELECTION, NUE_CAP_ACCELEROMETER, NUE_CAP_GYROSCOPE};
    for (nue_capability c : caps) g_backend.capabilities[c / 64] |= 1ull << (c % 64);
#if defined(SDL_PLATFORM_WINDOWS) || defined(SDL_PLATFORM_LINUX)
    g_backend.capabilities[NUE_CAP_ATTACH_NATIVE_WINDOW / 64] |= 1ull << (NUE_CAP_ATTACH_NATIVE_WINDOW % 64);
#endif
}

}  // namespace

const nue_backend* nue_backend_sdl3_get(void) {
    static bool done = false;
    if (!done) { fill_backend(); done = true; }
    return &g_backend;
}
