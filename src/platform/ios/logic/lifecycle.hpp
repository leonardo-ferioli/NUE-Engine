// iOS app lifecycle as a pure state machine (no Apple headers: runs in host tests).
//
// The Objective-C++ glue (apple/nue_ios_lifecycle.mm) translates UIKit notifications into LifecycleInput and executes the returned
// actions. Keeping the table here makes the policy reviewable and testable on any machine.
#pragma once
#include <cstdint>

namespace nue::ios {

enum class LifecycleState : uint8_t { not_running, inactive, active, background };

enum class LifecycleInput : uint8_t {
    launched,            // application:didFinishLaunching / first scene connect
    did_become_active,   // UIApplicationDidBecomeActiveNotification
    will_resign_active,  // UIApplicationWillResignActiveNotification (call, control centre, app switcher, multitasking)
    did_enter_background,
    will_enter_foreground,
    will_terminate,      // UIApplicationWillTerminateNotification (not guaranteed to be delivered)
    memory_warning,      // UIApplicationDidReceiveMemoryWarningNotification
};

// What the glue must do. Bits, so a transition can request several.
enum LifecycleAction : uint32_t {
    act_none = 0,
    act_pause_game = 1u << 0,          // stop simulating / present a pause state
    act_resume_game = 1u << 1,
    act_pause_audio = 1u << 2,         // unless background audio is enabled by the game (policy in the glue)
    act_resume_audio = 1u << 3,
    act_stop_gpu_submission = 1u << 4, // Metal: no command buffer may be committed while backgrounded (the OS can kill the app)
    act_resume_gpu_submission = 1u << 5,
    act_save_state = 1u << 6,          // persist settings/saves now: the process may be killed without notice afterwards
    act_release_caches = 1u << 7,      // drop reloadable memory (decoded textures, shader caches in RAM)
    act_flush_logs = 1u << 8,
    act_emit_low_memory = 1u << 9,     // forward to the engine as a low-memory event
    act_refresh_drawable_size = 1u << 10, // the layer size/scale may have changed while away (rotation, Stage Manager)
};

struct Transition {
    LifecycleState next;
    uint32_t actions;
    bool accepted;   // false: the input does not make sense in this state (state unchanged, no actions)
};

Transition lifecycle_step(LifecycleState state, LifecycleInput input);

const char* lifecycle_state_name(LifecycleState);
const char* lifecycle_input_name(LifecycleInput);

}  // namespace nue::ios
