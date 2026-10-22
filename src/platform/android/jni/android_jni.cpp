// JNI glue: JNI_OnLoad, method cache and the natives of org.nue.engine.NueBridge (Java -> native snapshots).
#include <android/native_window_jni.h>

#include <cstring>
#include <string>
#include <vector>

#include "android_state.hpp"
#include "display_policy.hpp"

namespace nue::android::native {
namespace {

std::string to_std(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

template <size_t N>
void copy_str(char (&dst)[N], const std::string& src) {
    std::strncpy(dst, src.c_str(), N - 1);
    dst[N - 1] = '\0';
}

int64_t g_last_trim_ms = -1000000;

// ---------------------------------------------------------------------------------------- device / storage

void n_setDeviceInfo(JNIEnv* env, jclass, jint sdk, jstring abi, jstring manufacturer, jstring model, jstring soc,
                     jlong total_ram, jint mem_class, jint large_class, jboolean low_ram, jint cores, jint page,
                     jboolean touch, jboolean tv, jboolean chromeos, jboolean emulator) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    NueAndroidDeviceInfo& d = st.device;
    d.struct_size = sizeof d;
    d.sdk_int = sdk;
    d.is_64bit = sizeof(void*) == 8;
    copy_str(d.abi, to_std(env, abi));
    copy_str(d.manufacturer, to_std(env, manufacturer));
    copy_str(d.model, to_std(env, model));
    copy_str(d.soc, to_std(env, soc));
    d.total_ram_bytes = static_cast<uint64_t>(total_ram);
    d.memory_class_mb = mem_class;
    d.large_memory_class_mb = large_class;
    d.low_ram_device = low_ram;
    d.cpu_cores = cores;
    d.page_size = page;
    d.has_touchscreen = touch;
    d.is_tv = tv;
    d.is_chromeos = chromeos;
    d.is_emulator = emulator;
    st.mem.total_ram = d.total_ram_bytes;
    st.mem.memory_class_mb = mem_class;
    st.mem.large_memory_class_mb = large_class;
    st.mem.low_ram_device = low_ram;
}

void n_setStoragePaths(JNIEnv* env, jclass, jstring files, jstring ext, jstring cache, jstring game, jstring saves,
                       jstring config, jstring crash) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    NueAndroidStoragePaths& s = st.storage;
    s.struct_size = sizeof s;
    copy_str(s.files_dir, to_std(env, files));
    copy_str(s.external_files_dir, to_std(env, ext));
    copy_str(s.cache_dir, to_std(env, cache));
    copy_str(s.game_dir, to_std(env, game));
    copy_str(s.saves_dir, to_std(env, saves));
    copy_str(s.config_dir, to_std(env, config));
    copy_str(s.crash_dir, to_std(env, crash));
}

void n_setStorageFree(JNIEnv*, jclass, jlong internal_free, jlong external_free, jboolean complete) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.storage.internal_free_bytes = static_cast<uint64_t>(internal_free);
    st.storage.external_free_bytes = static_cast<uint64_t>(external_free);
    st.storage.game_data_complete = complete;
}

// ---------------------------------------------------------------------------------------- display

void n_setDisplay(JNIEnv* env, jclass, jint w, jint h, jint rotation, jfloat refresh, jfloat dpi, jfloatArray supported,
                  jboolean cutout, jboolean hdr, jboolean wide) {
    State& st = state();
    NueAndroidEvent ev = make_event(NUE_ANDROID_EVENT_DISPLAY, w, h, rotation);
    ev.f[0] = refresh;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        NueAndroidDisplayInfo& d = st.display;
        d.struct_size = sizeof d;
        d.width = w;
        d.height = h;
        d.rotation = rotation;
        d.refresh_hz = refresh;
        d.dpi = dpi;
        d.has_cutout = cutout;
        d.hdr = hdr;
        d.wide_color = wide;
        d.supported_refresh_count = 0;
        if (supported) {
            jsize n = env->GetArrayLength(supported);
            if (n > 16) n = 16;
            env->GetFloatArrayRegion(supported, 0, n, d.supported_refresh);
            d.supported_refresh_count = n;
        }
    }
    st.events.push(ev);
}

void n_setSafeArea(JNIEnv*, jclass, jint l, jint t, jint r, jint b) {
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mu);
        st.display.safe_left = l;
        st.display.safe_top = t;
        st.display.safe_right = r;
        st.display.safe_bottom = b;
    }
    st.events.push(make_event(NUE_ANDROID_EVENT_SAFE_AREA, l, t, r, b));
}

// ---------------------------------------------------------------------------------------- lifecycle

LifeEvent life_event_from(int v, bool* ok) {
    *ok = true;
    switch (v) {
    case NUE_ANDROID_LIFE_CREATE: return LifeEvent::Create;
    case NUE_ANDROID_LIFE_START: return LifeEvent::Start;
    case NUE_ANDROID_LIFE_RESUME: return LifeEvent::Resume;
    case NUE_ANDROID_LIFE_PAUSE: return LifeEvent::Pause;
    case NUE_ANDROID_LIFE_STOP: return LifeEvent::Stop;
    case NUE_ANDROID_LIFE_DESTROY: return LifeEvent::Destroy;
    default: *ok = false; return LifeEvent::Create;
    }
}

// Returns true when the transition asked the engine to release the surface.
bool feed_life(LifeEvent e, int w, int h) {
    State& st = state();
    std::vector<LifeAction> actions;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        st.life.handle(e, w, h, &actions);
    }
    push_life_actions(actions);
    for (LifeAction a : actions)
        if (a == LifeAction::ReleaseSurface) return true;
    return false;
}

void n_onLifecycle(JNIEnv*, jclass, jint event) {
    bool ok;
    LifeEvent e = life_event_from(event, &ok);
    if (!ok) return;
    state().events.push(make_event(NUE_ANDROID_EVENT_LIFECYCLE, event));
    feed_life(e, 0, 0);
}

void n_onSurface(JNIEnv* env, jclass, jint event, jint w, jint h) {
    State& st = state();
    st.events.push(make_event(NUE_ANDROID_EVENT_SURFACE, event, w, h));
    switch (event) {
    case NUE_ANDROID_SURFACE_CREATED: feed_life(LifeEvent::SurfaceCreated, w, h); break;
    case NUE_ANDROID_SURFACE_CHANGED: feed_life(LifeEvent::SurfaceChanged, w, h); break;
    case NUE_ANDROID_SURFACE_DESTROYED: {
        bool release = feed_life(LifeEvent::SurfaceDestroyed, 0, 0);
        // Wait for the engine to destroy its swapchain, but only if an engine is actually polling and never for long:
        // a stuck engine must not freeze the UI thread (ANR after 5 s).
        if (release && now_ms() - st.last_poll_ms.load() < 2000) {
            std::unique_lock<std::mutex> lk(st.ack_mu);
            st.ack_pending = true;
            st.ack_cv.wait_for(lk, std::chrono::milliseconds(750), [&] { return !st.ack_pending; });
            if (st.ack_pending) NLOGW("engine did not acknowledge surface release within 750 ms");
            st.ack_pending = false;
        }
        std::lock_guard<std::mutex> lock(st.mu);
        if (st.native_window) {
            ANativeWindow_release(static_cast<ANativeWindow*>(st.native_window));
            st.native_window = nullptr;
        }
        break;
    }
    default: break;
    }
}

void n_onFocus(JNIEnv*, jclass, jboolean focus) {
    state().events.push(make_event(NUE_ANDROID_EVENT_FOCUS, focus ? 1 : 0));
    feed_life(focus ? LifeEvent::FocusGained : LifeEvent::FocusLost, 0, 0);
}

void n_setTargetFps(JNIEnv*, jclass, jint fps) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.target_fps = fps > 0 ? fps : 60;
}

// ---------------------------------------------------------------------------------------- memory

void n_setMemory(JNIEnv*, jclass, jlong avail, jlong threshold, jboolean low) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.mem.available_ram = static_cast<uint64_t>(avail);
    st.mem.low_threshold = static_cast<uint64_t>(threshold);
    st.mem.low_memory = low;
    // Android only reports increasing pressure; let it decay when the system recovered for 30 s.
    if (!low && st.pressure != MemPressure::None && now_ms() - g_last_trim_ms > 30000) st.pressure = MemPressure::None;
}

void n_onTrim(JNIEnv*, jclass, jint level) {
    State& st = state();
    MemPressure p = pressure_from_trim_level(level);
    {
        std::lock_guard<std::mutex> lock(st.mu);
        if (p > st.pressure || p == MemPressure::Background) st.pressure = p;
        g_last_trim_ms = now_ms();
    }
    st.events.push(make_event(NUE_ANDROID_EVENT_MEMORY, level, static_cast<int32_t>(p)));
}

// ---------------------------------------------------------------------------------------- power / thermal

void n_onThermal(JNIEnv*, jclass, jint status, jfloat headroom) {
    State& st = state();
    ThermalAdvice a;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        st.thermal_status = status;
        st.thermal_headroom = headroom;
        a = st.thermal.update(status, headroom, now_ms());
    }
    NueAndroidEvent ev = make_event(NUE_ANDROID_EVENT_THERMAL, static_cast<int32_t>(a.level), status);
    ev.f[0] = headroom;
    st.events.push(ev);
}

void n_onGameMode(JNIEnv*, jclass, jint mode) {
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mu);
        st.game_mode = mode;
    }
    st.events.push(make_event(NUE_ANDROID_EVENT_GAME_MODE, mode));
}

void n_onPowerSave(JNIEnv*, jclass, jboolean on) {
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mu);
        st.power_save = on;
    }
    st.events.push(make_event(NUE_ANDROID_EVENT_BATTERY_SAVER, on ? 1 : 0));
}

void n_setBattery(JNIEnv*, jclass, jint percent, jboolean charging) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.battery_percent = percent;
    st.charging = charging;
}

void n_setSustainedSupported(JNIEnv*, jclass, jboolean on) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.sustained_supported = on;
}

// ---------------------------------------------------------------------------------------- audio

void n_setAudio(JNIEnv*, jclass, jint rate, jint frames, jboolean low_latency, jboolean pro, jint device_type) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.audio_props.native_sample_rate = rate;
    st.audio_props.frames_per_buffer = frames;
    st.audio_props.low_latency = low_latency;
    st.audio_props.pro = pro;
    st.audio_device_type = device_type;
    st.audio_route = route_from_device_type(device_type);
    st.audio_route_slow = route_has_high_latency(st.audio_route, device_type);
}

void n_onAudioRoute(JNIEnv*, jclass, jint device_type) {
    State& st = state();
    AudioRoute r;
    bool slow;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        st.audio_device_type = device_type;
        st.audio_route = r = route_from_device_type(device_type);
        st.audio_route_slow = slow = route_has_high_latency(r, device_type);
    }
    st.events.push(make_event(NUE_ANDROID_EVENT_AUDIO_ROUTE, static_cast<int32_t>(r), slow ? 1 : 0));
}

void n_onAudioFocus(JNIEnv*, jclass, jint change) {
    State& st = state();
    FocusAction a;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        a = st.audio_focus.on_change(change);
    }
    if (a == FocusAction::None) return;
    st.events.push(make_event(NUE_ANDROID_EVENT_AUDIO_FOCUS, static_cast<int32_t>(a)));
    if (a == FocusAction::Pause) feed_life(LifeEvent::AudioFocusLost, 0, 0);
    else if (a == FocusAction::Resume) feed_life(LifeEvent::AudioFocusGained, 0, 0);
}

// ---------------------------------------------------------------------------------------- input

void n_onInputDevice(JNIEnv*, jclass, jint id, jint added, jint sources, jboolean is_virtual, jint kb_type) {
    State& st = state();
    InputKind k = classify_sources(static_cast<uint32_t>(sources), is_virtual, kb_type);
    st.events.push(make_event(NUE_ANDROID_EVENT_INPUT_DEVICE, id, added, static_cast<int32_t>(k)));
    if (!added) {
        std::lock_guard<std::mutex> lock(st.mu);
        st.last_input.device_removed(k);
    }
}

void n_onInputSample(JNIEnv*, jclass, jint sources, jboolean is_virtual, jint kb_type, jfloat magnitude) {
    State& st = state();
    InputKind k = classify_sources(static_cast<uint32_t>(sources), is_virtual, kb_type);
    bool changed;
    InputKind now;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        changed = st.last_input.note({k, magnitude}, now_ms());
        now = st.last_input.last();
    }
    if (changed) st.events.push(make_event(NUE_ANDROID_EVENT_LAST_INPUT, static_cast<int32_t>(now)));
}

void n_setGamepadConnected(JNIEnv*, jclass, jboolean connected) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.device.has_gamepad_connected = connected;
}

// ---------------------------------------------------------------------------------------- storage import

jint n_importRun(JNIEnv* env, jclass, jstring dest, jstring manifest, jobjectArray paths, jlongArray sizes, jboolean fsync_files) {
    return import_run_from_java(env, dest, manifest, paths, sizes, fsync_files);
}

jint n_importDir(JNIEnv* env, jclass, jstring dir, jstring dest, jstring manifest, jboolean fsync_files) {
    return import_run_from_directory(to_std(env, dir), to_std(env, dest), to_std(env, manifest), fsync_files);
}

void n_importCancel(JNIEnv*, jclass) { state().import_cancel = true; }

void n_onTreePicked(JNIEnv*, jclass, jboolean picked) {
    state().events.push(make_event(NUE_ANDROID_EVENT_TREE_PICKED, picked ? 1 : 0));
}

void n_installCrashHandler(JNIEnv* env, jclass, jstring dir, jstring version) {
    std::string d = to_std(env, dir), v = to_std(env, version);
    nue_android_install_crash_handler(d.c_str(), v.c_str());
}

jint n_chooseDisplayMode(JNIEnv* env, jclass, jintArray ids, jintArray widths, jintArray heights, jfloatArray refresh,
                         jint current_id, jfloat target_fps) {
    jsize n = env->GetArrayLength(ids);
    std::vector<jint> id(n), w(n), h(n);
    std::vector<jfloat> r(n);
    env->GetIntArrayRegion(ids, 0, n, id.data());
    env->GetIntArrayRegion(widths, 0, n, w.data());
    env->GetIntArrayRegion(heights, 0, n, h.data());
    env->GetFloatArrayRegion(refresh, 0, n, r.data());
    std::vector<DisplayMode> modes;
    DisplayMode cur;
    bool have_cur = false;
    for (jsize i = 0; i < n; ++i) {
        DisplayMode m{id[i], w[i], h[i], r[i]};
        modes.push_back(m);
        if (m.id == current_id) {
            cur = m;
            have_cur = true;
        }
    }
    if (!have_cur) return -1;
    return choose_display_mode(modes, cur, target_fps).mode_id;
}

void n_shutdown(JNIEnv*, jclass) {
    adpf_shutdown();
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (st.native_window) {
        ANativeWindow_release(static_cast<ANativeWindow*>(st.native_window));
        st.native_window = nullptr;
    }
}

const JNINativeMethod kMethods[] = {
    {"nativeSetDeviceInfo", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JIIZIIZZZZ)V",
     reinterpret_cast<void*>(n_setDeviceInfo)},
    {"nativeSetStoragePaths",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
     reinterpret_cast<void*>(n_setStoragePaths)},
    {"nativeSetStorageFree", "(JJZ)V", reinterpret_cast<void*>(n_setStorageFree)},
    {"nativeSetDisplay", "(IIIFF[FZZZ)V", reinterpret_cast<void*>(n_setDisplay)},
    {"nativeSetSafeArea", "(IIII)V", reinterpret_cast<void*>(n_setSafeArea)},
    {"nativeOnLifecycle", "(I)V", reinterpret_cast<void*>(n_onLifecycle)},
    {"nativeOnSurface", "(III)V", reinterpret_cast<void*>(n_onSurface)},
    {"nativeOnFocus", "(Z)V", reinterpret_cast<void*>(n_onFocus)},
    {"nativeSetTargetFps", "(I)V", reinterpret_cast<void*>(n_setTargetFps)},
    {"nativeSetMemory", "(JJZ)V", reinterpret_cast<void*>(n_setMemory)},
    {"nativeOnTrim", "(I)V", reinterpret_cast<void*>(n_onTrim)},
    {"nativeOnThermal", "(IF)V", reinterpret_cast<void*>(n_onThermal)},
    {"nativeOnGameMode", "(I)V", reinterpret_cast<void*>(n_onGameMode)},
    {"nativeOnPowerSave", "(Z)V", reinterpret_cast<void*>(n_onPowerSave)},
    {"nativeSetBattery", "(IZ)V", reinterpret_cast<void*>(n_setBattery)},
    {"nativeSetSustainedSupported", "(Z)V", reinterpret_cast<void*>(n_setSustainedSupported)},
    {"nativeSetAudio", "(IIZZI)V", reinterpret_cast<void*>(n_setAudio)},
    {"nativeOnAudioRoute", "(I)V", reinterpret_cast<void*>(n_onAudioRoute)},
    {"nativeOnAudioFocus", "(I)V", reinterpret_cast<void*>(n_onAudioFocus)},
    {"nativeOnInputDevice", "(IIIZI)V", reinterpret_cast<void*>(n_onInputDevice)},
    {"nativeOnInputSample", "(IZIF)V", reinterpret_cast<void*>(n_onInputSample)},
    {"nativeSetGamepadConnected", "(Z)V", reinterpret_cast<void*>(n_setGamepadConnected)},
    {"nativeImportRun", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[JZ)I", reinterpret_cast<void*>(n_importRun)},
    {"nativeImportDir", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)I", reinterpret_cast<void*>(n_importDir)},
    {"nativeImportCancel", "()V", reinterpret_cast<void*>(n_importCancel)},
    {"nativeOnTreePicked", "(Z)V", reinterpret_cast<void*>(n_onTreePicked)},
    {"nativeInstallCrashHandler", "(Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(n_installCrashHandler)},
    {"nativeChooseDisplayMode", "([I[I[I[FIF)I", reinterpret_cast<void*>(n_chooseDisplayMode)},
    {"nativeShutdown", "()V", reinterpret_cast<void*>(n_shutdown)},
};

} // namespace

bool register_natives(JNIEnv* env) {
    State& st = state();
    jclass local = env->FindClass("org/nue/engine/NueBridge");
    if (!local) {
        env->ExceptionClear();
        NLOGE("org.nue.engine.NueBridge not found: is it kept by R8/ProGuard?");
        return false;
    }
    st.bridge = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (env->RegisterNatives(st.bridge, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) {
        env->ExceptionClear();
        NLOGE("RegisterNatives failed (signature mismatch between NueBridge.java and android_jni.cpp)");
        return false;
    }
    auto get = [&](const char* name, const char* sig) {
        jmethodID id = env->GetStaticMethodID(st.bridge, name, sig);
        if (!id) {
            env->ExceptionClear();
            NLOGE("NueBridge.%s%s missing", name, sig);
        }
        return id;
    };
    BridgeMethods& m = st.m;
    m.getSurface = get("getSurface", "()Landroid/view/Surface;");
    m.setFrameRate = get("setFrameRate", "(F)V");
    m.setKeepScreenOn = get("setKeepScreenOn", "(Z)V");
    m.setImmersive = get("setImmersive", "(Z)V");
    m.setPreferredRefresh = get("setPreferredRefresh", "(F)V");
    m.setGameState = get("setGameState", "(ZI)V");
    m.setSustained = get("setSustainedPerformance", "(Z)V");
    m.audioFocus = get("audioFocus", "(I)V");
    m.vibrate = get("vibrate", "(II)V");
    m.pickFolder = get("pickGameFolder", "()V");
    m.importStart = get("importStart", "(Ljava/lang/String;Z)Z");
    m.importCancel = get("importCancelFromNative", "()V");
    m.openImportFd = get("openImportFd", "(I)I");
    return true;
}

} // namespace nue::android::native

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    using namespace nue::android::native;
    state().vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    if (!register_natives(env)) return JNI_ERR;
    NLOGI("libnue_android loaded (api %d)", NUE_ANDROID_API_VERSION);
    return JNI_VERSION_1_6;
}
