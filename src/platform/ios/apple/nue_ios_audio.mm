// AVAudioSession configuration, interruptions, route changes, latency preferences. The audio stream itself is opened by SDL3
// (SDL_AudioStream -> CoreAudio/AudioUnit) and is not touched here: this module only owns the session.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include "nue_ios_internal.hpp"
#include "audio_policy.hpp"
#include "lifecycle.hpp"
#include "power.hpp"

using namespace nue::ios;

namespace {
int g_policy = 0;
bool g_session_active = false;
bool g_paused_by_interruption = false;
NSMutableArray<id>* g_tokens = nil;

RouteKind current_route_kind() {
    AVAudioSessionRouteDescription* r = [AVAudioSession sharedInstance].currentRoute;
    for (AVAudioSessionPortDescription* p in r.outputs) {
        NSString* t = p.portType;
        if ([t isEqualToString:AVAudioSessionPortBuiltInSpeaker] || [t isEqualToString:AVAudioSessionPortBuiltInReceiver]) return RouteKind::built_in_speaker;
        if ([t isEqualToString:AVAudioSessionPortHeadphones]) return RouteKind::wired;
        if ([t isEqualToString:AVAudioSessionPortUSBAudio]) return RouteKind::usb;
        if ([t isEqualToString:AVAudioSessionPortBluetoothA2DP]) return RouteKind::bluetooth_a2dp;
        if ([t isEqualToString:AVAudioSessionPortBluetoothLE] || [t isEqualToString:AVAudioSessionPortBluetoothHFP]) return RouteKind::bluetooth_le;
        if ([t isEqualToString:AVAudioSessionPortAirPlay]) return RouteKind::airplay;
    }
    return RouteKind::other;
}

void apply_preferences() {
    AVAudioSession* s = [AVAudioSession sharedInstance];
    RouteKind k = current_route_kind();
    NSError* err = nil;
    [s setPreferredIOBufferDuration:preferred_io_buffer_seconds(k, [NSProcessInfo processInfo].lowPowerModeEnabled) error:&err];
    double sr = preferred_sample_rate(k);
    if (sr > 0) [s setPreferredSampleRate:sr error:&err];
}
}  // namespace

namespace nue::ios::detail {

void audio_start() {
    g_tokens = [NSMutableArray array];
    // Under the NUE core SDL3 owns the audio device and sets the session category itself (the adapter exports SDL_AUDIO_CATEGORY);
    // this module then only observes, so the two never fight over the session.
    if (!config().adapter_mode) nue_ios_audio_configure(config().audio_mix_policy);
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    id t1 = [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        bool began = type == AVAudioSessionInterruptionTypeBegan;
        bool resume = !began && ([n.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue] & AVAudioSessionInterruptionOptionShouldResume);
        AudioAction a = on_interruption(began ? InterruptionPhase::began : InterruptionPhase::ended, resume, app_is_active());
        g_paused_by_interruption = began || (g_paused_by_interruption && a != AudioAction::resume);
        if (a == AudioAction::resume && g_session_active && !config().adapter_mode) nue_ios_audio_activate(1);   // the session is deactivated by the system on an interruption
        emit(NUE_IOS_EVENT_AUDIO_INTERRUPTION, began ? 1 : 0, resume ? 1 : 0);
    }];
    id t2 = [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* n) {
        int reason = static_cast<int>([n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue]);
        RouteChange c = route_change_from_raw(reason);
        if (!config().adapter_mode) apply_preferences();
        emit(NUE_IOS_EVENT_AUDIO_ROUTE, static_cast<int64_t>(current_route_kind()), on_route_change(c) == AudioAction::pause ? 1 : 0);
    }];
    id t3 = [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*) {
        // The audio daemon restarted: every audio object is invalid. Re-apply the session; the engine must recreate its stream.
        if (!config().adapter_mode) {
            nue_ios_audio_configure(g_policy);
            if (g_session_active) nue_ios_audio_activate(1);
        }
        emit(NUE_IOS_EVENT_AUDIO_INTERRUPTION, 1, 0);
        emit(NUE_IOS_EVENT_AUDIO_INTERRUPTION, 0, 1);
    }];
    [g_tokens addObjectsFromArray:@[t1, t2, t3]];
}

void audio_stop() {
    for (id t in g_tokens) [[NSNotificationCenter defaultCenter] removeObserver:t];
    g_tokens = nil;
    if (g_session_active && !config().adapter_mode) nue_ios_audio_activate(0);
}

void audio_on_lifecycle_actions(unsigned actions) {
    if (config().adapter_mode) return;   // SDL3 pauses/resumes its own device
    // The game declares no background audio mode (see Info.plist): when backgrounded the process is suspended, so release the session
    // so other apps get the device back immediately, and take it again on return. The engine receives the lifecycle event first (emit() is
    // synchronous) and must have stopped its audio stream by then: setActive:NO fails while I/O is still running.
    if (actions & act_pause_audio) {
        if (g_session_active) {
            NSError* err = nil;
            [[AVAudioSession sharedInstance] setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&err];
        }
    }
    if ((actions & act_resume_audio) && g_session_active && !g_paused_by_interruption) nue_ios_audio_activate(1);
}

}  // namespace nue::ios::detail

extern "C" {

int nue_ios_audio_configure(int mix_policy) {
    g_policy = mix_policy;
    SessionConfig c = session_config_for(static_cast<AudioMixPolicy>(mix_policy));
    AVAudioSession* s = [AVAudioSession sharedInstance];
    NSError* err = nil;
    NSString* category = AVAudioSessionCategoryPlayback;
    AVAudioSessionCategoryOptions opts = 0;
    switch (c.category) {
    case SessionCategory::solo_ambient: category = AVAudioSessionCategorySoloAmbient; break;
    case SessionCategory::ambient: category = AVAudioSessionCategoryAmbient; break;
    case SessionCategory::playback: category = AVAudioSessionCategoryPlayback; break;
    }
    if (c.mix_with_others) opts |= AVAudioSessionCategoryOptionMixWithOthers;
    if (c.duck_others) opts |= AVAudioSessionCategoryOptionDuckOthers;
    // Mode "GameChat" is for voice chat; plain games use Default. MoviePlayback would change routing/processing.
    BOOL ok = [s setCategory:category mode:AVAudioSessionModeDefault options:opts error:&err];
    return ok ? 0 : -1;
}

int nue_ios_audio_activate(int active) {
    AVAudioSession* s = [AVAudioSession sharedInstance];
    NSError* err = nil;
    if (active) apply_preferences();
    BOOL ok = active ? [s setActive:YES error:&err]
                     : [s setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&err];
    if (ok) { g_session_active = active != 0; if (active) g_paused_by_interruption = false; }
    return ok ? 0 : -1;
}

double nue_ios_audio_output_latency(void) { return [AVAudioSession sharedInstance].outputLatency; }

}  // extern "C"
