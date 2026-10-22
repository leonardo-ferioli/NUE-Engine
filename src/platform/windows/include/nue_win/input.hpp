// Input beside SDL3: Raw Input (keyboard / mouse deltas for an HWND we do not own), XInput gamepads with rumble and battery,
// GameInput availability, last-used-device detection. SDL3 remains the default gamepad path on desktop; this module covers what it does
// not give a recompiled game that owns its HWND: relative mouse straight from the HID stack, a stable XInput view that also works while
// SDL is not initialised, and the "which device did the player touch last" signal used to switch on-screen button prompts.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

// ---- Raw Input ------------------------------------------------------------------------------------------------------------------------
struct RawMouseEvent {
    int32_t dx = 0, dy = 0;           // relative motion, in device units (not accelerated); for absolute devices (RDP, pen) the delta since the last event
    int32_t wheel = 0, hwheel = 0;    // WHEEL_DELTA multiples
    uint16_t button_down = 0, button_up = 0;   // bit i = button i (0 left, 1 right, 2 middle, 3 X1, 4 X2)
    bool absolute = false;
    bool virtual_desktop = false;
};
struct RawKeyEvent {
    uint16_t vkey = 0;                // VK_*, with VK_SHIFT/CONTROL/MENU resolved to the left/right variant
    uint16_t scancode = 0;            // set 1 scan code; extended keys have 0xE000 or'ed in
    bool down = false;
    bool extended = false;
    bool repeat = false;              // true when the key was already down (derived from the tracked state)
};
struct RawInputEvent {
    enum Kind : uint8_t { None, Mouse, Key, Hid } kind = None;
    RawMouseEvent mouse;
    RawKeyEvent key;
    void* device = nullptr;           // RAWINPUTHEADER::hDevice
};

class RawInput {
public:
    struct Options {
        bool keyboard = true;
        bool mouse = true;
        bool gamepads = false;        // HID generic desktop gamepad/joystick usages (WM_INPUT reports raw HID, not decoded here)
        bool background = false;      // RIDEV_INPUTSINK: keep receiving while the window is not focused
        bool no_legacy = false;       // RIDEV_NOLEGACY: stop WM_MOUSEMOVE/WM_KEYDOWN generation (breaks the game's own message handling! default off)
        bool capture_mouse = false;   // RIDEV_CAPTUREMOUSE (needs no_legacy)
    };
    RawInput() = default;
    ~RawInput() { unregister_all(); }
    RawInput(const RawInput&) = delete;
    RawInput& operator=(const RawInput&) = delete;
    // Registers the devices for `hwnd`. Call on the thread that owns the window.
    Status register_devices(HWND hwnd, const Options& options);
    void unregister_all();
    bool registered() const { return registered_; }
    // Decodes one WM_INPUT (lparam = HRAWINPUT). Returns false when the message could not be read.
    bool handle_wm_input(LPARAM lparam, RawInputEvent& out);
    // Decoder on already-read data (also used by the tests with synthetic RAWINPUT).
    bool decode(const RAWINPUT& raw, RawInputEvent& out);
    // Accumulates the relative motion since the last call (relative mouse mode).
    void take_mouse_motion(int32_t& dx, int32_t& dy);
private:
    bool registered_ = false;
    Options options_;
    HWND hwnd_ = nullptr;
    bool key_down_[256] = {};
    int32_t acc_dx_ = 0, acc_dy_ = 0;
    int32_t last_abs_x_ = 0, last_abs_y_ = 0;
    bool have_last_abs_ = false;
};

struct RawDeviceInfo {
    enum Type : uint8_t { Mouse, Keyboard, Hid } type = Hid;
    std::string name;                 // device interface path
    uint16_t vendor_id = 0, product_id = 0, usage_page = 0, usage = 0;
    uint32_t mouse_buttons = 0;
    uint32_t mouse_sample_rate = 0;
    bool is_gamepad_like() const { return type == Hid && usage_page == 1 && (usage == 4 || usage == 5); }
};
bool enumerate_raw_devices(std::vector<RawDeviceInfo>& out);

// ---- XInput ----------------------------------------------------------------------------------------------------------------------------
namespace xinput_buttons {
constexpr uint16_t DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008, Start = 0x0010, Back = 0x0020, LeftThumb = 0x0040, RightThumb = 0x0080,
                   LeftShoulder = 0x0100, RightShoulder = 0x0200, Guide = 0x0400, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
}
struct GamepadState {
    bool connected = false;
    uint32_t packet = 0;              // changes when the state changes
    uint16_t buttons = 0;
    float left_trigger = 0, right_trigger = 0;      // 0..1 after the trigger threshold
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;   // -1..1 after a radial dead zone, rescaled to fill 0..1
    bool any_input = false;           // anything beyond the dead zones is active (button, trigger or stick)
};
enum class BatteryKind : uint8_t { Unknown, Wired, Disposable, Alkaline, Nimh, None };
enum class BatteryLevel : uint8_t { Empty, Low, Medium, Full, Unknown };
struct GamepadBattery { BatteryKind kind = BatteryKind::Unknown; BatteryLevel level = BatteryLevel::Unknown; };

// Dead zone processing (pure; testable). Radial dead zone for sticks, linear for triggers; output is rescaled so full deflection = 1.
void apply_stick_deadzone(int raw_x, int raw_y, int deadzone, float& out_x, float& out_y);
float apply_trigger_threshold(int raw, int threshold);

class XInput {
public:
    static constexpr int MaxPads = 4;
    XInput();
    bool available() const { return get_state_ != nullptr; }
    const char* dll_name() const { return dll_; }
    bool uses_guide_button() const { return use_ex_; }   // XInputGetStateEx (ordinal 100) delivers the Guide button; undocumented but stable
    // Reads one pad. `slot` 0..3. Dead zones default to the XInput recommendations (7849 / 8689 / 30).
    bool poll(int slot, GamepadState& out);
    // Motor speeds 0..1 (low = heavy left motor, high = light right motor). XInput has no trigger motors; those need GameInput.
    Status set_rumble(int slot, float low_frequency, float high_frequency);
    void stop_all_rumble();
    bool battery(int slot, GamepadBattery& out);
    int left_deadzone = 7849, right_deadzone = 8689, trigger_threshold = 30;
    // Slots that were connected at the last poll (bit i).
    uint32_t connected_mask() const { return connected_mask_; }
    // Enumerating slots that never connected is expensive on some Windows versions (XInput probes devices): poll absent slots at most
    // every `absent_poll_interval_ms`.
    uint32_t absent_poll_interval_ms = 1000;
private:
    using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
    using SetStateFn = DWORD(WINAPI*)(DWORD, void*);
    using BatteryFn = DWORD(WINAPI*)(DWORD, BYTE, void*);
    GetStateFn get_state_ = nullptr;
    SetStateFn set_state_ = nullptr;
    BatteryFn battery_ = nullptr;
    const char* dll_ = "";
    bool use_ex_ = false;
    uint32_t connected_mask_ = 0;
    uint64_t next_absent_poll_ms_[MaxPads] = {};
};

// ---- GameInput ---------------------------------------------------------------------------------------------------------------------------
// GameInput (gameinput.dll) is the successor API (trigger rumble, Xbox One/Series features, per-device haptics). This only reports whether
// the runtime is there and what it created; the SDL3 GameInput joystick driver or a future wrapper builds on it.
struct GameInputProbe {
    bool dll_present = false;         // System32\GameInput.dll loads
    bool create_ok = false;           // GameInputCreate succeeded
    bool redistributable_present = false;   // GameInputRedist.dll (v1 runtime, installed with the Microsoft.GameInput package)
    std::string note;
};
GameInputProbe probe_game_input();

// ---- Last-used device ------------------------------------------------------------------------------------------------------------------
enum class InputDevice : uint8_t { None, KeyboardMouse, Gamepad };
const char* input_device_name(InputDevice d);
// Feed it every time a physical input arrives; it tells which family the player used last, with a hysteresis so that mouse jitter and
// stick drift near the dead zone do not flip the on-screen prompts.
class LastUsedDevice {
public:
    struct Options { int mouse_motion_threshold = 12; uint32_t switch_cooldown_ms = 250; };
    LastUsedDevice() = default;
    explicit LastUsedDevice(const Options& o) : options_(o) {}
    // `now_ms` is any monotonic clock. Each call returns true when the last-used device changed.
    bool on_key(uint64_t now_ms) { return set(InputDevice::KeyboardMouse, now_ms); }
    bool on_mouse_button(uint64_t now_ms) { return set(InputDevice::KeyboardMouse, now_ms); }
    bool on_mouse_motion(int dx, int dy, uint64_t now_ms) {
        accumulated_ += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (accumulated_ < options_.mouse_motion_threshold) return false;
        accumulated_ = 0;
        return set(InputDevice::KeyboardMouse, now_ms);
    }
    bool on_gamepad(const GamepadState& s, uint64_t now_ms) {
        const bool changed_state = s.packet != last_packet_;
        last_packet_ = s.packet;
        return (s.any_input && changed_state) ? set(InputDevice::Gamepad, now_ms) : false;
    }
    InputDevice current() const { return current_; }
private:
    bool set(InputDevice d, uint64_t now_ms) {
        if (d == current_) return false;
        if (current_ != InputDevice::None && now_ms - last_switch_ms_ < options_.switch_cooldown_ms) return false;
        current_ = d;
        last_switch_ms_ = now_ms;
        return true;
    }
    Options options_;
    InputDevice current_ = InputDevice::None;
    uint64_t last_switch_ms_ = 0;
    uint32_t last_packet_ = 0;
    int accumulated_ = 0;
};

}  // namespace nue::win
