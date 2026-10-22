#include "input_classify.hpp"

#include <algorithm>
#include <cmath>

namespace nue::android {

namespace {
bool has(uint32_t sources, uint32_t mask) { return (sources & mask) == mask; }
} // namespace

InputKind classify_sources(uint32_t s, bool is_virtual, int keyboard_type) {
    // Order matters: more specific sources first.
    if (has(s, src::kGamepad) || has(s, src::kJoystick)) return is_virtual ? InputKind::Unknown : InputKind::Gamepad;
    if (has(s, src::kStylus)) return InputKind::Stylus;
    if (has(s, src::kTouchscreen)) return InputKind::Touch;
    if (has(s, src::kMouse) || has(s, src::kMouseRelative)) return InputKind::Mouse;
    if (has(s, src::kTouchpad)) return InputKind::Touchpad;
    if (has(s, src::kKeyboard) && !is_virtual && keyboard_type == 2) return InputKind::Keyboard;
    return InputKind::Unknown;
}

PadButton pad_button_from_keycode(int k) {
    switch (k) {
    case 96: return PadButton::A;               // KEYCODE_BUTTON_A
    case 97: return PadButton::B;               // BUTTON_B
    case 99: return PadButton::X;               // BUTTON_X
    case 100: return PadButton::Y;              // BUTTON_Y
    case 102: return PadButton::LeftShoulder;   // BUTTON_L1
    case 103: return PadButton::RightShoulder;  // BUTTON_R1
    case 104: return PadButton::LeftTrigger;    // BUTTON_L2 (digital trigger)
    case 105: return PadButton::RightTrigger;   // BUTTON_R2
    case 106: return PadButton::LeftStick;      // BUTTON_THUMBL
    case 107: return PadButton::RightStick;     // BUTTON_THUMBR
    case 108: return PadButton::Start;          // BUTTON_START
    case 109: return PadButton::Back;           // BUTTON_SELECT
    case 110: return PadButton::Guide;          // BUTTON_MODE
    case 19: return PadButton::DpadUp;          // KEYCODE_DPAD_UP
    case 20: return PadButton::DpadDown;
    case 21: return PadButton::DpadLeft;
    case 22: return PadButton::DpadRight;
    default: return PadButton::None;
    }
}

PadAxis pad_axis_from_android_axis(int a, bool xbox_right_stick) {
    switch (a) {
    case 0: return PadAxis::LeftX;               // AXIS_X
    case 1: return PadAxis::LeftY;               // AXIS_Y
    case 11: return xbox_right_stick ? PadAxis::RightX : PadAxis::None; // AXIS_Z
    case 14: return xbox_right_stick ? PadAxis::RightY : PadAxis::None; // AXIS_RZ
    case 12: return xbox_right_stick ? PadAxis::None : PadAxis::RightX; // AXIS_RX
    case 13: return xbox_right_stick ? PadAxis::None : PadAxis::RightY; // AXIS_RY
    case 17:
    case 23: return PadAxis::LeftTrigger;        // AXIS_LTRIGGER / AXIS_BRAKE
    case 18:
    case 22: return PadAxis::RightTrigger;       // AXIS_RTRIGGER / AXIS_GAS
    case 15: return PadAxis::HatX;               // AXIS_HAT_X
    case 16: return PadAxis::HatY;               // AXIS_HAT_Y
    default: return PadAxis::None;
    }
}

bool uses_xbox_right_stick(bool z, bool rz, bool rx, bool ry) { return z && rz && !(rx && ry); }

StickValue apply_radial_deadzone(float x, float y, float dz) {
    float mag = std::sqrt(x * x + y * y);
    if (mag <= dz || dz >= 1.0f) return {0, 0};
    float scaled = std::min(1.0f, (mag - dz) / (1.0f - dz));
    return {x / mag * scaled, y / mag * scaled};
}

bool LastInputTracker::note(const InputSample& s, int64_t now_ms) {
    if (s.kind == InputKind::Unknown || s.magnitude < threshold_) return false;
    if (s.kind == last_) return false;
    if (now_ms - last_switch_ms_ < hold_ms_) return false;
    last_ = s.kind;
    last_switch_ms_ = now_ms;
    return true;
}

void LastInputTracker::device_removed(InputKind kind) {
    if (kind == last_) last_ = InputKind::Touch;
}

} // namespace nue::android
