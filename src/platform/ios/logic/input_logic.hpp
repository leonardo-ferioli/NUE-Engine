// Input math and mapping tables for the iOS backend (touch phases, gamepad conditioning, gyro steering, last-used device).
#pragma once
#include <cstdint>

namespace nue::ios {

// ---- touch ----------------------------------------------------------------------------------------------------------------------------------
enum class TouchPhase : uint8_t { down, move, stationary, up, cancel, hover_enter, hover_move, hover_exit, ignored };
// UITouchPhase raw values: began 0, moved 1, stationary 2, ended 3, cancelled 4, regionEntered 5, regionMoved 6, regionExited 7.
TouchPhase touch_phase_from_raw(int ui_touch_phase);

// Points -> normalised [0,1] window coordinates; clamps. Scale-independent so the engine does not care about @2x/@3x.
void normalize_point(float x_points, float y_points, float width_points, float height_points, float& nx, float& ny);

// ---- gamepad --------------------------------------------------------------------------------------------------------------------------------
// Engine-neutral gamepad snapshot. Buttons use the Xbox-style layout; for PlayStation/Switch pads the glue maps by position (cross = a).
enum PadButton : uint32_t {
    pad_a = 1u << 0, pad_b = 1u << 1, pad_x = 1u << 2, pad_y = 1u << 3,
    pad_left_shoulder = 1u << 4, pad_right_shoulder = 1u << 5,
    pad_dpad_up = 1u << 6, pad_dpad_down = 1u << 7, pad_dpad_left = 1u << 8, pad_dpad_right = 1u << 9,
    pad_menu = 1u << 10,      // Start
    pad_options = 1u << 11,   // Back / View / Share-side button
    pad_home = 1u << 12,
    pad_left_stick = 1u << 13, pad_right_stick = 1u << 14,
    pad_left_trigger_digital = 1u << 15, pad_right_trigger_digital = 1u << 16,
};

struct PadState {
    uint32_t buttons = 0;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;   // -1..1, y up
    float left_trigger = 0, right_trigger = 0;                // 0..1
};

struct PadTuning {
    float stick_deadzone = 0.12f;    // radial
    float stick_outer = 0.98f;       // reach full deflection a bit early (worn sticks)
    float trigger_deadzone = 0.04f;
    float trigger_digital_threshold = 0.5f;
};

// Radial deadzone with rescale so output is continuous at the edge of the deadzone.
void condition_stick(float x, float y, const PadTuning&, float& ox, float& oy);
float condition_trigger(float v, const PadTuning&);
// Applies the tuning to a raw snapshot and sets the digital trigger bits.
PadState condition_pad(const PadState& raw, const PadTuning& = {});

// GCController.vendorName / productCategory -> glyph family for on-screen hints. Strings from GCProductCategory*.
enum class PadFamily : uint8_t { generic, xbox, playstation, nintendo, mfi };
PadFamily pad_family_from_product_category(const char* product_category);

// ---- gyro steering --------------------------------------------------------------------------------------------------------------------------
// UIInterfaceOrientation raw values: portrait 1, portraitUpsideDown 2, landscapeRight 3, landscapeLeft 4.
// Gravity vector in device coordinates as delivered by CMDeviceMotion.gravity (x right, y up along the screen, z out of the screen).
// Returns steering in [-1, 1], positive = turn right (device rotated clockwise as seen by the player).
// `deadzone_deg` and `full_lock_deg`: angle of the wheel for zero and full steering.
// Signs for the two landscape orientations follow the geometry in the source and still need an on-device check (see PLATFORM_IOS.md).
float steering_from_gravity(float gx, float gy, float gz, int ui_interface_orientation, float deadzone_deg = 4.0f, float full_lock_deg = 35.0f);

// ---- last used input device -----------------------------------------------------------------------------------------------------------------
enum class InputDevice : uint8_t { none, touch, keyboard, mouse, gamepad, motion };

// Decides which device the player is "using" so the UI can swap prompts. Motion alone never takes over (the phone always moves);
// small gamepad stick noise and mouse jitter are ignored via `significant`.
class LastUsedInput {
public:
    // Returns true if the current device changed.
    bool note(InputDevice d, bool significant = true);
    // Motion only counts when no other device was used in the last `quiet_ms` and the steering is significant.
    bool note_motion(float steering, uint32_t now_ms, uint32_t quiet_ms = 2000);
    void tick(uint32_t now_ms) { now_ms_ = now_ms; }
    InputDevice current() const { return current_; }
private:
    InputDevice current_ = InputDevice::none;
    uint32_t now_ms_ = 0, last_other_ms_ = 0;
};

}  // namespace nue::ios
