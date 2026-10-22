// Display mode / refresh rate selection and safe-area math.  Pure logic; Java feeds Display.Mode lists and
// WindowInsets values, the results go back through Window.preferredDisplayModeId / Surface.setFrameRate.
#pragma once
#include <vector>

namespace nue::android {

struct DisplayMode {
    int id = 0;
    int width = 0, height = 0;
    float refresh = 60.0f;
};

struct ModeChoice {
    int mode_id = -1;          // -1: keep the current mode
    float refresh = 0;
    int present_interval = 1;  // vsyncs per game frame (1 = present every vsync)
    bool exact = false;        // refresh equals the target (within 0.5 Hz)
    bool seamless_multiple = false;
};

enum class RefreshPolicy {
    PreferExact,        // exact target, else the smallest integer multiple, else closest
    PreferHighest       // highest refresh at the current resolution (UI smoothness), frames still paced to target
};

// Only modes with the same resolution as `current` are candidates: changing resolution is a visible mode
// switch on some devices and the game does not need it.  `target_fps` is the game's frame rate (e.g. 60 or 30).
ModeChoice choose_display_mode(const std::vector<DisplayMode>& modes, const DisplayMode& current, float target_fps,
                               RefreshPolicy policy = RefreshPolicy::PreferExact);

struct Insets {
    int left = 0, top = 0, right = 0, bottom = 0;
};

inline Insets max_insets(const Insets& a, const Insets& b) {
    return {a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
            a.right > b.right ? a.right : b.right, a.bottom > b.bottom ? a.bottom : b.bottom};
}

// Safe area = union of system bars and display cutout insets, negative values clamped to zero.
Insets compute_safe_area(const Insets& system_bars, const Insets& cutout);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

// Largest rectangle with `aspect` (w/h) that fits in the window minus insets, centred (letterbox / pillarbox).
// `integer_scale_hint` is not applied: the game scales with bilinear filtering.
Rect fit_aspect(int window_w, int window_h, const Insets& inset, float aspect);

} // namespace nue::android
