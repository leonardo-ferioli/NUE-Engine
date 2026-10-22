#include "input_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nue::ios {

TouchPhase touch_phase_from_raw(int p) {
    switch (p) {
    case 0: return TouchPhase::down;
    case 1: return TouchPhase::move;
    case 2: return TouchPhase::stationary;
    case 3: return TouchPhase::up;
    case 4: return TouchPhase::cancel;
    case 5: return TouchPhase::hover_enter;
    case 6: return TouchPhase::hover_move;
    case 7: return TouchPhase::hover_exit;
    default: return TouchPhase::ignored;
    }
}

void normalize_point(float x, float y, float w, float h, float& nx, float& ny) {
    nx = w > 0 ? std::clamp(x / w, 0.0f, 1.0f) : 0.0f;
    ny = h > 0 ? std::clamp(y / h, 0.0f, 1.0f) : 0.0f;
}

void condition_stick(float x, float y, const PadTuning& t, float& ox, float& oy) {
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= t.stick_deadzone) { ox = oy = 0; return; }
    float scaled = (mag - t.stick_deadzone) / (t.stick_outer - t.stick_deadzone);
    scaled = std::min(scaled, 1.0f);
    ox = x / mag * scaled;
    oy = y / mag * scaled;
}

float condition_trigger(float v, const PadTuning& t) {
    if (v <= t.trigger_deadzone) return 0.0f;
    return std::min((v - t.trigger_deadzone) / (1.0f - t.trigger_deadzone), 1.0f);
}

PadState condition_pad(const PadState& raw, const PadTuning& t) {
    PadState s = raw;
    condition_stick(raw.left_x, raw.left_y, t, s.left_x, s.left_y);
    condition_stick(raw.right_x, raw.right_y, t, s.right_x, s.right_y);
    s.left_trigger = condition_trigger(raw.left_trigger, t);
    s.right_trigger = condition_trigger(raw.right_trigger, t);
    s.buttons &= ~(pad_left_trigger_digital | pad_right_trigger_digital);
    if (raw.left_trigger >= t.trigger_digital_threshold) s.buttons |= pad_left_trigger_digital;
    if (raw.right_trigger >= t.trigger_digital_threshold) s.buttons |= pad_right_trigger_digital;
    return s;
}

PadFamily pad_family_from_product_category(const char* c) {
    if (!c) return PadFamily::generic;
    // GCProductCategoryXboxOne = "Xbox One", GCProductCategoryDualShock4 = "DualShock 4", GCProductCategoryDualSense = "DualSense",
    // GCProductCategoryMFi = "MFi", GCProductCategorySwitchPro = "Switch Pro Controller" (strings as documented by Apple).
    if (std::strstr(c, "Xbox")) return PadFamily::xbox;
    if (std::strstr(c, "DualShock") || std::strstr(c, "DualSense")) return PadFamily::playstation;
    if (std::strstr(c, "Switch") || std::strstr(c, "Joy-Con")) return PadFamily::nintendo;
    if (std::strstr(c, "MFi")) return PadFamily::mfi;
    return PadFamily::generic;
}

float steering_from_gravity(float gx, float gy, float gz, int orientation, float deadzone_deg, float full_lock_deg) {
    (void)gz;
    // Wheel angle = rotation about the screen normal, measured from the pose where the UI's "down" is aligned with gravity.
    //   d = UI-down in device xy, r = UI-right in device xy; angle = atan2(g.r, g.d); clockwise rotation as seen by the player is positive.
    float dx, dy, rx, ry;
    switch (orientation) {
    case 2:  dx = 0;  dy = 1;  rx = -1; ry = 0;  break;   // portraitUpsideDown
    case 3:  dx = -1; dy = 0;  rx = 0;  ry = -1; break;   // landscapeRight (home button right)
    case 4:  dx = 1;  dy = 0;  rx = 0;  ry = 1;  break;   // landscapeLeft (home button left)
    default: dx = 0;  dy = -1; rx = 1;  ry = 0;  break;   // portrait
    }
    const float along = gx * dx + gy * dy;
    const float across = gx * rx + gy * ry;
    // Phone flat on a table: gravity is almost entirely along z, the angle is meaningless.
    if (std::sqrt(along * along + across * across) < 0.2f) return 0.0f;
    const float deg = std::atan2(across, along) * 57.29577951308232f;
    const float mag = std::fabs(deg);
    if (mag <= deadzone_deg) return 0.0f;
    float v = (mag - deadzone_deg) / (full_lock_deg - deadzone_deg);
    v = std::min(v, 1.0f);
    return deg < 0 ? -v : v;
}

bool LastUsedInput::note(InputDevice d, bool significant) {
    if (!significant || d == InputDevice::none) return false;
    last_other_ms_ = now_ms_;
    if (d == current_) return false;
    current_ = d;
    return true;
}

bool LastUsedInput::note_motion(float steering, uint32_t now_ms, uint32_t quiet_ms) {
    now_ms_ = now_ms;
    if (std::fabs(steering) < 0.25f) return false;
    if (current_ == InputDevice::motion) return false;
    if (current_ != InputDevice::none && now_ms - last_other_ms_ < quiet_ms) return false;
    current_ = InputDevice::motion;
    return true;
}

}  // namespace nue::ios
