#include "lifecycle_model.hpp"

namespace nue::android {

const char* to_string(LifeAction a) {
    switch (a) {
    case LifeAction::StartRendering: return "StartRendering";
    case LifeAction::StopRendering: return "StopRendering";
    case LifeAction::ReleaseSurface: return "ReleaseSurface";
    case LifeAction::ResizeSurface: return "ResizeSurface";
    case LifeAction::SuspendAudio: return "SuspendAudio";
    case LifeAction::ResumeAudio: return "ResumeAudio";
    case LifeAction::PauseSimulation: return "PauseSimulation";
    case LifeAction::ResumeSimulation: return "ResumeSimulation";
    case LifeAction::SaveState: return "SaveState";
    case LifeAction::Quit: return "Quit";
    }
    return "?";
}

void LifecycleModel::handle(LifeEvent e, int width, int height, std::vector<LifeAction>* out) {
    bool resize = false;
    bool release_surface = false;
    bool save = false;
    switch (e) {
    case LifeEvent::Create:
        s_ = LifeState{};
        s_.created = true;
        s_.has_focus = true; // Android only reports changes; a new activity starts with focus
        break;
    case LifeEvent::Start: s_.started = true; break;
    case LifeEvent::Resume: s_.resumed = true; break;
    case LifeEvent::Pause:
        s_.resumed = false;
        save = policy_.save_on_pause;
        break;
    case LifeEvent::Stop:
        s_.started = false;
        save = policy_.save_on_stop;
        break;
    case LifeEvent::Destroy:
        s_.destroyed = true;
        s_.resumed = false;
        s_.started = false;
        break;
    case LifeEvent::SurfaceCreated:
        s_.has_surface = true;
        s_.width = width;
        s_.height = height;
        break;
    case LifeEvent::SurfaceChanged:
        if (s_.has_surface && (s_.width != width || s_.height != height)) resize = true;
        s_.has_surface = true;
        s_.width = width;
        s_.height = height;
        break;
    case LifeEvent::SurfaceDestroyed:
        release_surface = s_.has_surface;
        s_.has_surface = false;
        break;
    case LifeEvent::FocusGained: s_.has_focus = true; break;
    case LifeEvent::FocusLost: s_.has_focus = false; break;
    case LifeEvent::AudioFocusLost: s_.audio_focus_lost = true; break;
    case LifeEvent::AudioFocusGained: s_.audio_focus_lost = false; break;
    case LifeEvent::ConfigurationChanged:
        if (s_.has_surface && s_.rendering) resize = true;
        break;
    }

    // The swapchain must be gone before surfaceDestroyed returns, so this action goes first.
    if (release_surface) {
        if (s_.rendering) {
            out->push_back(LifeAction::StopRendering);
            s_.rendering = false;
        }
        out->push_back(LifeAction::ReleaseSurface);
    }
    if (save) out->push_back(LifeAction::SaveState);
    reconcile(out);
    if (resize && s_.rendering) out->push_back(LifeAction::ResizeSurface);
    if (e == LifeEvent::Destroy) out->push_back(LifeAction::Quit);
}

void LifecycleModel::reconcile(std::vector<LifeAction>* out) {
    const bool want_render = can_render();
    if (want_render && !s_.rendering) {
        out->push_back(LifeAction::StartRendering);
        s_.rendering = true;
    } else if (!want_render && s_.rendering) {
        out->push_back(LifeAction::StopRendering);
        s_.rendering = false;
    }

    // Audio runs only while the activity is resumed and nobody else took the audio focus away.
    const bool want_audio = s_.resumed && !s_.destroyed && !s_.audio_focus_lost;
    if (want_audio && !s_.audio_running) {
        out->push_back(LifeAction::ResumeAudio);
        s_.audio_running = true;
    } else if (!want_audio && s_.audio_running) {
        out->push_back(LifeAction::SuspendAudio);
        s_.audio_running = false;
    }

    const bool want_pause =
        s_.resumed && ((policy_.pause_sim_on_focus_loss && !s_.has_focus) ||
                       (policy_.pause_sim_on_audio_focus_loss && s_.audio_focus_lost));
    if (want_pause && !s_.sim_paused) {
        out->push_back(LifeAction::PauseSimulation);
        s_.sim_paused = true;
    } else if (!want_pause && s_.sim_paused && s_.resumed) {
        out->push_back(LifeAction::ResumeSimulation);
        s_.sim_paused = false;
    }
}

} // namespace nue::android
