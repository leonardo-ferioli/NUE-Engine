// Platform API: input devices. Shaped like NUE's include/nue/input.h: polled snapshots (keyboard as a table of 512 HID-usage scancodes,
// mouse position and buttons, gamepads and raw joysticks with axes/buttons/hats) plus an ordered event stream with timestamps. The DirectInput
// layer is written against this interface only: it maps HID scancodes to DIK_*, builds DIMOUSESTATE2 / DIJOYSTATE2 from the snapshots and fills
// its buffered-data rings from the events.
#pragma once
#include "nue_legacy/platform/types.hpp"
#include <functional>
#include <string>
#include <vector>

namespace nue::legacy::platform {

using GamepadId = uint32_t;                 // instance id, never reused within a run; 0 = invalid

constexpr uint32_t kScancodeCount = 512;    // USB HID usage ids (layout independent)
constexpr uint32_t kMouseLeft = 1, kMouseMiddle = 2, kMouseRight = 3, kMouseX1 = 4, kMouseX2 = 5;   // like nue_mouse_button

struct GamepadInfo {
    GamepadId id = 0;
    std::string name;
    uint16_t vendor_id = 0, product_id = 0;
    uint32_t axes = 0, buttons = 0, hats = 0;   // the raw joystick layout
    bool xbox_style = false;
    bool connected = true;
};

struct GamepadState {
    int16_t axes[8] = {};                       // -32768..32767 (up/left negative), triggers 0..32767
    uint32_t buttons = 0;                       // raw joystick buttons, bit i = button i
    uint8_t hats[4] = {};                       // bit mask 1=up 2=right 4=down 8=left
};

enum class InputEventKind : uint8_t { key, mouse_button, mouse_motion, mouse_wheel, pad_button, pad_axis, pad_hat, pad_added, pad_removed };

struct InputEvent {
    InputEventKind kind = InputEventKind::key;
    int64_t time_ns = 0;                        // monotonic, same domain as Clock::time_ns
    uint32_t code = 0;                          // scancode / mouse button / pad button, axis or hat index
    int32_t value = 0;                          // key and buttons: 1 down, 0 up; motion: delta x (y in `value2`); wheel: notches*120; axis: raw value; hat: mask
    int32_t value2 = 0;
    GamepadId pad = 0;
};

class Input {
public:
    virtual ~Input() = default;
    // Called by the application loop (or by `poll_events` below): updates the snapshots from the OS.
    virtual void pump() = 0;
    virtual const uint8_t* key_state() = 0;                                  // kScancodeCount bytes of 0/1
    virtual uint32_t mouse_state(float* x, float* y) = 0;                    // button mask (1 << (button - 1)), position in the window
    virtual uint32_t mouse_relative(float* dx, float* dy) = 0;               // motion since the last call (consumes)
    virtual bool mouse_present() = 0;
    virtual std::vector<GamepadInfo> gamepads() = 0;
    virtual bool gamepad_state(GamepadId, GamepadState& out) = 0;
    // The event stream. Every listener sees every event, in order, from the pump that produced it.
    using Listener = std::function<void(const InputEvent&)>;
    virtual uint32_t add_listener(Listener) = 0;
    virtual void remove_listener(uint32_t id) = 0;
};

}  // namespace nue::legacy::platform
