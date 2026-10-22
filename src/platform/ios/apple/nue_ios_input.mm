// GameController (pads, keyboard/mouse presence), CoreHaptics rumble through GCController.haptics, CoreMotion gyro steering,
// last-used-device signal. Touch events are delivered by SDL3 (SDL_EVENT_FINGER_*); this module only classifies them if the core asks.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <CoreHaptics/CoreHaptics.h>
#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "nue_ios_internal.hpp"
#include "input_logic.hpp"
#include "lifecycle.hpp"

using namespace nue::ios;

namespace {
std::mutex g_mutex;                         // guards g_pads (pad_state/rumble can be called from the game thread)
NSMutableArray<GCController*>* g_pads = nil;   // connection order = pad index
NSMutableArray<id>* g_tokens = nil;
CMMotionManager* g_motion = nil;
std::mutex g_motion_mutex;
float g_gx = 0, g_gy = -1, g_gz = 0;
bool g_motion_valid = false;
float g_neutral_deg = 0;
LastUsedInput g_last;
uint32_t now_ms() { return static_cast<uint32_t>(CACurrentMediaTime() * 1000.0); }

void note_device(InputDevice d, bool significant = true) {
    g_last.tick(now_ms());
    if (g_last.note(d, significant)) nue::ios::detail::emit(NUE_IOS_EVENT_INPUT_DEVICE, static_cast<int64_t>(d));
}

PadFamily family_of(GCController* c) {
    NSString* cat = c.productCategory;
    return pad_family_from_product_category(cat ? [cat UTF8String] : nullptr);
}

void attach_pad(GCController* c) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (![g_pads containsObject:c]) [g_pads addObject:c];
    }
    GCExtendedGamepad* gp = c.extendedGamepad;
    if (gp) {
        gp.valueChangedHandler = ^(GCExtendedGamepad* pad, GCControllerElement* element) {
            // Mark the pad as the active device only for deliberate input: stick noise inside the deadzone is not input.
            bool significant = true;
            if (element == pad.leftThumbstick || element == pad.rightThumbstick) {
                GCControllerDirectionPad* d = (GCControllerDirectionPad*)element;
                float x = d.xAxis.value, y = d.yAxis.value;
                significant = (x * x + y * y) > 0.25f;
            }
            note_device(InputDevice::gamepad, significant);
        };
    }
    int idx = static_cast<int>([g_pads indexOfObject:c]);
    nue::ios::detail::emit(NUE_IOS_EVENT_CONTROLLER, 1, idx);
    note_device(InputDevice::gamepad);
}

void detach_pad(GCController* c) {
    int idx;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        idx = static_cast<int>([g_pads indexOfObject:c]);
        if (idx != NSNotFound) [g_pads removeObjectAtIndex:idx];
    }
    nue::ios::detail::emit(NUE_IOS_EVENT_CONTROLLER, 0, idx == NSNotFound ? -1 : idx);
}

void start_motion() {
    if (g_motion) return;
    g_motion = [CMMotionManager new];
    if (!g_motion.deviceMotionAvailable) { g_motion = nil; return; }
    g_motion.deviceMotionUpdateInterval = 1.0 / 60.0;
    NSOperationQueue* q = [NSOperationQueue new];
    q.name = @"org.nue.motion";
    q.qualityOfService = NSQualityOfServiceUserInteractive;
    // Reference frame X-arbitrary-Z-vertical is not needed: gravity is enough for a wheel angle and does not drift.
    [g_motion startDeviceMotionUpdatesToQueue:q withHandler:^(CMDeviceMotion* m, NSError* err) {
        if (!m) return;
        std::lock_guard<std::mutex> lk(g_motion_mutex);
        g_gx = static_cast<float>(m.gravity.x); g_gy = static_cast<float>(m.gravity.y); g_gz = static_cast<float>(m.gravity.z);
        g_motion_valid = true;
    }];
}

void stop_motion() {
    if (!g_motion) return;
    [g_motion stopDeviceMotionUpdates];
    g_motion = nil;
    std::lock_guard<std::mutex> lk(g_motion_mutex);
    g_motion_valid = false;
}

int current_interface_orientation() {
    for (UIScene* s in [UIApplication sharedApplication].connectedScenes)
        if ([s isKindOfClass:[UIWindowScene class]]) return static_cast<int>(((UIWindowScene*)s).interfaceOrientation);
    return 3;   // landscapeRight
}
}  // namespace

namespace nue::ios::detail {

void input_start() {
    g_pads = [NSMutableArray array];
    g_tokens = [NSMutableArray array];
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    [g_tokens addObject:[nc addObserverForName:GCControllerDidConnectNotification object:nil queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification* n) { attach_pad((GCController*)n.object); }]];
    [g_tokens addObject:[nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification* n) { detach_pad((GCController*)n.object); }]];
    for (GCController* c in [GCController controllers]) attach_pad(c);

    // Keyboard and mouse on iPad (and iPhone with a Bluetooth keyboard): used only as "last used device" signals; SDL3 delivers the events.
    [g_tokens addObject:[nc addObserverForName:GCKeyboardDidConnectNotification object:nil queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification*) { note_device(InputDevice::keyboard); }]];
    [g_tokens addObject:[nc addObserverForName:GCMouseDidConnectNotification object:nil queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification*) { note_device(InputDevice::mouse); }]];
    GCKeyboard.coalescedKeyboard.keyboardInput.keyChangedHandler = ^(GCKeyboardInput*, GCControllerButtonInput*, GCKeyCode, BOOL pressed) {
        if (pressed) note_device(InputDevice::keyboard);
    };
    GCMouse.current.mouseInput.mouseMovedHandler = ^(GCMouseInput*, float dx, float dy) {
        note_device(InputDevice::mouse, dx * dx + dy * dy > 4.0f);
    };
    // Wireless pads are discovered only while this is running; the system also shows its own UI when GCSupportsControllerUserInteraction is set.
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:^{}];

    if (config().enable_motion) start_motion();
}

void input_stop() {
    [GCController stopWirelessControllerDiscovery];
    for (id t in g_tokens) [[NSNotificationCenter defaultCenter] removeObserver:t];
    g_tokens = nil;
    stop_motion();
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pads = nil;
}

void input_on_lifecycle_actions(unsigned actions) {
    // CoreMotion keeps running in the background otherwise (it is cheap, but the player would steer while the app is suspended).
    if (!config().enable_motion) return;
    if (actions & act_pause_game) stop_motion();
    if (actions & act_resume_game) start_motion();
}

}  // namespace nue::ios::detail

extern "C" {

int nue_ios_pad_state(int index, NueIosPadState* out) {
    GCController* c = nil;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (index < 0 || index >= static_cast<int>(g_pads.count)) return -1;
        c = g_pads[static_cast<NSUInteger>(index)];
    }
    GCExtendedGamepad* gp = c.extendedGamepad;
    if (!gp || !out) return -1;
    PadState raw;
    auto btn = [&](GCControllerButtonInput* b, uint32_t bit) { if (b.isPressed) raw.buttons |= bit; };
    btn(gp.buttonA, pad_a); btn(gp.buttonB, pad_b); btn(gp.buttonX, pad_x); btn(gp.buttonY, pad_y);
    btn(gp.leftShoulder, pad_left_shoulder); btn(gp.rightShoulder, pad_right_shoulder);
    btn(gp.dpad.up, pad_dpad_up); btn(gp.dpad.down, pad_dpad_down); btn(gp.dpad.left, pad_dpad_left); btn(gp.dpad.right, pad_dpad_right);
    btn(gp.buttonMenu, pad_menu);
    if (gp.buttonOptions) btn(gp.buttonOptions, pad_options);
    if (gp.buttonHome) btn(gp.buttonHome, pad_home);
    if (gp.leftThumbstickButton) btn(gp.leftThumbstickButton, pad_left_stick);
    if (gp.rightThumbstickButton) btn(gp.rightThumbstickButton, pad_right_stick);
    raw.left_x = gp.leftThumbstick.xAxis.value;  raw.left_y = gp.leftThumbstick.yAxis.value;   // GameController: y up, same as ours
    raw.right_x = gp.rightThumbstick.xAxis.value; raw.right_y = gp.rightThumbstick.yAxis.value;
    raw.left_trigger = gp.leftTrigger.value; raw.right_trigger = gp.rightTrigger.value;
    PadState s = condition_pad(raw);
    out->buttons = s.buttons;
    out->lx = s.left_x; out->ly = s.left_y; out->rx = s.right_x; out->ry = s.right_y;
    out->lt = s.left_trigger; out->rt = s.right_trigger;
    out->family = static_cast<int>(family_of(c));
    out->has_haptics = c.haptics != nil;
    return 0;
}

int nue_ios_pad_rumble(int index, float low, float high, uint32_t duration_ms) {
    GCController* c = nil;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (index < 0 || index >= static_cast<int>(g_pads.count)) return -1;
        c = g_pads[static_cast<NSUInteger>(index)];
    }
    GCDeviceHaptics* h = c.haptics;
    if (!h) return -1;
    // One engine per pad and locality, kept in an associated dictionary: creating CHHapticEngine per call is expensive.
    static NSMutableDictionary<NSString*, CHHapticEngine*>* engines = [NSMutableDictionary dictionary];
    float strength = std::max(low, high);
    if (strength <= 0.0f) return 0;
    NSString* key = [NSString stringWithFormat:@"%p", c];
    CHHapticEngine* engine = engines[key];
    if (!engine) {
        engine = [h createEngineWithLocality:GCHapticsLocalityDefault];
        if (!engine) return -1;
        NSError* e = nil;
        [engine startAndReturnError:&e];
        __weak CHHapticEngine* weak_engine = engine;
        engine.resetHandler = ^{ NSError* e2 = nil; [weak_engine startAndReturnError:&e2]; };
        engines[key] = engine;
    }
    NSError* err = nil;
    CHHapticEventParameter* intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:strength];
    CHHapticEventParameter* sharp = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                                                   value:high > low ? 0.8f : 0.2f];   // high = fast motor = sharp
    CHHapticEvent* ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[intensity, sharp]
                                                    relativeTime:0 duration:duration_ms / 1000.0];
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
    if (!pattern) return -1;
    id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&err];
    return (player && [player startAtTime:0 error:&err]) ? 0 : -1;
}

int nue_ios_motion_steering(float* out) {
    float gx, gy, gz;
    {
        std::lock_guard<std::mutex> lk(g_motion_mutex);
        if (!g_motion_valid || !out) return -1;
        gx = g_gx; gy = g_gy; gz = g_gz;
    }
    // Neutral offset: rotate gravity by -g_neutral_deg about z before measuring.
    const float a = -g_neutral_deg * 0.017453292519943295f, ca = std::cos(a), sa = std::sin(a);
    const float rx = gx * ca - gy * sa, ry = gx * sa + gy * ca;
    *out = steering_from_gravity(rx, ry, gz, current_interface_orientation());
    g_last.tick(now_ms());
    if (g_last.note_motion(*out, now_ms())) nue::ios::detail::emit(NUE_IOS_EVENT_INPUT_DEVICE, static_cast<int64_t>(InputDevice::motion));
    return 0;
}

void nue_ios_motion_recalibrate(void) {
    float gx, gy;
    {
        std::lock_guard<std::mutex> lk(g_motion_mutex);
        if (!g_motion_valid) return;
        gx = g_gx; gy = g_gy;
    }
    // Current wheel angle in the active interface orientation becomes the neutral offset.
    const int o = current_interface_orientation();
    float dx = 0, dy = -1, rx = 1, ry = 0;
    if (o == 2) { dx = 0; dy = 1; rx = -1; ry = 0; }
    else if (o == 3) { dx = -1; dy = 0; rx = 0; ry = -1; }
    else if (o == 4) { dx = 1; dy = 0; rx = 0; ry = 1; }
    g_neutral_deg = std::atan2(gx * rx + gy * ry, gx * dx + gy * dy) * 57.29577951308232f;
}

}  // extern "C"
