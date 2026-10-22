#include "android_state.hpp"

namespace nue::android::native {

State& state() {
    static State* s = new State(); // never destroyed: native threads can still call us during process teardown
    return *s;
}

JniEnv::JniEnv() {
    State& st = state();
    if (!st.vm) return;
    jint r = st.vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (st.vm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
            attached_ = true;
        } else {
            env_ = nullptr;
        }
    } else if (r != JNI_OK) {
        env_ = nullptr;
    }
}

JniEnv::~JniEnv() {
    if (attached_) state().vm->DetachCurrentThread();
}

void push_life_actions(const std::vector<LifeAction>& actions) {
    State& st = state();
    for (LifeAction a : actions) {
        int32_t code = 0;
        switch (a) {
        case LifeAction::StartRendering: code = NUE_ANDROID_ACTION_START_RENDERING; break;
        case LifeAction::StopRendering: code = NUE_ANDROID_ACTION_STOP_RENDERING; break;
        case LifeAction::ReleaseSurface: code = NUE_ANDROID_ACTION_RELEASE_SURFACE; break;
        case LifeAction::ResizeSurface: code = NUE_ANDROID_ACTION_RESIZE_SURFACE; break;
        case LifeAction::SuspendAudio: code = NUE_ANDROID_ACTION_SUSPEND_AUDIO; break;
        case LifeAction::ResumeAudio: code = NUE_ANDROID_ACTION_RESUME_AUDIO; break;
        case LifeAction::PauseSimulation: code = NUE_ANDROID_ACTION_PAUSE_SIMULATION; break;
        case LifeAction::ResumeSimulation: code = NUE_ANDROID_ACTION_RESUME_SIMULATION; break;
        case LifeAction::SaveState: code = NUE_ANDROID_ACTION_SAVE_STATE; break;
        case LifeAction::Quit: code = NUE_ANDROID_ACTION_QUIT; break;
        }
        if (code) st.events.push(make_event(NUE_ANDROID_EVENT_ACTION, code));
    }
}

} // namespace nue::android::native
