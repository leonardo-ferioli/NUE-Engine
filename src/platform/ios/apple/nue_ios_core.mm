// init/shutdown, event delivery, lifecycle observers, power (thermal, Low Power Mode, battery), memory pressure, game mode info.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <os/proc.h>
#include <pthread/qos.h>
#include <dispatch/dispatch.h>

#include "nue_ios_internal.hpp"
#include "lifecycle.hpp"
#include "power.hpp"

using namespace nue::ios;

namespace {
NueIosConfig g_config{};
std::string g_game_dir = "NFSU2";
bool g_inited = false;
LifecycleState g_state = LifecycleState::not_running;
NSMutableArray<id>* g_observers = nil;
dispatch_source_t g_mem_source = nil;
dispatch_source_t g_mem_poll = nil;
MemoryPressure g_last_pressure = MemoryPressure::normal;
ThermalLevel g_last_thermal = ThermalLevel::nominal;

void observe(NSNotificationName name, void (^block)(NSNotification*)) {
    id token = [[NSNotificationCenter defaultCenter] addObserverForName:name object:nil queue:[NSOperationQueue mainQueue] usingBlock:block];
    [g_observers addObject:token];
}

void step(LifecycleInput in) {
    Transition t = lifecycle_step(g_state, in);
    if (!t.accepted) {
        nue_ios_log(0, "lifecycle", [[NSString stringWithFormat:@"ignored %s in %s", lifecycle_input_name(in), lifecycle_state_name(g_state)] UTF8String]);
        return;
    }
    g_state = t.next;
    detail::emit(NUE_IOS_EVENT_LIFECYCLE, static_cast<int64_t>(g_state), t.actions);
    detail::audio_on_lifecycle_actions(t.actions);
    detail::input_on_lifecycle_actions(t.actions);
    detail::display_on_lifecycle_actions(t.actions);
}

void publish_thermal() {
    ThermalLevel lvl = thermal_from_raw(static_cast<int>([NSProcessInfo processInfo].thermalState));
    bool lpm = [NSProcessInfo processInfo].lowPowerModeEnabled;
    QualityAdvice adv = advise_quality(lvl, lpm, g_config.target_fps ? g_config.target_fps : 60);
    g_last_thermal = lvl;
    detail::emit(NUE_IOS_EVENT_THERMAL, static_cast<int64_t>(lvl), lpm ? 1 : 0, adv.max_fps, adv.resolution_scale_percent / 100.0f);
}

void publish_power() {
    UIDevice* d = [UIDevice currentDevice];
    d.batteryMonitoringEnabled = YES;
    int state = 0;
    switch (d.batteryState) {
    case UIDeviceBatteryStateUnplugged: state = 1; break;
    case UIDeviceBatteryStateCharging: state = 2; break;
    case UIDeviceBatteryStateFull: state = 3; break;
    default: state = 0; break;
    }
    detail::emit(NUE_IOS_EVENT_POWER, [NSProcessInfo processInfo].lowPowerModeEnabled ? 1 : 0, state, d.batteryLevel);
}

void publish_memory(MemoryPressure p, int64_t available) {
    if (p == g_last_pressure && p == MemoryPressure::normal) return;
    g_last_pressure = p;
    detail::emit(NUE_IOS_EVENT_MEMORY_PRESSURE, static_cast<int64_t>(p), available);
}
}  // namespace

namespace nue::ios::detail {

void emit(const NueIosEvent& e) {
    if (g_config.on_event) g_config.on_event(&e, g_config.user);
}
void emit(NueIosEventKind kind, int64_t a, int64_t b, float f0, float f1, float f2, float f3) {
    NueIosEvent e{};
    e.kind = kind; e.a = a; e.b = b; e.f[0] = f0; e.f[1] = f1; e.f[2] = f2; e.f[3] = f3;
    emit(e);
}
const NueIosConfig& config() { return g_config; }
std::string game_dir_name() { return g_game_dir; }
bool app_is_active() { return g_state == LifecycleState::active; }

void lifecycle_start() {
    // UIApplication-level notifications are used on purpose: they are posted for classic app-delegate apps and, as far as Apple documents,
    // also for apps that adopt UIScene (aggregated across scenes), so SDL3's lifecycle choice does not matter. Unverified on a device.
    // lifecycle_step() tolerates duplicates and reordering (rejected inputs have no effect).
    observe(UIApplicationDidFinishLaunchingNotification, ^(NSNotification*) { step(LifecycleInput::launched); });
    observe(UIApplicationDidBecomeActiveNotification, ^(NSNotification*) { step(LifecycleInput::did_become_active); });
    observe(UIApplicationWillResignActiveNotification, ^(NSNotification*) { step(LifecycleInput::will_resign_active); });
    observe(UIApplicationDidEnterBackgroundNotification, ^(NSNotification*) { step(LifecycleInput::did_enter_background); });
    observe(UIApplicationWillEnterForegroundNotification, ^(NSNotification*) { step(LifecycleInput::will_enter_foreground); });
    observe(UIApplicationWillTerminateNotification, ^(NSNotification*) { step(LifecycleInput::will_terminate); });
    observe(UIApplicationDidReceiveMemoryWarningNotification, ^(NSNotification*) { step(LifecycleInput::memory_warning); });
    // nue_ios_init usually runs after didFinishLaunching: derive the current state from UIApplication.
    UIApplicationState s = [UIApplication sharedApplication].applicationState;
    step(LifecycleInput::launched);
    if (s == UIApplicationStateActive) step(LifecycleInput::did_become_active);
    else if (s == UIApplicationStateBackground) { step(LifecycleInput::did_enter_background); }
}
void lifecycle_stop() {}

void power_start() {
    observe(NSProcessInfoThermalStateDidChangeNotification, ^(NSNotification*) { publish_thermal(); });
    observe(NSProcessInfoPowerStateDidChangeNotification, ^(NSNotification*) { publish_thermal(); publish_power(); });
    observe(UIDeviceBatteryStateDidChangeNotification, ^(NSNotification*) { publish_power(); });
    observe(UIDeviceBatteryLevelDidChangeNotification, ^(NSNotification*) { publish_power(); });
    publish_thermal();
    publish_power();

    // Kernel memory-pressure notifications (warning / critical): complement to UIApplicationDidReceiveMemoryWarning, which only fires for
    // the foreground app. Handler runs on the main queue.
    g_mem_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                          DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, dispatch_get_main_queue());
    dispatch_source_set_event_handler(g_mem_source, ^{
        unsigned long m = dispatch_source_get_data(g_mem_source);
        int64_t avail = nue_ios_available_memory();
        publish_memory((m & DISPATCH_MEMORYPRESSURE_CRITICAL) ? MemoryPressure::critical : MemoryPressure::high, avail);
    });
    dispatch_resume(g_mem_source);

    if (g_config.enable_memory_poll) {
        g_mem_poll = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(g_mem_poll, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(g_mem_poll, ^{
            if (!app_is_active()) return;
            int64_t avail = nue_ios_available_memory();
            if (avail < 0) return;
            MemoryPressure p = classify_memory(static_cast<uint64_t>(avail));
            if (p != g_last_pressure) publish_memory(p, avail);
        });
        dispatch_resume(g_mem_poll);
    }
}
void power_stop() {
    if (g_mem_source) { dispatch_source_cancel(g_mem_source); g_mem_source = nil; }
    if (g_mem_poll) { dispatch_source_cancel(g_mem_poll); g_mem_poll = nil; }
}

}  // namespace nue::ios::detail

extern "C" {

int nue_ios_init(const NueIosConfig* config) {
    if (g_inited) return -1;
    if (config) g_config = *config;
    if (g_config.game_dir_name && g_config.game_dir_name[0]) g_game_dir = g_config.game_dir_name;
    g_observers = [NSMutableArray array];
    g_inited = true;
    detail::lifecycle_start();
    detail::power_start();
    detail::audio_start();
    if (!g_config.adapter_mode) {   // under the NUE core, SDL3 owns input and display events
        detail::input_start();
        detail::display_start();
    }
    if (g_config.enable_crash_handler) nue_ios_install_crash_handler();
    return 0;
}

void nue_ios_shutdown(void) {
    if (!g_inited) return;
    detail::display_stop();   // also invalidates the frame-rate CADisplayLink, which nue_ios_set_frame_rate creates on demand
    if (!g_config.adapter_mode) detail::input_stop();
    detail::audio_stop();
    detail::power_stop();
    detail::lifecycle_stop();
    for (id t in g_observers) [[NSNotificationCenter defaultCenter] removeObserver:t];
    g_observers = nil;
    g_inited = false;
}

void nue_ios_set_keep_awake(int enabled) {
    dispatch_async(dispatch_get_main_queue(), ^{ [UIApplication sharedApplication].idleTimerDisabled = enabled ? YES : NO; });
}

int64_t nue_ios_available_memory(void) {
    return static_cast<int64_t>(os_proc_available_memory());   // 0 on the Simulator and macOS: treat <= 0 as unknown
}

void nue_ios_set_thread_qos(int qos) {
    qos_class_t c = QOS_CLASS_DEFAULT;
    switch (qos) {
    case 0: c = QOS_CLASS_USER_INTERACTIVE; break;
    case 1: c = QOS_CLASS_USER_INITIATED; break;
    case 2: c = QOS_CLASS_UTILITY; break;
    case 3: c = QOS_CLASS_BACKGROUND; break;
    }
    pthread_set_qos_class_self_np(c, 0);
}

int nue_ios_game_mode_info(void) {
    NSDictionary* info = [[NSBundle mainBundle] infoDictionary];
    int bits = 0;
    if ([info[@"GCSupportsGameMode"] boolValue]) bits |= 1;
    NSString* cat = info[@"LSApplicationCategoryType"];
    if ([cat hasPrefix:@"public.app-category."] && [cat containsString:@"games"]) bits |= 2;
    if (@available(iOS 18.0, *)) bits |= 4;
    return bits;
}

}  // extern "C"
