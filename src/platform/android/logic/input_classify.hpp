// Android input device / key / axis classification tables and the "last used input device" tracker.
// SDL3 already turns Android input into SDL events; this module provides what SDL does not: a stable
// device-kind classification (for UI glyph switching), the last-used-device signal, and the tables for the
// non-SDL (null / debug) path and for tests.
#pragma once
#include <cstdint>

namespace nue::android {

enum class InputKind : int32_t { Unknown = 0, Touch, Gamepad, Keyboard, Mouse, Stylus, Touchpad };

// android.view.InputDevice.SOURCE_* values (class bits are the low byte).
namespace src {
constexpr uint32_t kClassButton = 0x00000001, kClassPointer = 0x00000002, kClassTrackball = 0x00000004,
                   kClassPosition = 0x00000008, kClassJoystick = 0x00000010;
constexpr uint32_t kKeyboard = 0x00000101, kDpad = 0x00000201, kGamepad = 0x00000401, kTouchscreen = 0x00001002,
                   kMouse = 0x00002002, kStylus = 0x00004002, kTrackball = 0x00010004, kMouseRelative = 0x00020004,
                   kTouchpad = 0x00100008, kJoystick = 0x01000010;
} // namespace src

// Keyboard sources include gamepads (SOURCE_GAMEPAD has the BUTTON class) so test the specific bits in order.
// `is_virtual` devices (the on-screen keyboard, device id -1) never count as a gamepad or keyboard.
// `keyboard_type` is InputDevice.getKeyboardType(): 0 none, 1 non-alphabetic, 2 alphabetic.
InputKind classify_sources(uint32_t sources, bool is_virtual, int keyboard_type = 2);

// Gamepad buttons in the order of the Xbox layout (what the game's DirectInput action map expects).
enum class PadButton : int32_t {
    None = -1, A = 0, B, X, Y, LeftShoulder, RightShoulder, LeftTrigger, RightTrigger, Back, Start, Guide,
    LeftStick, RightStick, DpadUp, DpadDown, DpadLeft, DpadRight, Count
};
// KeyEvent.KEYCODE_* -> pad button.  KEYCODE_BACK (4) is not a pad button; it is the system back key.
PadButton pad_button_from_keycode(int keycode);

enum class PadAxis : int32_t { None = -1, LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger, HatX, HatY, Count };
// MotionEvent.AXIS_* -> pad axis.  `xbox_right_stick` selects AXIS_Z/AXIS_RZ (Xbox, PlayStation on Android) as the
// right stick; otherwise AXIS_RX/AXIS_RY.  Controllers report one of the two layouts.
PadAxis pad_axis_from_android_axis(int axis, bool xbox_right_stick);

struct StickValue {
    float x = 0, y = 0;
};
// Radial dead zone with rescaling so the output is continuous from 0 at the edge of the dead zone to 1.
StickValue apply_radial_deadzone(float x, float y, float deadzone);

// Which axis layout a device uses: true when it reports AXIS_Z / AXIS_RZ ranges but no AXIS_RX / AXIS_RY.
bool uses_xbox_right_stick(bool has_axis_z, bool has_axis_rz, bool has_axis_rx, bool has_axis_ry);

struct InputSample {
    InputKind kind = InputKind::Unknown;
    float magnitude = 0; // 0..1: stick deflection, 1 for buttons / key presses / touches, pixels/16 for mouse moves
};

class LastInputTracker {
public:
    explicit LastInputTracker(float threshold = 0.35f, int64_t switch_hold_ms = 150)
        : threshold_(threshold), hold_ms_(switch_hold_ms) {}
    // Returns true when the "last used" kind changed.  Weak signals (drifting stick, mouse jitter) never switch.
    // To stop keyboard + gamepad flapping in the same frame a switch needs hold_ms since the last switch.
    bool note(const InputSample& s, int64_t now_ms);
    InputKind last() const { return last_; }
    void device_removed(InputKind kind); // if the current kind disappears fall back to touch

private:
    float threshold_;
    int64_t hold_ms_;
    InputKind last_ = InputKind::Unknown;
    int64_t last_switch_ms_ = -1000000;
};

} // namespace nue::android
