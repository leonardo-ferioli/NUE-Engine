#include "lifecycle.hpp"

namespace nue::ios {

namespace {
using S = LifecycleState;
using I = LifecycleInput;
constexpr Transition reject(S s) { return {s, act_none, false}; }
constexpr Transition go(S s, uint32_t a) { return {s, a, true}; }
}  // namespace

Transition lifecycle_step(LifecycleState state, LifecycleInput input) {
    switch (input) {
    case I::launched:
        return state == S::not_running ? go(S::inactive, act_none) : reject(state);

    case I::did_become_active:
        if (state == S::inactive) return go(S::active, act_resume_game | act_resume_audio | act_resume_gpu_submission);
        // background -> active without will_enter_foreground can happen with scene notifications arriving out of order:
        // accept it, otherwise the game would stay frozen.
        if (state == S::background) return go(S::active, act_resume_game | act_resume_audio | act_resume_gpu_submission | act_refresh_drawable_size);
        return reject(state);

    case I::will_resign_active:
        if (state == S::active) return go(S::inactive, act_pause_game);
        return reject(state);

    case I::did_enter_background:
        // From active too: scene notifications may skip will_resign_active.
        if (state == S::active) return go(S::background, act_pause_game | act_pause_audio | act_stop_gpu_submission | act_save_state | act_release_caches | act_flush_logs);
        if (state == S::inactive) return go(S::background, act_pause_audio | act_stop_gpu_submission | act_save_state | act_release_caches | act_flush_logs);
        return reject(state);

    case I::will_enter_foreground:
        if (state == S::background) return go(S::inactive, act_refresh_drawable_size);
        return reject(state);

    case I::will_terminate:
        if (state == S::not_running) return reject(state);
        return go(S::not_running, act_pause_game | act_stop_gpu_submission | act_save_state | act_flush_logs);

    case I::memory_warning:
        if (state == S::not_running) return reject(state);
        return go(state, act_release_caches | act_emit_low_memory);
    }
    return reject(state);
}

const char* lifecycle_state_name(LifecycleState s) {
    switch (s) {
    case S::not_running: return "not_running";
    case S::inactive: return "inactive";
    case S::active: return "active";
    case S::background: return "background";
    }
    return "?";
}

const char* lifecycle_input_name(LifecycleInput i) {
    switch (i) {
    case I::launched: return "launched";
    case I::did_become_active: return "did_become_active";
    case I::will_resign_active: return "will_resign_active";
    case I::did_enter_background: return "did_enter_background";
    case I::will_enter_foreground: return "will_enter_foreground";
    case I::will_terminate: return "will_terminate";
    case I::memory_warning: return "memory_warning";
    }
    return "?";
}

}  // namespace nue::ios
