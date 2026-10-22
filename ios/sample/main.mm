// NUE iOS sample: SDL3 window + CAMetalLayer, the iOS backend services, and an on-screen status overlay.
// It proves the platform plumbing (lifecycle, power, memory, storage, audio session, input, Metal caps), not the game: no bgfx here.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "nue_ios.h"

namespace {

struct Status {
    std::atomic<int> lifecycle{0};      // LifecycleState
    std::atomic<int> thermal{0};
    std::atomic<int> pressure{0};
    std::atomic<int64_t> available{-1};
    std::atomic<int> low_power{0};
    std::atomic<int> pads{0};
    std::atomic<int> input_device{0};
    std::atomic<int> audio_interruptions{0};
    std::atomic<float> max_fps{60};
    std::atomic<float> insets[4]{};
    std::atomic<bool> gpu_allowed{true};   // false while backgrounded: no Metal submission
} g;

const char* kLifecycle[] = {"not_running", "inactive", "active", "background"};
const char* kLevel[] = {"nominal/normal", "fair/moderate", "serious/high", "critical"};
const char* kDevice[] = {"none", "touch", "keyboard", "mouse", "gamepad", "motion"};

void on_event(const NueIosEvent* e, void*) {
    switch (e->kind) {
    case NUE_IOS_EVENT_LIFECYCLE:
        g.lifecycle = static_cast<int>(e->a);
        // actions bit 4 = act_stop_gpu_submission, bit 5 = act_resume_gpu_submission (logic/lifecycle.hpp)
        if (e->b & (1u << 4)) g.gpu_allowed = false;
        if (e->b & (1u << 5)) g.gpu_allowed = true;
        break;
    case NUE_IOS_EVENT_THERMAL: g.thermal = static_cast<int>(e->a); g.max_fps = e->f[0]; g.low_power = static_cast<int>(e->b); break;
    case NUE_IOS_EVENT_POWER: g.low_power = static_cast<int>(e->a); break;
    case NUE_IOS_EVENT_MEMORY_PRESSURE: g.pressure = static_cast<int>(e->a); g.available = e->b; break;
    case NUE_IOS_EVENT_AUDIO_INTERRUPTION: if (e->a) ++g.audio_interruptions; break;
    case NUE_IOS_EVENT_SAFE_AREA: for (int i = 0; i < 4; ++i) g.insets[i] = e->f[i]; break;
    case NUE_IOS_EVENT_CONTROLLER: g.pads += e->a ? 1 : -1; break;
    case NUE_IOS_EVENT_INPUT_DEVICE: g.input_device = static_cast<int>(e->a); break;
    default: break;
    }
}

std::string describe() {
    char buf[1024];
    NueIosGpuInfo gpu{};
    static bool probed = false;
    static NueIosGpuInfo cached{};
    if (!probed) { nue_ios_probe_gpu(&cached); probed = true; }
    gpu = cached;
    int gm = nue_ios_game_mode_info();
    int64_t avail = nue_ios_available_memory();
    float steer = 0;
    bool has_motion = nue_ios_motion_steering(&steer) == 0;
    std::snprintf(buf, sizeof buf,
        "NUE iOS sample\n"
        "state: %s   gpu submit: %s\n"
        "gpu: %s  AppleFamily%d  BC:%d  maxTex:%u  DXT->%s\n"
        "memory: avail %lld MB  pressure: %s  budget hard %llu MB\n"
        "thermal/LPM: %s  lpm:%d  advised max fps: %.0f\n"
        "game mode: plist=%d category=%d os18=%d\n"
        "pads: %d  last input: %s  steering: %s%.2f\n"
        "audio interruptions: %d  latency: %.1f ms\n"
        "safe area: t%.0f l%.0f b%.0f r%.0f\n",
        kLifecycle[g.lifecycle.load()], g.gpu_allowed ? "yes" : "no",
        gpu.name, gpu.highest_apple_family, gpu.supports_bc, gpu.max_texture_size,
        gpu.texture_storage == 0 ? "native" : gpu.texture_storage == 1 ? "ASTC4x4" : gpu.texture_storage == 2 ? "ASTC6x6" : "RGBA8",
        static_cast<long long>(avail >> 20), kLevel[g.pressure.load()], static_cast<unsigned long long>(gpu.hard_memory_limit >> 20),
        kLevel[g.thermal.load()], g.low_power.load(), g.max_fps.load(),
        gm & 1, (gm >> 1) & 1, (gm >> 2) & 1,
        g.pads.load(), kDevice[g.input_device.load()], has_motion ? "" : "n/a ", steer,
        g.audio_interruptions.load(), nue_ios_audio_output_latency() * 1000.0,
        g.insets[0].load(), g.insets[1].load(), g.insets[2].load(), g.insets[3].load());
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "1");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            nue_ios_log(3, "sample", SDL_GetError());
            return 1;
        }
        SDL_Window* window = SDL_CreateWindow("NUE sample", 0, 0, SDL_WINDOW_METAL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) { nue_ios_log(3, "sample", SDL_GetError()); return 1; }
        SDL_MetalView view = SDL_Metal_CreateView(window);
        CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);

        NueIosConfig cfg{};
        cfg.game_dir_name = "NFSU2";
        cfg.audio_mix_policy = 0;
        cfg.enable_motion = 1;
        cfg.enable_crash_handler = 1;
        cfg.enable_memory_poll = 1;
        cfg.target_fps = 60;
        cfg.on_event = on_event;
        nue_ios_init(&cfg);

        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        UIWindow* uiwindow = (__bridge UIWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
        nue_ios_set_window((__bridge void*)uiwindow);
        uint32_t w = 0, h = 0;
        nue_ios_configure_metal_layer((__bridge void*)layer, 0, &w, &h);
        nue_ios_audio_activate(1);

        // Overlay label (UIKit), updated twice a second.
        UILabel* label = [[UILabel alloc] init];
        label.numberOfLines = 0;
        label.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
        label.textColor = [UIColor whiteColor];
        label.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
        [uiwindow.rootViewController.view addSubview:label];

        id<MTLDevice> device = layer.device ?: MTLCreateSystemDefaultDevice();
        layer.device = device;
        id<MTLCommandQueue> queue = [device newCommandQueue];

        bool running = true;
        Uint64 last_label = 0;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) running = false;
                if (ev.type == SDL_EVENT_WINDOW_RESIZED || ev.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED ||
                    ev.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
                    nue_ios_refresh_display();
                    nue_ios_configure_metal_layer((__bridge void*)layer, 0, &w, &h);
                }
                if (ev.type == SDL_EVENT_FINGER_DOWN) nue_ios_motion_recalibrate();   // tap = "this is my neutral wheel angle"
            }
            const Uint64 now = SDL_GetTicks();
            if (now - last_label > 500) {
                last_label = now;
                NSString* text = [NSString stringWithUTF8String:describe().c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{
                    UIEdgeInsets in = uiwindow.safeAreaInsets;
                    label.text = text;
                    label.frame = CGRectMake(in.left + 8, in.top + 8, 420, 0);
                    [label sizeToFit];
                });
            }
            // Policy under test: never commit GPU work while backgrounded (iOS can kill the app for it).
            if (g.gpu_allowed && g.lifecycle == 2) {
                @autoreleasepool {
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    if (drawable) {
                        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                        const double t = now / 1000.0;
                        pass.colorAttachments[0].texture = drawable.texture;
                        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.15 + 0.1 * std::sin(t), 0.3 + 0.2 * std::sin(t * 0.5), 1);
                        id<MTLCommandBuffer> cb = [queue commandBuffer];
                        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
                        [enc endEncoding];
                        [cb presentDrawable:drawable];
                        [cb commit];
                    }
                }
            } else {
                SDL_Delay(50);
            }
        }
        nue_ios_audio_activate(0);
        nue_ios_shutdown();
        SDL_Metal_DestroyView(view);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    return 0;
}
