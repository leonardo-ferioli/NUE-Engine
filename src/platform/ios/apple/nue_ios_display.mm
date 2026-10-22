// Display and graphics support: safe area, orientation, frame-rate request, home-indicator deferral, Metal device probe and CAMetalLayer setup.
// The CAMetalLayer itself is created by SDL3 (SDL_Metal_CreateView) and handed to bgfx; this module only configures it and reports caps.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "nue_ios_internal.hpp"
#include "caps.hpp"
#include "lifecycle.hpp"
#include "power.hpp"

using namespace nue::ios;

// Objective-C classes cannot live inside a C++ namespace.
@interface NUEPacingTarget : NSObject
- (void)tick:(CADisplayLink*)link;
@end
@implementation NUEPacingTarget
- (void)tick:(CADisplayLink*)link {}   // intentionally empty: the link only carries preferredFrameRateRange to the display
@end

namespace {
NUEPacingTarget* g_target = nil;
__weak UIWindow* g_window = nil;
CADisplayLink* g_link = nil;
NSMutableArray<id>* g_tokens = nil;
float g_fr_min = 0, g_fr_pref = 0, g_fr_max = 0;
bool g_home_hidden = false;

UIWindow* find_window() {
    if (g_window) return g_window;
    for (UIScene* s in [UIApplication sharedApplication].connectedScenes)
        if ([s isKindOfClass:[UIWindowScene class]]) {
            UIWindow* w = ((UIWindowScene*)s).windows.firstObject;
            if (w) return w;
        }
    return nil;
}

void publish_safe_area_and_display() {
    UIWindow* w = find_window();
    if (!w) return;
    UIEdgeInsets i = w.safeAreaInsets;
    nue::ios::detail::emit(NUE_IOS_EVENT_SAFE_AREA, 0, 0, static_cast<float>(i.top), static_cast<float>(i.left), static_cast<float>(i.bottom), static_cast<float>(i.right));
    UIScreen* screen = w.windowScene.screen ?: [UIScreen mainScreen];
    UIInterfaceOrientation o = w.windowScene.interfaceOrientation;
    nue::ios::detail::emit(NUE_IOS_EVENT_DISPLAY, static_cast<int64_t>(o), 0, static_cast<float>(w.bounds.size.width), static_cast<float>(w.bounds.size.height),
                           static_cast<float>(screen.scale), static_cast<float>(screen.maximumFramesPerSecond));
}

void apply_frame_rate() {
    if (!g_link) {
        g_target = [NUEPacingTarget new];
        g_link = [CADisplayLink displayLinkWithTarget:g_target selector:@selector(tick:)];
        [g_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
    g_link.paused = !nue::ios::detail::app_is_active();
    if (g_fr_max > 0) g_link.preferredFrameRateRange = CAFrameRateRangeMake(g_fr_min, g_fr_max, g_fr_pref);
    else g_link.preferredFrameRateRange = CAFrameRateRangeDefault;
}
}  // namespace

namespace nue::ios::detail {

void display_start() {
    g_tokens = [NSMutableArray array];
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    // Orientation / window size changes (rotation, Stage Manager, Split View): re-publish the geometry.
    // Device orientation notifications (the status-bar one is deprecated and silent for scene apps). SDL3 also reports window size and
    // safe-area changes; the core adapter should call nue_ios_refresh_display() from those events as well.
    [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];
    [g_tokens addObject:[nc addObserverForName:UIDeviceOrientationDidChangeNotification object:nil queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification*) { publish_safe_area_and_display(); }]];
    [g_tokens addObject:[nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification*) { publish_safe_area_and_display(); }]];
    [g_tokens addObject:[nc addObserverForName:UIScreenModeDidChangeNotification object:nil queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification*) { publish_safe_area_and_display(); }]];
    // Frame-rate range: derive from the target and the thermal/LPM advice; updated again by nue_ios_set_frame_rate.
    uint16_t target = config().target_fps ? config().target_fps : 60;
    FrameRateRange r = choose_frame_rate(static_cast<uint16_t>([UIScreen mainScreen].maximumFramesPerSecond), target);
    nue_ios_set_frame_rate(r.minimum, r.preferred, r.maximum);
    publish_safe_area_and_display();
}

void display_stop() {
    [[UIDevice currentDevice] endGeneratingDeviceOrientationNotifications];
    for (id t in g_tokens) [[NSNotificationCenter defaultCenter] removeObserver:t];
    g_tokens = nil;
    [g_link invalidate];
    g_link = nil;
}

void display_on_lifecycle_actions(unsigned actions) {
    if (actions & (act_pause_game | act_stop_gpu_submission)) { if (g_link) g_link.paused = YES; }
    if (actions & act_resume_game) { if (g_link) g_link.paused = NO; }
    if (actions & act_refresh_drawable_size) publish_safe_area_and_display();
}

}  // namespace nue::ios::detail

extern "C" {

void nue_ios_refresh_display(void) { publish_safe_area_and_display(); }

void nue_ios_set_window(void* ui_window) {
    g_window = (__bridge UIWindow*)ui_window;
    publish_safe_area_and_display();
    // Hidden home indicator / deferred edge gestures need the root view controller of this window.
    nue_ios_set_home_indicator_hidden(g_home_hidden);
}

void nue_ios_set_frame_rate(float minimum, float preferred, float maximum) {
    g_fr_min = minimum; g_fr_pref = preferred; g_fr_max = maximum;
    apply_frame_rate();
}

void nue_ios_set_home_indicator_hidden(int hidden) {
    g_home_hidden = hidden != 0;
    // SDL3's own view controller reads SDL_HINT_IOS_HIDE_HOME_INDICATOR ("1" = hide, "2" = hide only after the first touch);
    // the hint is the supported way, and the sample sets it before window creation. This call only asks the system to re-evaluate.
    UIViewController* vc = find_window().rootViewController;
    [vc setNeedsUpdateOfHomeIndicatorAutoHidden];
    [vc setNeedsUpdateOfScreenEdgesDeferringSystemGestures];
}

int nue_ios_probe_gpu(NueIosGpuInfo* out) {
    if (!out) return -1;
    std::memset(out, 0, sizeof *out);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) return -1;
    std::snprintf(out->name, sizeof out->name, "%s", [dev.name UTF8String]);

    MetalProbe p;
    // MTLGPUFamilyApple1 = 1001 ... Apple9 = 1009. supportsFamily: needs iOS 13+.
    for (int n = 9; n >= 1; --n)
        if ([dev supportsFamily:static_cast<MTLGPUFamily>(1000 + n)]) { p.highest_apple_family = n; break; }
    // supportsBCTextureCompression exists from iOS 16.4 (and macOS 11); earlier systems cannot sample BC on any Apple GPU.
    if (@available(iOS 16.4, macOS 11.0, *)) p.supports_bc = [dev supportsBCTextureCompression];
    p.unified_memory = [dev hasUnifiedMemory];
#if TARGET_OS_SIMULATOR
    p.is_simulator = true;
#endif
    // recommendedMaxWorkingSetSize needs iOS 16; the engine preset deploys to iOS 15.
    if (@available(iOS 16.0, macOS 10.12, *)) p.recommended_max_working_set = [dev recommendedMaxWorkingSetSize];
    p.physical_memory = [NSProcessInfo processInfo].physicalMemory;

    DeviceCaps c = derive_caps(p);
    MemoryPolicy policy;
    policy.override_hard_limit = nue::ios::detail::config().memory_limit_override_bytes;
    MemoryBudget b = plan_memory_budget(p.physical_memory, static_cast<uint64_t>(std::max<int64_t>(0, nue_ios_available_memory())),
                                        static_cast<uint64_t>(std::max<int64_t>(0, nue_ios_memory_footprint())), policy);
    TexturePolicy pol = choose_texture_policy(c, b.texture_share, 0);

    out->highest_apple_family = p.highest_apple_family;
    out->supports_bc = p.supports_bc; out->unified_memory = p.unified_memory; out->is_simulator = p.is_simulator;
    out->recommended_max_working_set = p.recommended_max_working_set;
    out->physical_memory = p.physical_memory;
    out->texture_format_count = static_cast<uint32_t>(c.texture_format.size());
    for (size_t i = 0; i < c.texture_format.size() && i < sizeof out->texture_format; ++i) out->texture_format[i] = c.texture_format[i];
    out->max_texture_size = c.max_texture_size; out->max_anisotropy = c.max_anisotropy; out->max_samples = c.max_samples;
    out->depth24_stencil8 = c.depth24_stencil8;
    out->texture_storage = static_cast<int>(pol.preferred);
    out->hard_memory_limit = b.hard_limit; out->soft_memory_limit = b.soft_limit; out->texture_budget = b.texture_share;
    return 0;
}

int nue_ios_configure_metal_layer(void* layer_ptr, int framebuffer_only, uint32_t* width_px, uint32_t* height_px) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_ptr;
    if (![layer isKindOfClass:[CAMetalLayer class]]) return -1;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = framebuffer_only ? YES : NO;   // NO is needed to read the drawable back (screenshots) or blit into it
    layer.maximumDrawableCount = 3;
    // CAMetalLayer.displaySyncEnabled is macOS-only: iOS always presents in sync with the display.
    layer.opaque = YES;
    layer.allowsNextDrawableTimeout = YES;
    UIScreen* screen = [UIScreen mainScreen];
    layer.contentsScale = screen.nativeScale;
    CGSize size = layer.bounds.size;
    layer.drawableSize = CGSizeMake(size.width * layer.contentsScale, size.height * layer.contentsScale);
    if (width_px) *width_px = static_cast<uint32_t>(layer.drawableSize.width);
    if (height_px) *height_px = static_cast<uint32_t>(layer.drawableSize.height);
    return 0;
}

}  // extern "C"
