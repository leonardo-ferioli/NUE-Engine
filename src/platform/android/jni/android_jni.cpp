// JNI glue: method cache and the natives of org.nue.engine.NueBridge (Java -> native snapshots).
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

void n_setRotation(JNIEnv*, jclass, jfloat x, jfloat y, jfloat z, jfloat w, jlong ts) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    st.rotation[0] = x;
    st.rotation[1] = y;
    st.rotation[2] = z;
    st.rotation[3] = w;
    st.rotation_ts = ts;
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

void n_onTreePicked(JNIEnv* env, jclass, jboolean picked, jstring uri) {
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.mu);
        st.picked_uri = picked ? to_std(env, uri) : std::string();
    }
    st.events.push(make_event(NUE_ANDROID_EVENT_TREE_PICKED, picked ? 1 : 0));
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



} // namespace

namespace {

std::once_flag g_bridge_once;

void init_bridge(JNIEnv* env, jclass cls) {
    State& st = state();
    env->GetJavaVM(&st.vm);
    st.bridge = static_cast<jclass>(env->NewGlobalRef(cls));
    auto get = [&](const char* name, const char* sig) {
        jmethodID id = env->GetStaticMethodID(st.bridge, name, sig);
        if (!id) {
            env->ExceptionClear();
            NLOGE("NueBridge.%s%s missing (R8 removed it? see consumer-rules.pro)", name, sig);
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
    m.hasVibrator = get("hasVibrator", "()Z");
    m.pickFolder = get("pickGameFolder", "()V");
    m.importStart = get("importStart", "(Ljava/lang/String;Z)Z");
    m.importCancel = get("importCancelFromNative", "()V");
    m.openImportFd = get("openImportFd", "(I)I");
    m.enableRotation = get("enableRotation", "(Z)V");
    m.rotationAvailable = get("rotationAvailable", "()Z");
    m.treeOpen = get("treeOpen", "(Ljava/lang/String;)I");
    m.treePath = get("treePath", "(I)Ljava/lang/String;");
    m.treeSize = get("treeSize", "(I)J");
    m.treeFd = get("treeFd", "(I)I");
    m.treeClose = get("treeClose", "()V");
    NLOGI("NUE Android bridge ready (api %d)", NUE_ANDROID_API_VERSION);
}

} // namespace

void ensure_bridge(JNIEnv* env, jclass cls) { std::call_once(g_bridge_once, init_bridge, env, cls); }

// ---------------------------------------------------------------------------------------- exported JNI entry points
// No JNI_OnLoad and no RegisterNatives: SDL3 (linked statically into libnue) defines its own JNI_OnLoad, and the
// JVM resolves these by name from any library loaded by the app's class loader.  The first call (nativeSetDeviceInfo,
// made by NueActivity.onCreate) caches the VM and the NueBridge class.
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetDeviceInfo(JNIEnv* e, jclass c, jint sdk, jstring abi, jstring manufacturer, jstring model, jstring soc, jlong total_ram, jint mem_class, jint large_class, jboolean low_ram, jint cores, jint page, jboolean touch, jboolean tv, jboolean chromeos, jboolean emulator) { ensure_bridge(e, c); return n_setDeviceInfo(e, c, sdk, abi, manufacturer, model, soc, total_ram, mem_class, large_class, low_ram, cores, page, touch, tv, chromeos, emulator); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetStoragePaths(JNIEnv* e, jclass c, jstring files, jstring ext, jstring cache, jstring game, jstring saves, jstring config, jstring crash) { ensure_bridge(e, c); return n_setStoragePaths(e, c, files, ext, cache, game, saves, config, crash); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetStorageFree(JNIEnv* e, jclass c, jlong internal_free, jlong external_free, jboolean complete) { ensure_bridge(e, c); return n_setStorageFree(e, c, internal_free, external_free, complete); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetDisplay(JNIEnv* e, jclass c, jint w, jint h, jint rotation, jfloat refresh, jfloat dpi, jfloatArray supported, jboolean cutout, jboolean hdr, jboolean wide) { ensure_bridge(e, c); return n_setDisplay(e, c, w, h, rotation, refresh, dpi, supported, cutout, hdr, wide); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetSafeArea(JNIEnv* e, jclass c, jint l, jint t, jint r, jint b) { ensure_bridge(e, c); return n_setSafeArea(e, c, l, t, r, b); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnLifecycle(JNIEnv* e, jclass c, jint event) { ensure_bridge(e, c); return n_onLifecycle(e, c, event); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnSurface(JNIEnv* e, jclass c, jint event, jint w, jint h) { ensure_bridge(e, c); return n_onSurface(e, c, event, w, h); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnFocus(JNIEnv* e, jclass c, jboolean focus) { ensure_bridge(e, c); return n_onFocus(e, c, focus); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetTargetFps(JNIEnv* e, jclass c, jint fps) { ensure_bridge(e, c); return n_setTargetFps(e, c, fps); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetMemory(JNIEnv* e, jclass c, jlong avail, jlong threshold, jboolean low) { ensure_bridge(e, c); return n_setMemory(e, c, avail, threshold, low); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnTrim(JNIEnv* e, jclass c, jint level) { ensure_bridge(e, c); return n_onTrim(e, c, level); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnThermal(JNIEnv* e, jclass c, jint status, jfloat headroom) { ensure_bridge(e, c); return n_onThermal(e, c, status, headroom); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnGameMode(JNIEnv* e, jclass c, jint mode) { ensure_bridge(e, c); return n_onGameMode(e, c, mode); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnPowerSave(JNIEnv* e, jclass c, jboolean on) { ensure_bridge(e, c); return n_onPowerSave(e, c, on); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetBattery(JNIEnv* e, jclass c, jint percent, jboolean charging) { ensure_bridge(e, c); return n_setBattery(e, c, percent, charging); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetSustainedSupported(JNIEnv* e, jclass c, jboolean on) { ensure_bridge(e, c); return n_setSustainedSupported(e, c, on); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetAudio(JNIEnv* e, jclass c, jint rate, jint frames, jboolean low_latency, jboolean pro, jint device_type) { ensure_bridge(e, c); return n_setAudio(e, c, rate, frames, low_latency, pro, device_type); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnAudioRoute(JNIEnv* e, jclass c, jint device_type) { ensure_bridge(e, c); return n_onAudioRoute(e, c, device_type); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnAudioFocus(JNIEnv* e, jclass c, jint change) { ensure_bridge(e, c); return n_onAudioFocus(e, c, change); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnInputDevice(JNIEnv* e, jclass c, jint id, jint added, jint sources, jboolean is_virtual, jint kb_type) { ensure_bridge(e, c); return n_onInputDevice(e, c, id, added, sources, is_virtual, kb_type); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnInputSample(JNIEnv* e, jclass c, jint sources, jboolean is_virtual, jint kb_type, jfloat magnitude) { ensure_bridge(e, c); return n_onInputSample(e, c, sources, is_virtual, kb_type, magnitude); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetGamepadConnected(JNIEnv* e, jclass c, jboolean connected) { ensure_bridge(e, c); return n_setGamepadConnected(e, c, connected); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeSetRotation(JNIEnv* e, jclass c, jfloat x, jfloat y, jfloat z, jfloat w, jlong ts) { ensure_bridge(e, c); return n_setRotation(e, c, x, y, z, w, ts); }
extern "C" JNIEXPORT jint JNICALL Java_org_nue_engine_NueBridge_nativeImportRun(JNIEnv* e, jclass c, jstring dest, jstring manifest, jobjectArray paths, jlongArray sizes, jboolean fsync_files) { ensure_bridge(e, c); return n_importRun(e, c, dest, manifest, paths, sizes, fsync_files); }
extern "C" JNIEXPORT jint JNICALL Java_org_nue_engine_NueBridge_nativeImportDir(JNIEnv* e, jclass c, jstring dir, jstring dest, jstring manifest, jboolean fsync_files) { ensure_bridge(e, c); return n_importDir(e, c, dir, dest, manifest, fsync_files); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeImportCancel(JNIEnv* e, jclass c) { ensure_bridge(e, c); return n_importCancel(e, c); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeOnTreePicked(JNIEnv* e, jclass c, jboolean picked, jstring uri) { ensure_bridge(e, c); return n_onTreePicked(e, c, picked, uri); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeInstallCrashHandler(JNIEnv* e, jclass c, jstring dir, jstring version) { ensure_bridge(e, c); return n_installCrashHandler(e, c, dir, version); }
extern "C" JNIEXPORT jint JNICALL Java_org_nue_engine_NueBridge_nativeChooseDisplayMode(JNIEnv* e, jclass c, jintArray ids, jintArray widths, jintArray heights, jfloatArray refresh, jint current_id, jfloat target_fps) { ensure_bridge(e, c); return n_chooseDisplayMode(e, c, ids, widths, heights, refresh, current_id, target_fps); }
extern "C" JNIEXPORT void JNICALL Java_org_nue_engine_NueBridge_nativeShutdown(JNIEnv* e, jclass c) { ensure_bridge(e, c); return n_shutdown(e, c); }

} // namespace nue::android::native

// Referenced from platform_android.cpp so a static libnue keeps this object file (and with it the exported JNI entry points).
extern "C" void nue_android_jni_anchor(void) {}
