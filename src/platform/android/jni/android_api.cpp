// Implementation of the public C API of nue_android.h (everything except ADPF, trace and crash which have their own files).
#include <android/native_window_jni.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include "android_state.hpp"

using namespace nue::android;
using namespace nue::android::native;

namespace {

template <typename... A>
void call_void(jmethodID id, A... args) {
    State& st = state();
    JniEnv env;
    if (!env || !id || !st.bridge) return;
    env->CallStaticVoidMethod(st.bridge, id, args...);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

uint64_t read_rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    int n = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);
    if (n != 2) return 0;
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}

// Structs are versioned by their leading size member: callers set struct_size, we copy at most that much.
template <typename T>
bool copy_out(const T& src, T* dst) {
    if (!dst || dst->struct_size == 0) return false;
    uint32_t want = dst->struct_size;
    std::memcpy(dst, &src, want < sizeof(T) ? want : sizeof(T));
    dst->struct_size = want;
    return true;
}

} // namespace

extern "C" {

int nue_android_poll_event(NueAndroidEvent* out) {
    State& st = state();
    st.last_poll_ms = now_ms();
    return out && st.events.poll(out) ? 1 : 0;
}

void nue_android_surface_released(void) {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.ack_mu);
    st.ack_pending = false;
    st.ack_cv.notify_all();
}
uint32_t nue_android_dropped_events(void) { return state().events.dropped(); }

int nue_android_get_device_info(NueAndroidDeviceInfo* out) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    return copy_out(st.device, out) ? 1 : 0;
}

int nue_android_get_display_info(NueAndroidDisplayInfo* out) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    return copy_out(st.display, out) ? 1 : 0;
}

void nue_android_probe_gpu(void) {
    State& st = state();
    if (st.gpu_started.exchange(true)) return;
    std::thread([] { run_gpu_probe(); }).detach();
}

int nue_android_get_gpu_info(NueAndroidGpuInfo* out) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!copy_out(st.gpu, out)) return 0;
    return st.gpu.probed ? 1 : 0;
}

int nue_android_recommend_dxt(NueAndroidDxtStrategy* out) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.gpu.probed) return 0;
    uint32_t caps = st.gpu.prefer_vulkan ? st.gpu.vk_tex_caps : st.gpu.gles_tex_caps;
    if (out) *out = static_cast<NueAndroidDxtStrategy>(plan_dxt(caps));
    return 1;
}

int nue_android_get_memory_budget(NueAndroidMemoryBudget* out) {
    State& st = state();
    SystemMemory mem;
    MemPressure pressure;
    BudgetConfig cfg;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        mem = st.mem;
        pressure = st.pressure;
        cfg = st.budget_cfg;
        if (st.memory_cap) cfg.process_cap = st.memory_cap;
    }
    mem.rss = read_rss_bytes();
    Budget b = compute_budget(mem, pressure, cfg);
    NueAndroidMemoryBudget r{};
    r.struct_size = sizeof r;
    r.process_hard_limit = b.hard_limit;
    r.process_soft_limit = b.soft_limit;
    r.texture_budget = b.texture_budget;
    r.rss_bytes = mem.rss;
    r.system_available = mem.available_ram;
    r.pressure = static_cast<int32_t>(b.pressure);
    r.bytes_to_release = static_cast<int32_t>(b.bytes_to_release >> 20);
    return copy_out(r, out) ? 1 : 0;
}

void nue_android_set_memory_cap(uint64_t bytes) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.memory_cap = bytes;
}

void* nue_android_get_native_window(void) {
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mu);
        if (st.native_window) return st.native_window;
    }
    JniEnv env;
    if (!env || !st.m.getSurface) return nullptr;
    jobject surface = env->CallStaticObjectMethod(st.bridge, st.m.getSurface);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    if (!surface) return nullptr;
    ANativeWindow* w = ANativeWindow_fromSurface(env.get(), surface);
    env->DeleteLocalRef(surface);
    if (!w) return nullptr;
    std::lock_guard<std::mutex> lock(st.mu);
    if (st.native_window) { // raced with another caller
        ANativeWindow_release(w);
        return st.native_window;
    }
    st.native_window = w; // released on SURFACE_DESTROYED
    return w;
}

void nue_android_set_frame_rate(float fps) { call_void(state().m.setFrameRate, static_cast<jfloat>(fps)); }
void nue_android_set_keep_screen_on(int on) { call_void(state().m.setKeepScreenOn, static_cast<jboolean>(on != 0)); }
void nue_android_set_immersive(int on) { call_void(state().m.setImmersive, static_cast<jboolean>(on != 0)); }
void nue_android_set_preferred_refresh(float fps) { call_void(state().m.setPreferredRefresh, static_cast<jfloat>(fps)); }

int nue_android_get_power_info(NueAndroidPowerInfo* out) {
    State& st = state();
    NueAndroidPowerInfo p{};
    {
        std::lock_guard<std::mutex> lock(st.mu);
        const ThermalAdvice& a = st.thermal.advice();
        p.thermal_level = static_cast<int32_t>(a.level);
        p.thermal_status = st.thermal_status;
        p.thermal_headroom = st.thermal_headroom;
        p.power_save = st.power_save;
        p.game_mode = st.game_mode;
        p.sustained_perf_supported = st.sustained_supported;
        p.battery_percent = st.battery_percent;
        p.charging = st.charging;
        p.suggested_fps_cap = a.fps_cap;
        p.suggested_quality = a.quality_scale;
        // power saver halves the quality target on top of the thermal advice
        if (st.power_save) {
            p.suggested_quality *= 0.85f;
            if (p.suggested_fps_cap == 0 || p.suggested_fps_cap > 30) p.suggested_fps_cap = 30;
        }
    }
    p.adpf_available = adpf_available();
    return copy_out(p, out) ? 1 : 0;
}

void nue_android_set_game_state(int is_loading, int mode) {
    call_void(state().m.setGameState, static_cast<jboolean>(is_loading != 0), static_cast<jint>(mode));
}
void nue_android_set_sustained_performance(int on) { call_void(state().m.setSustained, static_cast<jboolean>(on != 0)); }

int nue_android_get_audio_info(int desired_rate, int target_latency_ms, NueAndroidAudioInfo* out) {
    State& st = state();
    NueAndroidAudioInfo a{};
    {
        std::lock_guard<std::mutex> lock(st.mu);
        a.native_sample_rate = st.audio_props.native_sample_rate;
        a.frames_per_buffer = st.audio_props.frames_per_buffer;
        a.low_latency = st.audio_props.low_latency;
        a.pro = st.audio_props.pro;
        a.route = static_cast<int32_t>(st.audio_route);
        a.route_high_latency = st.audio_route_slow;
        AudioPlan p = plan_audio(st.audio_props, desired_rate, target_latency_ms);
        a.plan_sample_rate = p.sample_rate;
        a.plan_buffer_frames = p.buffer_frames;
        a.plan_needs_resample = p.needs_resample;
    }
    a.struct_size = sizeof a;
    return copy_out(a, out) ? 1 : 0;
}

void nue_android_audio_focus(int request) { call_void(state().m.audioFocus, static_cast<jint>(request)); }

int nue_android_audio_focus_state(void) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    return st.audio_focus.paused() ? 2 : (st.audio_focus.ducked() ? 1 : 0);
}

int nue_android_has_vibrator(void) {
    State& st = state();
    JniEnv env;
    if (!env || !st.m.hasVibrator) return 0;
    jboolean r = env->CallStaticBooleanMethod(st.bridge, st.m.hasVibrator);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    return r ? 1 : 0;
}

int nue_android_last_input_kind(void) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    return static_cast<int>(st.last_input.last());
}

int nue_android_rotation_available(void) {
    State& st = state();
    JniEnv env;
    if (!env || !st.m.rotationAvailable) return 0;
    jboolean r = env->CallStaticBooleanMethod(st.bridge, st.m.rotationAvailable);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    return r ? 1 : 0;
}

void nue_android_enable_rotation(int on) { call_void(state().m.enableRotation, static_cast<jboolean>(on != 0)); }

int nue_android_get_rotation(float q[4], int64_t* ts) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (st.rotation_ts == 0 || !q) return 0;
    std::memcpy(q, st.rotation, sizeof st.rotation);
    if (ts) *ts = st.rotation_ts;
    return 1;
}

void nue_android_vibrate(int ms, int amplitude) { call_void(state().m.vibrate, static_cast<jint>(ms), static_cast<jint>(amplitude)); }

int nue_android_get_storage_paths(NueAndroidStoragePaths* out) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    return copy_out(st.storage, out) ? 1 : 0;
}

size_t nue_android_get_picked_uri(char* buffer, size_t capacity) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (buffer && capacity) {
        std::strncpy(buffer, st.picked_uri.c_str(), capacity - 1);
        buffer[capacity - 1] = '\0';
    }
    return st.picked_uri.size() + 1;
}

void nue_android_pick_game_folder(void) { call_void(state().m.pickFolder); }

int nue_android_import_start(const char* manifest, int use_external) {
    State& st = state();
    JniEnv env;
    if (!env || !st.m.importStart || st.import_running.load()) return 0;
    st.import_cancel = false;
    jstring m = manifest ? env->NewStringUTF(manifest) : nullptr;
    jboolean ok = env->CallStaticBooleanMethod(st.bridge, st.m.importStart, m, static_cast<jboolean>(use_external != 0));
    if (m) env->DeleteLocalRef(m);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    return ok ? 1 : 0;
}

void nue_android_import_cancel(void) {
    State& st = state();
    st.import_cancel = true;
    call_void(st.m.importCancel);
}

int nue_android_import_from_directory(const char* dir, const char* manifest, int use_external) {
    State& st = state();
    if (!dir || st.import_running.exchange(true)) return 0;
    std::string dest;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        if (use_external && st.storage.external_files_dir[0])
            dest = join_path(st.storage.external_files_dir, "gamedata");
        else
            dest = st.storage.game_dir;
    }
    if (dest.empty()) {
        st.import_running = false;
        return 0;
    }
    std::string d = dir, m = manifest ? manifest : "";
    st.import_cancel = false;
    std::thread([d, dest, m] {
        import_run_from_directory(d, dest, m, true);
        state().import_running = false;
    }).detach();
    return 1;
}

int nue_android_import_last_result(char* text, size_t size) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (text && size) {
        std::strncpy(text, st.import_last_text, size - 1);
        text[size - 1] = '\0';
    }
    return st.import_last_code;
}

void nue_android_log(int priority, const char* tag, const char* message) {
    __android_log_write(priority, tag ? tag : NUE_LOG_TAG, message ? message : "");
}

void nue_android_dump_capabilities(void) {
    State& st = state();
    NueAndroidDeviceInfo d{};
    d.struct_size = sizeof d;
    NueAndroidDisplayInfo disp{};
    disp.struct_size = sizeof disp;
    NueAndroidGpuInfo g{};
    g.struct_size = sizeof g;
    NueAndroidMemoryBudget mb{};
    mb.struct_size = sizeof mb;
    NueAndroidAudioInfo au{};
    au.struct_size = sizeof au;
    NueAndroidPowerInfo pw{};
    pw.struct_size = sizeof pw;
    NueAndroidStoragePaths sp{};
    sp.struct_size = sizeof sp;
    nue_android_get_device_info(&d);
    nue_android_get_display_info(&disp);
    nue_android_get_gpu_info(&g);
    nue_android_get_memory_budget(&mb);
    nue_android_get_audio_info(44100, 40, &au);
    nue_android_get_power_info(&pw);
    nue_android_get_storage_paths(&sp);

    NLOGI("== NUE capability report (api %d) ==", NUE_ANDROID_API_VERSION);
    NLOGI("device: %s %s soc=%s sdk=%d abi=%s 64bit=%d cores=%d page=%d ram=%llu MiB memclass=%d/%d lowram=%d tv=%d chromeos=%d emulator=%d touch=%d",
          d.manufacturer, d.model, d.soc, d.sdk_int, d.abi, d.is_64bit, d.cpu_cores, d.page_size,
          (unsigned long long)(d.total_ram_bytes >> 20), d.memory_class_mb, d.large_memory_class_mb, d.low_ram_device,
          d.is_tv, d.is_chromeos, d.is_emulator, d.has_touchscreen);
    NLOGI("display: %dx%d rot=%d %.2f Hz dpi=%.0f safe=[%d %d %d %d] cutout=%d hdr=%d widecolor=%d", disp.width, disp.height,
          disp.rotation, disp.refresh_hz, disp.dpi, disp.safe_left, disp.safe_top, disp.safe_right, disp.safe_bottom,
          disp.has_cutout, disp.hdr, disp.wide_color);
    std::string rates;
    for (int i = 0; i < disp.supported_refresh_count; ++i) {
        char b[24];
        std::snprintf(b, sizeof b, "%s%.1f", i ? ", " : "", disp.supported_refresh[i]);
        rates += b;
    }
    NLOGI("display: supported refresh at this resolution: %s", rates.c_str());
    if (g.probed) {
        NLOGI("gpu: %s vendor=%s(0x%x) vulkan=%d api=%u.%u driver=0x%x devlocal=%llu MiB", g.device_name,
              to_string(static_cast<GpuVendor>(g.vendor)), g.vendor_id, g.vulkan_present, vk_major(g.vulkan_api_version),
              vk_minor(g.vulkan_api_version), g.vulkan_driver_version, (unsigned long long)(g.device_local_bytes >> 20));
        NLOGI("gpu: gles %d.%d renderer='%s' version='%s' maxtex=%d msaa=%d", g.gles_major, g.gles_minor, g.gles_renderer,
              g.gles_version, g.max_texture_size, g.max_msaa);
        auto caps = [](uint32_t c) {
            std::string s;
            auto add = [&](uint32_t bit, const char* n) { if (c & bit) { s += n; s += ' '; } };
            add(NUE_ANDROID_TEX_BC1_3, "BC1-3(DXT)");
            add(NUE_ANDROID_TEX_BC4_5, "BC4-5");
            add(NUE_ANDROID_TEX_BC6H_7, "BC6H-7");
            add(NUE_ANDROID_TEX_ETC2, "ETC2");
            add(NUE_ANDROID_TEX_ASTC_LDR, "ASTC");
            add(NUE_ANDROID_TEX_D24S8, "D24S8");
            add(NUE_ANDROID_TEX_D32S8, "D32S8");
            add(NUE_ANDROID_TEX_BGRA8, "BGRA8");
            add(NUE_ANDROID_TEX_ANISO, "aniso");
            return s;
        };
        NLOGI("gpu: vulkan texture caps: %s", caps(g.vk_tex_caps).c_str());
        NLOGI("gpu: gles texture caps: %s", caps(g.gles_tex_caps).c_str());
        NueAndroidDxtStrategy dxt;
        if (nue_android_recommend_dxt(&dxt)) NLOGI("gpu: DXT policy: %s", to_string(static_cast<DxtStrategy>(dxt)));
        NLOGI("gpu: recommended API: %s (%s)", g.prefer_vulkan ? "Vulkan" : "OpenGL ES", g.note);
    } else {
        NLOGI("gpu: probe not finished");
    }
    NLOGI("memory: rss=%llu MiB hard=%llu MiB soft=%llu MiB textures=%llu MiB avail=%llu MiB pressure=%d",
          (unsigned long long)(mb.rss_bytes >> 20), (unsigned long long)(mb.process_hard_limit >> 20),
          (unsigned long long)(mb.process_soft_limit >> 20), (unsigned long long)(mb.texture_budget >> 20),
          (unsigned long long)(mb.system_available >> 20), mb.pressure);
    NLOGI("audio: native %d Hz, burst %d frames, lowlat=%d pro=%d route=%d slow=%d; plan for 44100 Hz/40 ms: %d Hz x %d frames resample=%d",
          au.native_sample_rate, au.frames_per_buffer, au.low_latency, au.pro, au.route, au.route_high_latency,
          au.plan_sample_rate, au.plan_buffer_frames, au.plan_needs_resample);
    NLOGI("power: thermal=%d(status %d, headroom %.2f) powersave=%d gamemode=%d sustained=%d adpf=%d battery=%d%% charging=%d cap=%d q=%.2f",
          pw.thermal_level, pw.thermal_status, pw.thermal_headroom, pw.power_save, pw.game_mode, pw.sustained_perf_supported,
          pw.adpf_available, pw.battery_percent, pw.charging, pw.suggested_fps_cap, pw.suggested_quality);
    NLOGI("storage: files=%s ext=%s game=%s complete=%d free(int)=%llu MiB free(ext)=%llu MiB", sp.files_dir,
          sp.external_files_dir, sp.game_dir, sp.game_data_complete, (unsigned long long)(sp.internal_free_bytes >> 20),
          (unsigned long long)(sp.external_free_bytes >> 20));
    (void)st;
}

} // extern "C"
