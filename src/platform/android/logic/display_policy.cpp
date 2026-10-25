#include "display_policy.hpp"

#include <algorithm>
#include <cmath>

namespace nue::android {

ModeChoice choose_display_mode(const std::vector<DisplayMode>& modes, const DisplayMode& current, float target_fps,
                               RefreshPolicy policy) {
    ModeChoice best;
    if (target_fps <= 0.0f) return best;

    struct Cand {
        const DisplayMode* m;
        int interval;
        double err;       // relative error of refresh/interval vs target
        bool exact;
    };
    std::vector<Cand> cands;
    for (const DisplayMode& m : modes) {
        if (m.width != current.width || m.height != current.height) continue;
        int interval = std::max(1, static_cast<int>(std::lround(m.refresh / target_fps)));
        double eff = double(m.refresh) / interval;
        double err = std::fabs(eff - target_fps) / target_fps;
        cands.push_back({&m, interval, err, std::fabs(m.refresh - target_fps) < 0.5f});
    }
    if (cands.empty()) return best;

    // Only multiples whose effective rate is within 1 % of the target keep the game's timing intact
    // (e.g. 120 Hz -> interval 2; 90 Hz -> interval 1.5 is rejected, 144 Hz -> 2.4 is rejected).
    auto usable = [](const Cand& c) { return c.err <= 0.01; };
    const Cand* pick = nullptr;
    for (const Cand& c : cands) {
        if (!usable(c)) continue;
        if (!pick) { pick = &c; continue; }
        bool better;
        if (policy == RefreshPolicy::PreferHighest) {
            better = c.m->refresh > pick->m->refresh;
        } else {
            // exact first, then the smallest interval (lowest refresh = least power), then lower error
            if (c.exact != pick->exact) better = c.exact;
            else if (c.interval != pick->interval) better = c.interval < pick->interval;
            else better = c.err < pick->err;
        }
        if (better) pick = &c;
    }
    if (!pick) {
        // No clean multiple (e.g. 30 fps on a 90 Hz panel is clean, 45 fps is not): take the closest rate and let
        // the engine's own pacer drop frames.
        pick = &*std::min_element(cands.begin(), cands.end(),
                                  [](const Cand& a, const Cand& b) { return a.err < b.err; });
    }
    best.mode_id = pick->m->id;
    best.refresh = pick->m->refresh;
    best.present_interval = pick->interval;
    best.exact = pick->exact;
    best.seamless_multiple = !pick->exact && usable(*pick);
    return best;
}

Insets compute_safe_area(const Insets& bars, const Insets& cutout) {
    Insets r = max_insets(bars, cutout);
    r.left = std::max(0, r.left);
    r.top = std::max(0, r.top);
    r.right = std::max(0, r.right);
    r.bottom = std::max(0, r.bottom);
    return r;
}

Rect fit_aspect(int window_w, int window_h, const Insets& inset, float aspect) {
    Rect avail{inset.left, inset.top, window_w - inset.left - inset.right, window_h - inset.top - inset.bottom};
    if (avail.w <= 0 || avail.h <= 0 || aspect <= 0.0f) return {0, 0, std::max(0, window_w), std::max(0, window_h)};
    Rect r = avail;
    if (double(avail.w) / avail.h > aspect) {
        r.w = static_cast<int>(std::lround(avail.h * double(aspect)));
        r.x = avail.x + (avail.w - r.w) / 2;
    } else {
        r.h = static_cast<int>(std::lround(avail.w / double(aspect)));
        r.y = avail.y + (avail.h - r.h) / 2;
    }
    return r;
}

} // namespace nue::android
