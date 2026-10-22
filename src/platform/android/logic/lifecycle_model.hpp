// Activity + surface lifecycle as a small state machine.  Java reports raw callbacks; this decides what the
// engine must do (stop rendering, release the swapchain, pause audio, save state ...).  Pure logic.
#pragma once
#include <cstdint>
#include <vector>

namespace nue::android {

enum class LifeEvent {
    Create, Start, Resume, Pause, Stop, Destroy,
    SurfaceCreated, SurfaceChanged, SurfaceDestroyed,
    FocusGained, FocusLost,
    AudioFocusLost, AudioFocusGained,  // permanent or transient loss; ducking is not a loss
    ConfigurationChanged               // rotation, window resize, fold posture: no state change, forces a size refresh
};

enum class LifeAction {
    StartRendering,       // surface + resumed: create / reset the swapchain, resume the frame loop
    StopRendering,        // leave the frame loop; no further GPU submission
    ReleaseSurface,       // surface is going away: destroy the swapchain BEFORE returning to Android
    ResizeSurface,        // same surface, new size (also after rotation)
    SuspendAudio,         // stop the device streams (battery) - game audio is silent anyway
    ResumeAudio,
    PauseSimulation,      // user-visible pause (focus lost / audio focus lost) when the policy says so
    ResumeSimulation,
    SaveState,            // write saves/settings now; the process may be killed after onPause/onStop
    Quit                  // activity destroyed for good
};

struct LifePolicy {
    // A visible window without input focus (split screen, notification shade) keeps rendering; whether the
    // simulation pauses is a game choice.
    bool pause_sim_on_focus_loss = true;
    bool pause_sim_on_audio_focus_loss = true;
    bool save_on_pause = true;
    bool save_on_stop = true;
};

struct LifeState {
    bool created = false;
    bool started = false;
    bool resumed = false;
    bool has_focus = false;
    bool has_surface = false;
    bool audio_focus_lost = false;
    bool destroyed = false;
    int width = 0, height = 0;
    bool rendering = false;       // we told the engine to render
    bool audio_running = false;
    bool sim_paused = false;
};

class LifecycleModel {
public:
    explicit LifecycleModel(LifePolicy policy = {}) : policy_(policy) {}
    // Applies the event and appends the resulting actions (in the order they must be executed).
    void handle(LifeEvent e, int width, int height, std::vector<LifeAction>* out);
    void handle(LifeEvent e, std::vector<LifeAction>* out) { handle(e, 0, 0, out); }
    const LifeState& state() const { return s_; }
    bool can_render() const { return s_.resumed && s_.has_surface && !s_.destroyed; }

private:
    void reconcile(std::vector<LifeAction>* out);
    LifePolicy policy_;
    LifeState s_;
};

const char* to_string(LifeAction a);

} // namespace nue::android
