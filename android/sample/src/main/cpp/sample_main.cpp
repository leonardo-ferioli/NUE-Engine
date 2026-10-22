// NUE Android sample: a window, an audio tone, multi-touch, gamepad input, the capability probe printed to logcat
// (tag "NUE") and on screen, the Android event stream, the game-data import buttons.
//
// Everything platform specific goes through nue_android.h; SDL3 is used for window, renderer, audio and input exactly
// as the portable core does.  adb logcat -s NUE
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "nue_android.h"
#include "nue_android_trace.hpp"

namespace {

struct Tone {
    std::atomic<bool> on{false};
    std::atomic<float> freq{440.0f};
    double phase = 0;
    int rate = 48000;
};

struct Finger {
    float x, y, pressure;
};

struct Button {
    const char* label;
    SDL_FRect rect;
};

void SDLCALL tone_callback(void* userdata, SDL_AudioStream* stream, int additional, int /*total*/) {
    Tone* t = static_cast<Tone*>(userdata);
    const int frames = additional / int(sizeof(float));
    if (frames <= 0) return;
    float buf[2048];
    int left = frames;
    const double step = 2.0 * 3.14159265358979323846 * double(t->freq.load()) / t->rate;
    while (left > 0) {
        int n = left < 2048 ? left : 2048;
        const bool on = t->on.load();
        for (int i = 0; i < n; ++i) {
            buf[i] = on ? float(std::sin(t->phase)) * 0.2f : 0.0f;
            t->phase += step;
            if (t->phase > 6.283185307179586) t->phase -= 6.283185307179586;
        }
        SDL_PutAudioStreamData(stream, buf, n * int(sizeof(float)));
        left -= n;
    }
}

const char* kind_name(int k) {
    switch (k) {
    case NUE_ANDROID_INPUT_TOUCH: return "touch";
    case NUE_ANDROID_INPUT_GAMEPAD: return "gamepad";
    case NUE_ANDROID_INPUT_KEYBOARD: return "keyboard";
    case NUE_ANDROID_INPUT_MOUSE: return "mouse";
    case NUE_ANDROID_INPUT_STYLUS: return "stylus";
    case NUE_ANDROID_INPUT_TOUCHPAD: return "touchpad";
    default: return "unknown";
    }
}

std::string describe(const NueAndroidEvent& e) {
    char b[200];
    switch (e.type) {
    case NUE_ANDROID_EVENT_LIFECYCLE: std::snprintf(b, sizeof b, "lifecycle %d", e.i[0]); break;
    case NUE_ANDROID_EVENT_SURFACE: std::snprintf(b, sizeof b, "surface %d %dx%d", e.i[0], e.i[1], e.i[2]); break;
    case NUE_ANDROID_EVENT_FOCUS: std::snprintf(b, sizeof b, "focus %d", e.i[0]); break;
    case NUE_ANDROID_EVENT_MEMORY: std::snprintf(b, sizeof b, "memory trim=%d pressure=%d", e.i[0], e.i[1]); break;
    case NUE_ANDROID_EVENT_THERMAL: std::snprintf(b, sizeof b, "thermal level=%d status=%d headroom=%.2f", e.i[0], e.i[1], e.f[0]); break;
    case NUE_ANDROID_EVENT_GAME_MODE: std::snprintf(b, sizeof b, "game mode %d", e.i[0]); break;
    case NUE_ANDROID_EVENT_BATTERY_SAVER: std::snprintf(b, sizeof b, "battery saver %d", e.i[0]); break;
    case NUE_ANDROID_EVENT_AUDIO_FOCUS: std::snprintf(b, sizeof b, "audio focus action %d", e.i[0]); break;
    case NUE_ANDROID_EVENT_AUDIO_ROUTE: std::snprintf(b, sizeof b, "audio route %d slow=%d", e.i[0], e.i[1]); break;
    case NUE_ANDROID_EVENT_DISPLAY: std::snprintf(b, sizeof b, "display %dx%d rot=%d %.1fHz", e.i[0], e.i[1], e.i[2], e.f[0]); break;
    case NUE_ANDROID_EVENT_SAFE_AREA: std::snprintf(b, sizeof b, "safe area %d %d %d %d", e.i[0], e.i[1], e.i[2], e.i[3]); break;
    case NUE_ANDROID_EVENT_INPUT_DEVICE: std::snprintf(b, sizeof b, "input device %d %s (%s)", e.i[0], e.i[1] ? "added" : "removed", kind_name(e.i[2])); break;
    case NUE_ANDROID_EVENT_LAST_INPUT: std::snprintf(b, sizeof b, "last input: %s", kind_name(e.i[0])); break;
    case NUE_ANDROID_EVENT_IMPORT:
        std::snprintf(b, sizeof b, "import state=%d files %d/%d  %lld/%lld MiB", e.i[0], e.i[1], e.i[2],
                      (long long)(e.l[0] >> 20), (long long)(e.l[1] >> 20));
        break;
    case NUE_ANDROID_EVENT_TREE_PICKED: std::snprintf(b, sizeof b, "folder %s", e.i[0] ? "picked" : "cancelled"); break;
    case NUE_ANDROID_EVENT_ACTION: std::snprintf(b, sizeof b, "ACTION %d", e.i[0]); break;
    default: std::snprintf(b, sizeof b, "event %u", e.type); break;
    }
    return b;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

int main(int, char**) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "game"); // AAudio usage GAME
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        nue_android_log(6, "NUE", SDL_GetError());
        return 1;
    }
    nue_android_probe_gpu();

    SDL_Window* window = SDL_CreateWindow("NUE sample", 0, 0, SDL_WINDOW_FULLSCREEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        nue_android_log(6, "NUE", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    nue_android_set_keep_screen_on(1);
    nue_android_set_frame_rate(60.0f);
    nue_android_set_preferred_refresh(60.0f);

    // Audio: open at the device's native rate (fast AAudio path), 40 ms buffer plan.
    NueAndroidAudioInfo audio{};
    audio.struct_size = sizeof audio;
    nue_android_get_audio_info(44100, 40, &audio);
    Tone tone;
    tone.rate = audio.plan_sample_rate > 0 ? audio.plan_sample_rate : 48000;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = tone.rate;
    char frames[16];
    std::snprintf(frames, sizeof frames, "%d", audio.plan_buffer_frames);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, tone_callback, &tone);
    if (stream) SDL_ResumeAudioStreamDevice(stream);
    nue_android_audio_focus(1);

    // ADPF hint session for this (render) thread, 60 fps.
    int32_t tid = static_cast<int32_t>(syscall(SYS_gettid));
    int adpf = nue_android_hint_session_start(&tid, 1, 14166666);

    std::map<SDL_FingerID, Finger> fingers;
    std::deque<std::string> log;
    auto note = [&](const std::string& s) {
        nue_android_log(4, "NUE", s.c_str());
        log.push_back(s);
        if (log.size() > 12) log.pop_front();
    };
    note(adpf ? "ADPF hint session started" : "ADPF not available");

    bool dumped = false;
    std::string manifest;
    {
        NueAndroidStoragePaths sp{};
        sp.struct_size = sizeof sp;
        if (nue_android_get_storage_paths(&sp) && sp.external_files_dir[0])
            manifest = read_file(std::string(sp.external_files_dir) + "/import_manifest.txt");
    }

    std::string last_input = "unknown";
    uint64_t t_prev = SDL_GetPerformanceCounter();
    const double freq = double(SDL_GetPerformanceFrequency());
    double fps = 0;
    bool running = true;

    while (running) {
        NUE_TRACE_SCOPE("frame");
        const uint64_t t0 = SDL_GetPerformanceCounter();

        NueAndroidEvent ne;
        while (nue_android_poll_event(&ne)) {
            note(describe(ne));
            if (ne.type == NUE_ANDROID_EVENT_ACTION && ne.i[0] == NUE_ANDROID_ACTION_RELEASE_SURFACE)
                nue_android_surface_released(); // the sample keeps no swapchain of its own: SDL's renderer handles it
            if (ne.type == NUE_ANDROID_EVENT_LAST_INPUT) last_input = kind_name(ne.i[0]);
            if (ne.type == NUE_ANDROID_EVENT_AUDIO_FOCUS && stream) {
                if (ne.i[0] == NUE_ANDROID_AUDIO_FOCUS_PAUSE) SDL_PauseAudioStreamDevice(stream);
                if (ne.i[0] == NUE_ANDROID_AUDIO_FOCUS_RESUME) SDL_ResumeAudioStreamDevice(stream);
                if (ne.i[0] == NUE_ANDROID_AUDIO_FOCUS_DUCK) SDL_SetAudioStreamGain(stream, 0.2f);
                if (ne.i[0] == NUE_ANDROID_AUDIO_FOCUS_UNDUCK) SDL_SetAudioStreamGain(stream, 1.0f);
            }
        }
        if (!dumped) {
            NueAndroidGpuInfo g{};
            g.struct_size = sizeof g;
            if (nue_android_get_gpu_info(&g)) {
                nue_android_dump_capabilities();
                dumped = true;
            }
        }

        int w = 0, h = 0;
        SDL_GetRenderOutputSize(renderer, &w, &h);
        const float fw = float(w), fh = float(h);
        Button buttons[3] = {{"PICK FOLDER", {fw * 0.02f, fh * 0.80f, fw * 0.28f, fh * 0.15f}},
                             {"IMPORT", {fw * 0.36f, fh * 0.80f, fw * 0.28f, fh * 0.15f}},
                             {"CANCEL", {fw * 0.70f, fh * 0.80f, fw * 0.28f, fh * 0.15f}}};

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
                fingers[e.tfinger.fingerID] = {e.tfinger.x, e.tfinger.y, e.tfinger.pressure};
                tone.freq = 220.0f + 880.0f * e.tfinger.x;
                if (e.type == SDL_EVENT_FINGER_DOWN) {
                    tone.on = true;
                    const float px = e.tfinger.x * fw, py = e.tfinger.y * fh;
                    for (int i = 0; i < 3; ++i) {
                        const SDL_FRect& r = buttons[i].rect;
                        if (px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h) {
                            tone.on = false;
                            if (i == 0) nue_android_pick_game_folder();
                            if (i == 1) {
                                int ok = nue_android_import_start(manifest.empty() ? nullptr : manifest.c_str(), 0);
                                note(ok ? "import started" : "import not started (pick a folder first)");
                            }
                            if (i == 2) nue_android_import_cancel();
                        }
                    }
                }
                break;
            case SDL_EVENT_FINGER_UP:
                fingers.erase(e.tfinger.fingerID);
                if (fingers.empty()) tone.on = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(e.gdevice.which);
                note("gamepad added");
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                char b[64];
                std::snprintf(b, sizeof b, "pad button %d", int(e.gbutton.button));
                note(b);
                tone.on = true;
                tone.freq = 330.0f + 40.0f * e.gbutton.button;
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_UP: tone.on = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_AC_BACK) note("back key");
                break;
            default: break;
            }
        }

        // ---- draw
        NueAndroidPowerInfo pw{};
        pw.struct_size = sizeof pw;
        nue_android_get_power_info(&pw);
        NueAndroidMemoryBudget mb{};
        mb.struct_size = sizeof mb;
        nue_android_get_memory_budget(&mb);
        NueAndroidDisplayInfo di{};
        di.struct_size = sizeof di;
        nue_android_get_display_info(&di);

        SDL_SetRenderDrawColor(renderer, 18, 22, 32, 255);
        SDL_RenderClear(renderer);
        for (const auto& kv : fingers) {
            const Finger& f = kv.second;
            const float s = 60.0f + 80.0f * f.pressure;
            SDL_FRect r{f.x * fw - s / 2, f.y * fh - s / 2, s, s};
            SDL_SetRenderDrawColor(renderer, 90, 200, 120, 255);
            SDL_RenderFillRect(renderer, &r);
        }
        for (const Button& b : buttons) {
            SDL_SetRenderDrawColor(renderer, 60, 70, 110, 255);
            SDL_RenderFillRect(renderer, &b.rect);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, b.rect.x + 12, b.rect.y + b.rect.h / 2 - 4, b.label);
        }
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const float sx = float(di.safe_left) + 8.0f;
        float y = float(di.safe_top) + 8.0f;
        auto line = [&](const char* fmt, auto... a) {
            char b[200];
            std::snprintf(b, sizeof b, fmt, a...);
            SDL_RenderDebugText(renderer, sx, y, b);
            y += 12.0f;
        };
        line("NUE Android sample  %.1f fps  %dx%d  %.1f Hz", fps, w, h, di.refresh_hz);
        line("last input: %s   fingers: %d   tone: %s", last_input.c_str(), int(fingers.size()), tone.on ? "on" : "off");
        line("thermal %d (status %d, headroom %.2f)  powersave %d  gamemode %d  adpf %d", pw.thermal_level, pw.thermal_status,
             pw.thermal_headroom, pw.power_save, pw.game_mode, pw.adpf_available);
        line("rss %llu MiB / hard %llu MiB  pressure %d", (unsigned long long)(mb.rss_bytes >> 20),
             (unsigned long long)(mb.process_hard_limit >> 20), mb.pressure);
        line("audio %d Hz x %d frames  low-latency %d  route %d", audio.plan_sample_rate, audio.plan_buffer_frames, audio.low_latency,
             audio.route);
        for (const std::string& s : log) line("%s", s.c_str());
        SDL_RenderPresent(renderer);

        // ---- timing: work duration for ADPF is everything before the vsync wait inside Present
        const uint64_t t1 = SDL_GetPerformanceCounter();
        if (adpf) nue_android_hint_report(int64_t(double(t1 - t0) / freq * 1e9));
        const double dt = double(t1 - t_prev) / freq;
        t_prev = t1;
        if (dt > 0) fps = fps * 0.95 + (1.0 / dt) * 0.05;
    }

    nue_android_hint_session_stop();
    nue_android_audio_focus(0);
    if (stream) SDL_DestroyAudioStream(stream);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
