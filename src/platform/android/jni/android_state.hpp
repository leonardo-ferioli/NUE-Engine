// Shared native state of the Android backend: snapshots pushed by Java, the event queue, the JVM handle.
#pragma once
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "audio_policy.hpp"
#include "event_queue.hpp"
#include "gpu_caps.hpp"
#include "import_engine.hpp"
#include "input_classify.hpp"
#include "lifecycle_model.hpp"
#include "memory_budget.hpp"
#include "nue_android.h"
#include "thermal_policy.hpp"

#define NUE_LOG_TAG "NUE"
#define NLOGI(...) __android_log_print(ANDROID_LOG_INFO, NUE_LOG_TAG, __VA_ARGS__)
#define NLOGW(...) __android_log_print(ANDROID_LOG_WARN, NUE_LOG_TAG, __VA_ARGS__)
#define NLOGE(...) __android_log_print(ANDROID_LOG_ERROR, NUE_LOG_TAG, __VA_ARGS__)

namespace nue::android::native {

inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Java methods of org.nue.engine.NueBridge that native code calls (all static).
struct BridgeMethods {
    jmethodID getSurface = nullptr;           // ()Landroid/view/Surface;
    jmethodID setFrameRate = nullptr;         // (F)V
    jmethodID setKeepScreenOn = nullptr;      // (Z)V
    jmethodID setImmersive = nullptr;         // (Z)V
    jmethodID setPreferredRefresh = nullptr;  // (F)V
    jmethodID setGameState = nullptr;         // (ZI)V
    jmethodID setSustained = nullptr;         // (Z)V
    jmethodID audioFocus = nullptr;           // (I)V
    jmethodID vibrate = nullptr;              // (II)V
    jmethodID pickFolder = nullptr;           // ()V
    jmethodID importStart = nullptr;          // (Ljava/lang/String;Z)Z
    jmethodID importCancel = nullptr;         // ()V
    jmethodID openImportFd = nullptr;         // (I)I
};

struct State {
    JavaVM* vm = nullptr;
    jclass bridge = nullptr; // global reference to org.nue.engine.NueBridge
    BridgeMethods m;

    EventQueue events;

    // Surface release handshake: the UI thread waits (bounded) until the engine destroyed its swapchain.
    std::mutex ack_mu;
    std::condition_variable ack_cv;
    bool ack_pending = false;
    std::atomic<int64_t> last_poll_ms{-1000000}; // engine liveness: last nue_android_poll_event call

    std::mutex mu; // guards everything below
    NueAndroidDeviceInfo device{};
    NueAndroidDisplayInfo display{};
    NueAndroidStoragePaths storage{};
    AudioProps audio_props;
    int audio_device_type = 0;
    AudioRoute audio_route = AudioRoute::Unknown;
    bool audio_route_slow = false;
    SystemMemory mem;
    MemPressure pressure = MemPressure::None;
    BudgetConfig budget_cfg;
    uint64_t memory_cap = 0; // 0 = BudgetConfig default

    ThermalGovernor thermal;
    int thermal_status = -1;
    float thermal_headroom = std::numeric_limits<float>::quiet_NaN();
    int power_save = 0;
    int game_mode = 0;
    int sustained_supported = 0;
    int battery_percent = -1;
    int charging = 0;

    LifecycleModel life;
    LastInputTracker last_input;
    AudioFocusModel audio_focus;
    int target_fps = 60;

    // GPU probe result (written by the probe thread under mu)
    NueAndroidGpuInfo gpu{};
    std::atomic<bool> gpu_started{false};

    // native window cache (see nue_android_get_native_window)
    void* native_window = nullptr;

    // import
    std::atomic<bool> import_cancel{false};
    std::atomic<bool> import_running{false};
    int import_last_code = 0;
    char import_last_text[200] = "";
};

State& state();

// Attaches the calling thread to the JVM for the lifetime of the object when it is not already attached.
class JniEnv {
public:
    JniEnv();
    ~JniEnv();
    JNIEnv* get() const { return env_; }
    explicit operator bool() const { return env_ != nullptr; }
    JNIEnv* operator->() const { return env_; }

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

// Pushes the events a lifecycle transition produced.
void push_life_actions(const std::vector<LifeAction>& actions);

// JNI registration, called from JNI_OnLoad.
bool register_natives(JNIEnv* env);

// Implemented in android_adpf.cpp / android_gpu_probe.cpp / android_import.cpp / android_crash.cpp.
void adpf_shutdown();
int adpf_available();
void run_gpu_probe(); // blocking, called on the probe thread
int import_run_from_java(JNIEnv* env, jstring dest_root, jstring manifest_text, jobjectArray paths, jlongArray sizes, jboolean fsync_files);
int import_run_from_directory(const std::string& dir, const std::string& dest_root, const std::string& manifest_text, bool fsync_files);
void crash_convert_pending(const char* crash_dir);

} // namespace nue::android::native
