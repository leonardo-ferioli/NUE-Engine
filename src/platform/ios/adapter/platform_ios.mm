// NUE platform layer for iOS: implements nue_platform_ios_get_ops() (src/core/internal/platform_ops.h) on top of the iOS backend glue
// (nue_ios.h, apple/*.mm) and the host-tested mapping logic (logic/nue_map.*).
// SDL3 (the shared backend) owns window, touch, keyboard, mouse, gamepad, sensors and the audio device; this layer adds power/thermal,
// storage rules, import, thread QoS, haptics, safe area, system info, crash reports and the os_log sink.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <os/log.h>
#include <pthread.h>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/ucontext.h>
#include <unistd.h>
#include <vector>

#include "internal/platform_ops.h"
#include "nue_ext_ios.h"
#include "nue_ios.h"
#include "nue_map.hpp"
#include "storage.hpp"

using namespace nue::ios;

namespace {

constexpr const char* kTag = "ios";

// ---------------------------------------------------------------------------------------------------------------------------- state
std::atomic<bool> g_inited{false};
std::atomic<bool> g_power_dirty{true};
std::atomic<int64_t> g_last_poll_ns{0};
PowerSnapshot g_power_last{};
bool g_power_have_last = false;
std::atomic<bool> g_keep_awake_active{false};
nue_power_request g_request{};

nue_result string_out(const std::string& s, char* buffer, size_t capacity, size_t* out_size) {
    if (out_size) *out_size = s.size() + 1;
    if (buffer && capacity) {
        const size_t n = s.size() < capacity - 1 ? s.size() : capacity - 1;
        std::memcpy(buffer, s.data(), n);
        buffer[n] = 0;
    }
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------------------------------- events from the glue
std::atomic<int> g_last_pressure{0};   // MemoryPressure as int, for memory_info_ex

void push_memory_pressure(MemoryPressure level, int64_t available) {
    g_last_pressure = static_cast<int>(level);
#if NUE_IOS_API_011
    nue_event ev{};
    ev.type = NUE_EVENT_MEMORY_PRESSURE;
    ev.payload.memory = make_memory_payload(level, available > 0 ? static_cast<uint64_t>(available) : 0);
    nue_internal_push_event(&ev);
#endif
    if (level == MemoryPressure::high || level == MemoryPressure::critical) {
        // Kept for hosts that only listen to the older "release caches" event.
        nue_event low{};
        low.type = NUE_EVENT_APP_LOW_MEMORY;
        nue_internal_push_event(&low);
        NUE_LOGW(kTag, "memory pressure %s, %lld MB left", memory_pressure_name(level), (long long)(available >> 20));
    }
}

void on_ios_event(const NueIosEvent* e, void*) {
    switch (e->kind) {
    case NUE_IOS_EVENT_MEMORY_PRESSURE:
        // Kernel pressure source and the os_proc_available_memory() poll: they fire before the OS warns.
        push_memory_pressure(static_cast<MemoryPressure>(std::min<int64_t>(3, std::max<int64_t>(0, e->a))), e->b);
        break;
    case NUE_IOS_EVENT_THERMAL:
    case NUE_IOS_EVENT_POWER:
        g_power_dirty = true;
        break;
    case NUE_IOS_EVENT_LIFECYCLE: {
        NUE_LOGD(kTag, "lifecycle state %d actions 0x%x", (int)e->a, (unsigned)e->b);
#if NUE_IOS_API_011
        // This layer owns the lifecycle (owns_lifecycle): the state machine's state is the NUE app state. The core emits
        // PAUSING / PAUSED / RESUMING / RESUMED from it, synchronously, so the game stops audio and GPU work inside this call.
        nue_internal_set_app_state(map_app_state(static_cast<LifecycleState>(e->a)));
#endif
        // UIApplicationDidReceiveMemoryWarning: the OS is asking for memory now.
        if (static_cast<uint32_t>(e->b) & act_emit_low_memory) push_memory_pressure(MemoryPressure::high, nue_ios_memory_available());
        break;
    }
    case NUE_IOS_EVENT_AUDIO_INTERRUPTION:
        NUE_LOGI(kTag, "audio interruption %s", e->a ? "began" : "ended");
#if NUE_IOS_API_011
        {
            // "ended" without shouldResume (the user started other audio) is not forwarded: the game stays paused until the player acts.
            if (e->a || e->b) {
                nue_event ev{};
                ev.type = e->a ? NUE_EVENT_AUDIO_INTERRUPTED : NUE_EVENT_AUDIO_RESUMED;
                nue_internal_push_event(&ev);
            }
        }
#endif
        break;
    case NUE_IOS_EVENT_AUDIO_ROUTE:
        NUE_LOGI(kTag, "audio route changed (kind %d%s)", (int)e->a, e->b ? ", device removed" : "");
#if NUE_IOS_API_011
        {
            nue_event ev{};
            ev.type = NUE_EVENT_AUDIO_ROUTE_CHANGED;   // payload.audio_device: output (is_capture = 0), device_id 0 = the new default
            nue_internal_push_event(&ev);
        }
#endif
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------- power
PowerSnapshot read_power() {
    NSProcessInfo* pi = [NSProcessInfo processInfo];
    UIDevice* d = [UIDevice currentDevice];
    if (!d.batteryMonitoringEnabled) d.batteryMonitoringEnabled = YES;
    PowerSnapshot s;
    s.thermal = map_thermal(thermal_from_raw(static_cast<int>(pi.thermalState)));
    s.state = map_battery_state(static_cast<int>(d.batteryState));
    s.percent = battery_percent(d.batteryLevel);
    s.low_power = pi.lowPowerModeEnabled;
    return s;
}

void push_power_event(uint32_t type, const PowerSnapshot& s) {
    nue_event ev{};
    ev.type = type;
    ev.payload.power = make_power_payload(s.thermal, s.state, s.percent, s.low_power, NUE_GAME_MODE_UNKNOWN);
    nue_internal_push_event(&ev);
}

void poll_power(bool force) {
    const int64_t now = nue_time_ns();
    if (!force && !g_power_dirty.exchange(false) && now - g_last_poll_ns.load() < 250000000ll) return;
    g_last_poll_ns = now;
    const PowerSnapshot s = read_power();
    if (g_power_have_last) {
        const unsigned c = power_changes(g_power_last, s);
        if (c & change_power) push_power_event(NUE_EVENT_POWER_CHANGED, s);
        if (c & change_thermal) push_power_event(NUE_EVENT_THERMAL_CHANGED, s);
        if (c & change_low_power) push_power_event(NUE_EVENT_LOW_POWER_MODE_CHANGED, s);
    }
    g_power_last = s;
    g_power_have_last = true;
}

nue_result ios_power_info(nue_power_info* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    const PowerSnapshot s = read_power();
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = size;
    out->state = s.state;
    out->battery_percent = s.percent;
    out->seconds_left = -1;
    out->on_ac_power = on_ac_power(s.state) ? NUE_TRUE : NUE_FALSE;
    out->low_power_mode = s.low_power ? NUE_TRUE : NUE_FALSE;
    out->thermal = s.thermal;
    out->game_mode = NUE_GAME_MODE_UNKNOWN;   // no public API reports whether Game Mode is active
    out->thermal_headroom = -1.0f;
    out->display_keep_awake_active = g_keep_awake_active.load() ? NUE_TRUE : NUE_FALSE;
    return NUE_OK;
}

void apply_fps(float fps) {
    // UIKit (CADisplayLink) is main-thread only. On the main thread apply at once so shutdown ordering is exact.
    auto apply = ^{
        if (fps > 0) nue_ios_set_frame_rate(fps, fps, fps);
        else nue_ios_set_frame_rate(0, 0, 0);
    };
    if ([NSThread isMainThread]) apply(); else dispatch_async(dispatch_get_main_queue(), apply);
}

nue_result ios_power_apply(const nue_power_request* request, nue_power_applied* out_applied) {
    if (!request) return NUE_ERR_INVALID_ARGUMENT;
    const PowerPlan plan = plan_power_request(*request);
    nue_ios_set_keep_awake(plan.keep_awake ? 1 : 0);
    g_keep_awake_active = plan.keep_awake;
    apply_fps(plan.fps);
    g_request = *request;
    if (out_applied) *out_applied = plan.applied;
    if (plan.release_all) return NUE_OK;
    return plan.partial ? NUE_OK_PARTIAL : NUE_OK;
}

// ---------------------------------------------------------------------------------------------------------------------------- system
nue_result ios_system_info(nue_system_info* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    NueIosSystemInfo s;
    if (nue_ios_system_info(&s) != 0) return NUE_ERR_FAILED;
    out->os = NUE_OS_IOS;
    std::snprintf(out->os_name, sizeof out->os_name, "%s %s", s.idiom == 2 ? "iPadOS" : "iOS", s.os_version);
    std::snprintf(out->os_version, sizeof out->os_version, "%s", s.os_version);
    out->os_build = 0;
    out->arch = NUE_ARCH_KIND_ARM64;
    out->running_under_translation = NUE_FALSE;
    switch (s.idiom) {
    case 1: out->device_class = NUE_DEVICE_PHONE; break;
    case 2: out->device_class = NUE_DEVICE_TABLET; break;
    case 3: out->device_class = NUE_DEVICE_TV; break;
    default: out->device_class = NUE_DEVICE_UNKNOWN; break;
    }
    std::snprintf(out->device_model, sizeof out->device_model, "%s", s.device_model);
    std::snprintf(out->cpu_name, sizeof out->cpu_name, "%s", s.cpu_name);
    out->cpu_logical_cores = s.logical_cores;
    out->cpu_physical_cores = s.physical_cores;
    out->cpu_performance_cores = s.performance_cores;
    out->cpu_efficiency_cores = s.efficiency_cores;
    out->cpu_max_mhz = s.max_mhz;
    out->cpu_features = NUE_CPU_NEON;
    out->memory_total_bytes = s.physical_memory;
    const int64_t avail = nue_ios_memory_available();
    out->memory_available_bytes = avail > 0 ? static_cast<uint64_t>(avail) : 0;
    NueIosGpuInfo gpu;
    if (nue_ios_probe_gpu(&gpu) == 0) {
        out->gpu_count = 1;
        std::snprintf(out->gpus[0].name, sizeof out->gpus[0].name, "%s", gpu.name);
        out->gpus[0].shared_memory_bytes = gpu.recommended_max_working_set;
        out->gpus[0].is_integrated = NUE_TRUE;
        out->gpus[0].is_default = NUE_TRUE;
    }
    nue_ios_locale(out->locale, sizeof out->locale);
    out->dark_mode = s.dark_mode ? NUE_TRUE : NUE_FALSE;
    return NUE_OK;
}

nue_result ios_memory_info(uint64_t* total, uint64_t* available) {
    NueIosSystemInfo s;
    if (nue_ios_system_info(&s) != 0) return NUE_ERR_FAILED;
    if (total) *total = s.physical_memory;
    if (available) {
        const int64_t a = nue_ios_memory_available();
        *available = a > 0 ? static_cast<uint64_t>(a) : 0;
    }
    return NUE_OK;
}

nue_result ios_system_locale(char* buffer, size_t capacity) {
    return nue_ios_locale(buffer, capacity) >= 0 ? NUE_OK : NUE_ERR_FAILED;
}

// ---------------------------------------------------------------------------------------------------------------------------- storage
NSString* base_dir_for(nue_root_kind kind) {
    NSSearchPathDirectory d = NSApplicationSupportDirectory;
    switch (kind) {
    case NUE_ROOT_DATA: d = NSDocumentDirectory; break;
    case NUE_ROOT_CACHE: d = NSCachesDirectory; break;
    case NUE_ROOT_TEMP: return NSTemporaryDirectory();
    default: break;
    }
    return NSSearchPathForDirectoriesInDomains(d, NSUserDomainMask, YES).firstObject;
}

nue_result ios_storage_root(nue_root_kind kind, const nue_init_desc* desc, char* buffer, size_t capacity, size_t* out_size) {
    std::string path;
    const char* org = desc ? desc->org_name : nullptr;
    const char* app = desc ? desc->app_id : nullptr;
    if (kind == NUE_ROOT_DATA && desc && desc->game_data_path && desc->game_data_path[0]) {
        path = desc->game_data_path;
    } else if ((kind == NUE_ROOT_USER || kind == NUE_ROOT_SETTINGS) && desc && desc->user_data_path && desc->user_data_path[0]) {
        path = kind == NUE_ROOT_SETTINGS ? path_join(desc->user_data_path, "settings") : std::string(desc->user_data_path);
    } else {
        const std::string sub = storage_subpath(kind, org, app);
        if (sub.empty()) return NUE_ERR_UNSUPPORTED;
        NSString* base = base_dir_for(kind);
        if (!base) return NUE_ERR_FAILED;
        path = path_join([base UTF8String], sub);
    }
    NSString* ns = [NSString stringWithUTF8String:path.c_str()];
    [[NSFileManager defaultManager] createDirectoryAtPath:ns withIntermediateDirectories:YES attributes:nil error:nil];
    // Reproducible or large data stays out of iCloud/iTunes backups: the game files (the player has them), caches and scratch space.
    if (kind == NUE_ROOT_DATA || kind == NUE_ROOT_CACHE || kind == NUE_ROOT_TEMP) nue_ios_exclude_from_backup(path.c_str(), 1);
    return string_out(path, buffer, capacity, out_size);
}

// ---- import source: enumerates a directory (path or security-scoped folder URL) and streams the files sequentially ----
struct ImportEntry { std::string rel, abs; uint64_t size; };
struct ImportSource {
    std::vector<ImportEntry> entries;
    size_t index = static_cast<size_t>(-1);
    FILE* file = nullptr;
    NSURL* scoped = nil;
    ~ImportSource() {
        if (file) std::fclose(file);
        if (scoped) [scoped stopAccessingSecurityScopedResource];
    }
};

nue_result import_open(nue_import_source_kind kind, const char* source, void** out) {
    if (!source || !out) return NUE_ERR_INVALID_ARGUMENT;
    if (kind == NUE_IMPORT_SOURCE_PICKER) return NUE_ERR_INVALID_ARGUMENT;   // call pick_folder first, then open the URI it returns
    auto* src = new ImportSource();
    NSURL* root = nil;
    if (kind == NUE_IMPORT_SOURCE_URI) {
        root = [NSURL URLWithString:[NSString stringWithUTF8String:source]];
        if (!root || !root.isFileURL) { delete src; return NUE_ERR_INVALID_ARGUMENT; }
        if ([root startAccessingSecurityScopedResource]) src->scoped = root;   // NO is fine: the URL is inside the sandbox already
    } else {
        root = [NSURL fileURLWithPath:[NSString stringWithUTF8String:source] isDirectory:YES];
    }
    root = [root URLByResolvingSymlinksInPath];   // /var vs /private/var would break the relative path computation
    NSString* root_path = root.path;
    BOOL is_dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:root_path isDirectory:&is_dir] || !is_dir) { delete src; return NUE_ERR_NOT_FOUND; }
    NSDirectoryEnumerator<NSURL*>* en = [[NSFileManager defaultManager] enumeratorAtURL:root
        includingPropertiesForKeys:@[NSURLIsRegularFileKey, NSURLFileSizeKey] options:0 errorHandler:^BOOL(NSURL*, NSError*) { return YES; }];
    const size_t prefix = root_path.length + 1;
    for (NSURL* u in en) {
        NSNumber* regular = nil; NSNumber* size = nil;
        [u getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil];
        if (![regular boolValue]) continue;
        [u getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        NSString* full = [u URLByResolvingSymlinksInPath].path;
        if (full.length <= prefix) continue;
        auto rel = normalize_relative_path([[full substringFromIndex:prefix] UTF8String]);
        if (!rel || rel->empty()) continue;
        if (rel->size() >= sizeof(((nue_import_entry*)nullptr)->path)) {
            NUE_LOGW(kTag, "import: path too long, skipped: %s", rel->c_str());
            continue;
        }
        src->entries.push_back({*rel, [full UTF8String], size ? [size unsignedLongLongValue] : 0});
    }
    std::sort(src->entries.begin(), src->entries.end(), [](const ImportEntry& a, const ImportEntry& b) { return a.rel < b.rel; });
    *out = src;
    return NUE_OK;
}

nue_result import_next(void* s, nue_import_entry* out) {
    auto* src = static_cast<ImportSource*>(s);
    if (!src || !out) return NUE_ERR_INVALID_ARGUMENT;
    if (src->file) { std::fclose(src->file); src->file = nullptr; }
    ++src->index;
    if (src->index >= src->entries.size()) return NUE_ERR_END_OF_DATA;
    const ImportEntry& e = src->entries[src->index];
    std::snprintf(out->path, sizeof out->path, "%s", e.rel.c_str());
    out->size = e.size;
    return NUE_OK;
}

nue_result import_read(void* s, void* buffer, size_t capacity, size_t* out_read) {
    auto* src = static_cast<ImportSource*>(s);
    if (!src || !buffer || !out_read || src->index >= src->entries.size()) return NUE_ERR_BAD_STATE;
    if (!src->file) {
        src->file = std::fopen(src->entries[src->index].abs.c_str(), "rb");
        if (!src->file) return NUE_ERR_IO;   // e.g. an iCloud file that is not downloaded
    }
    *out_read = std::fread(buffer, 1, capacity, src->file);
    if (*out_read == 0 && std::ferror(src->file)) return NUE_ERR_IO;
    return NUE_OK;
}

void import_close(void* s) { delete static_cast<ImportSource*>(s); }

struct PickContext {
    void (*done)(void*, nue_result, const char*);
    void* userdata;
};

nue_result import_pick_folder(void (*done)(void*, nue_result, const char*), void* userdata) {
    if (!done) return NUE_ERR_INVALID_ARGUMENT;
    auto* ctx = new PickContext{done, userdata};
    dispatch_async(dispatch_get_main_queue(), ^{
        nue_ios_pick_folder([](const char* path, void* user) {
            std::unique_ptr<PickContext> c(static_cast<PickContext*>(user));
            if (!path) { c->done(c->userdata, NUE_ERR_CANCELLED, nullptr); return; }
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path] isDirectory:YES];
            c->done(c->userdata, NUE_OK, [[url absoluteString] UTF8String]);
        }, ctx);
    });
    return NUE_OK_PENDING;
}

const nue_import_source_ops g_import_ops = {import_open, import_next, import_read, import_close, import_pick_folder};

// ---------------------------------------------------------------------------------------------------------------------------- threads
bool is_self(const nue_native_thread_ref* t) {
    if (!t || (t->handle == 0 && t->id == 0)) return true;
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    if (t->id && t->id == tid) return true;
    return t->handle && reinterpret_cast<pthread_t>(static_cast<uintptr_t>(t->handle)) == pthread_self();
}

qos_class_t to_qos_class(Qos q) {
    switch (q) {
    case Qos::user_interactive: return QOS_CLASS_USER_INTERACTIVE;
    case Qos::user_initiated: return QOS_CLASS_USER_INITIATED;
    case Qos::normal: return QOS_CLASS_DEFAULT;
    case Qos::utility: return QOS_CLASS_UTILITY;
    case Qos::background: return QOS_CLASS_BACKGROUND;
    }
    return QOS_CLASS_DEFAULT;
}

Qos from_qos_class(qos_class_t c) {
    switch (c) {
    case QOS_CLASS_USER_INTERACTIVE: return Qos::user_interactive;
    case QOS_CLASS_USER_INITIATED: return Qos::user_initiated;
    case QOS_CLASS_UTILITY: return Qos::utility;
    case QOS_CLASS_BACKGROUND: return Qos::background;
    default: return Qos::normal;
    }
}

nue_result ios_thread_set_priority(const nue_native_thread_ref* t, nue_thread_priority p) {
    if (is_self(t)) return pthread_set_qos_class_self_np(to_qos_class(qos_for_priority(p)), 0) == 0 ? NUE_OK : NUE_ERR_FAILED;
    // QoS can only be changed on the calling thread. For other threads fall back to the POSIX priority when a pthread handle is known.
    if (t->handle == 0) return NUE_ERR_UNSUPPORTED;
    sched_param sp{};
    sp.sched_priority = posix_priority_for(p);
    return pthread_setschedparam(reinterpret_cast<pthread_t>(static_cast<uintptr_t>(t->handle)), SCHED_OTHER, &sp) == 0 ? NUE_OK : NUE_ERR_FAILED;
}

nue_result ios_thread_get_priority(const nue_native_thread_ref* t, nue_thread_priority* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    if (!is_self(t)) return NUE_ERR_UNSUPPORTED;
    qos_class_t cls = QOS_CLASS_UNSPECIFIED;
    int rel = 0;
    if (pthread_get_qos_class_np(pthread_self(), &cls, &rel) != 0) return NUE_ERR_FAILED;
    *out = priority_for_qos(from_qos_class(cls));
    return NUE_OK;
}

nue_result ios_thread_set_core_class(const nue_native_thread_ref* t, nue_cpu_preference pref) {
    if (!is_self(t)) return NUE_ERR_UNSUPPORTED;
    if (pref == NUE_CPU_DEFAULT) return NUE_OK_NOOP;
    // iOS has no core affinity: the QoS class is the hint the scheduler uses to choose efficiency or performance cores.
    return pthread_set_qos_class_self_np(to_qos_class(qos_for_core_class(pref)), 0) == 0 ? NUE_OK : NUE_ERR_FAILED;
}

nue_result ios_thread_set_name(const char* name) {
    if (!name) return NUE_ERR_INVALID_ARGUMENT;
    return pthread_setname_np(name) == 0 ? NUE_OK : NUE_ERR_FAILED;
}

// ---------------------------------------------------------------------------------------------------------------------------- display / input extras
nue_result ios_window_safe_area(const nue_native_window* window, nue_rect_i* out) {
    if (!window || !out || window->kind != NUE_NATIVE_UIKIT_WINDOW || window->window == 0) return NUE_ERR_UNSUPPORTED;
    struct Insets { float v[4]; };   // a block cannot capture a raw array
    __block Insets ins = {{0, 0, 0, 0}};
    __block float w = 0, h = 0;
    __block int rc = -1;
    void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(window->window));
    auto query = ^{ rc = nue_ios_safe_area(ptr, ins.v, &w, &h); };
    if ([NSThread isMainThread]) query(); else dispatch_sync(dispatch_get_main_queue(), query);
    if (rc != 0) return NUE_ERR_FAILED;
    *out = safe_area_rect(w, h, ins.v[0], ins.v[1], ins.v[2], ins.v[3]);
    return NUE_OK;
}

nue_bool ios_haptics_available(void) { return nue_ios_device_haptics_available() ? NUE_TRUE : NUE_FALSE; }
nue_result ios_haptics_vibrate(uint32_t duration_ms, float intensity) {
    return nue_ios_device_haptic(duration_ms, intensity) == 0 ? NUE_OK : NUE_ERR_UNSUPPORTED;
}

// ---------------------------------------------------------------------------------------------------------------------------- crash handling
// Everything in the signal handler is async-signal-safe except the core's sink callbacks (which are specified to be) and
// backtrace_symbols_fd (not formally async-signal-safe, standard practice for crash logs).
nue_crash_sink g_sink{};
bool g_sink_set = false;
struct sigaction g_old_actions[32];
bool g_handlers_installed = false;
std::atomic<bool> g_in_crash{false};
stack_t g_alt_stack{};
const int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};

void write_stack_file(const char* dir) {
    char path[1100];
    size_t n = std::strlen(dir);
    if (n > 1000) return;
    std::memcpy(path, dir, n);
    std::memcpy(path + n, "/stack.txt", 11);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    void* frames[96];
    int count = backtrace(frames, 96);
    const char head[] = "stack of the crashing thread (module+offset; symbolicate with the dSYM):\n";
    (void)!write(fd, head, sizeof head - 1);
    backtrace_symbols_fd(frames, count, fd);
    fsync(fd);
    close(fd);
}

void crash_signal_handler(int sig, siginfo_t* si, void* ucontext) {
    if (g_in_crash.exchange(true)) _exit(128 + sig);
    nue_crash_info info{};
    info.struct_size = sizeof info;
    info.reason = signal_reason(sig);
    info.fault_address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(si ? si->si_addr : nullptr));
    info.signal_or_code = static_cast<uint32_t>(sig);
#if defined(__arm64__)
    if (ucontext) info.fault_pc = static_cast<uint64_t>(__darwin_arm_thread_state64_get_pc(static_cast<ucontext_t*>(ucontext)->uc_mcontext->__ss));
#elif defined(__x86_64__)
    if (ucontext) info.fault_pc = static_cast<uint64_t>(static_cast<ucontext_t*>(ucontext)->uc_mcontext->__ss.__rip);
#endif
    if (g_sink_set && g_sink.begin) {
        const char* dir = g_sink.begin(&info, g_sink.userdata);
        if (dir) {
            info.report_dir = dir;
            write_stack_file(dir);
        }
        if (g_sink.end) g_sink.end(&info, g_sink.userdata);
    }
    // Hand over to the previous handler (normally the system's, which produces Apple's own crash log) by re-raising.
    sigaction(sig, &g_old_actions[sig], nullptr);
    raise(sig);
}

nue_result ios_crash_install(const nue_crash_desc* desc, const nue_crash_sink* sink) {
    if (!sink) return NUE_ERR_INVALID_ARGUMENT;
    if (g_handlers_installed) { g_sink = *sink; g_sink_set = true; return NUE_OK_NOOP; }
    g_sink = *sink;
    g_sink_set = true;
    // Own stack so a stack overflow can still be reported. Only the installing thread gets one (sigaltstack is per thread).
    g_alt_stack.ss_size = 64 * 1024;
    g_alt_stack.ss_sp = std::malloc(g_alt_stack.ss_size);
    g_alt_stack.ss_flags = 0;
    if (!g_alt_stack.ss_sp || sigaltstack(&g_alt_stack, nullptr) != 0) return NUE_ERR_FAILED;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : kCrashSignals) sigaction(s, &sa, &g_old_actions[s]);
    g_handlers_installed = true;
    NUE_LOGI(kTag, "crash handler installed (signals; Mach exceptions are not used)");
    return NUE_OK;
}

void ios_crash_uninstall(void) {
    if (!g_handlers_installed) return;
    for (int s : kCrashSignals) sigaction(s, &g_old_actions[s], nullptr);
    g_handlers_installed = false;
    g_sink_set = false;
    g_in_crash = false;
}

nue_result ios_crash_write_dump(const char* directory, nue_crash_flags) {
    if (!directory) return NUE_ERR_INVALID_ARGUMENT;
    write_stack_file(directory);
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------------------------------- log
void ios_log_write(nue_log_level level, const char* tag, const char* message) {
    static os_log_t log = os_log_create("org.nue.engine", "engine");
    os_log_with_type(log, static_cast<os_log_type_t>(os_log_type_for(level)), "[%{public}s] %{public}s", tag ? tag : "nue", message ? message : "");
}

// ---------------------------------------------------------------------------------------------------------------------------- extension "nue.ios.v1"
nue_result ext_probe_gpu(nue_ios_gpu_info* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    NueIosGpuInfo g;
    if (nue_ios_probe_gpu(&g) != 0) return NUE_ERR_UNSUPPORTED;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = size ? size : sizeof *out;
    std::snprintf(out->name, sizeof out->name, "%s", g.name);
    out->apple_family = g.highest_apple_family;
    out->supports_bc = g.supports_bc;
    out->unified_memory = g.unified_memory;
    out->is_simulator = g.is_simulator;
    out->recommended_max_working_set = g.recommended_max_working_set;
    out->physical_memory = g.physical_memory;
    std::memcpy(out->texture_format, g.texture_format, sizeof out->texture_format);
    out->texture_format_count = g.texture_format_count;
    out->max_texture_size = g.max_texture_size;
    out->max_anisotropy = g.max_anisotropy;
    out->max_samples = g.max_samples;
    out->depth24_stencil8 = g.depth24_stencil8;
    out->dxt_storage = g.texture_storage;
    out->hard_memory_limit = g.hard_memory_limit;
    out->soft_memory_limit = g.soft_memory_limit;
    out->texture_budget = g.texture_budget;
    return NUE_OK;
}

nue_result ext_configure_layer(void* layer, nue_bool framebuffer_only, uint32_t* w, uint32_t* h) {
    return nue_ios_configure_metal_layer(layer, framebuffer_only ? 1 : 0, w, h) == 0 ? NUE_OK : NUE_ERR_INVALID_ARGUMENT;
}
uint32_t ext_game_mode_declared(void) { return static_cast<uint32_t>(nue_ios_game_mode_info()); }
int64_t ext_available_memory(void) { return nue_ios_memory_available(); }

const nue_ext_ios_v1 g_ext = {sizeof(nue_ext_ios_v1), ext_probe_gpu, ext_configure_layer, ext_game_mode_declared, ext_available_memory};

#if NUE_IOS_API_011
nue_result ios_memory_info_ex(nue_memory_info* m) {
    if (!m) return NUE_ERR_INVALID_ARGUMENT;
    static uint64_t working_set = [] {
        NueIosGpuInfo g;
        return nue_ios_probe_gpu(&g) == 0 ? g.recommended_max_working_set : 0;
    }();
    const int64_t footprint = nue_ios_memory_footprint();
    const int64_t avail = nue_ios_available_memory();   // os_proc_available_memory: the room LEFT before the jetsam limit; 0 on the Simulator
    fill_memory_info(m, footprint > 0 ? static_cast<uint64_t>(footprint) : 0, avail > 0 ? static_cast<uint64_t>(avail) : 0, working_set,
                     static_cast<MemoryPressure>(g_last_pressure.load()));
    return NUE_OK;
}

nue_result ios_gpu_probe(nue_gpu_caps* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    NueIosGpuInfo g;
    if (nue_ios_probe_gpu(&g) != 0) return NUE_ERR_UNSUPPORTED;   // no Metal device: the core falls back to its conservative table
    MetalProbe p;
    p.highest_apple_family = g.highest_apple_family;
    p.supports_bc = g.supports_bc != 0;
    p.unified_memory = g.unified_memory != 0;
    p.is_simulator = g.is_simulator != 0;
    p.recommended_max_working_set = g.recommended_max_working_set;
    p.physical_memory = g.physical_memory;
    fill_gpu_caps(out, p, derive_caps(p));
    return NUE_OK;
}

// ProMotion cooperation: an exact frame-rate range. A frame limit wins over the refresh request; 0 and 0 release it.
nue_result ios_window_request_refresh(const nue_native_window*, float refresh_hz, float frame_limit_fps) {
    apply_fps(frame_limit_fps > 0 ? frame_limit_fps : refresh_hz);
    return NUE_OK;
}
#endif

// ---------------------------------------------------------------------------------------------------------------------------- lifecycle of the layer
nue_result ios_init(const nue_backend_init* init) {
    (void)init;
    // SDL3 owns the audio device and picks the AVAudioSession category from this hint (SDL reads hints from the environment too).
    // "playback" ignores the silent switch and does not mix; the config key ios.audio_mix = "respect_silent" selects SDL's "ambient".
    char mix[32] = {0};
    nue_config_get_string("ios.audio_mix", "exclusive", mix, sizeof mix, nullptr);
    setenv("SDL_AUDIO_CATEGORY", std::strcmp(mix, "respect_silent") == 0 ? "ambient" : "playback", 0);

    auto start = ^{
        NueIosConfig cfg{};
        cfg.game_dir_name = "NFSU2";
        cfg.enable_memory_poll = 1;
        cfg.enable_crash_handler = 0;   // the core installs crash handling through crash_install
        cfg.enable_motion = 0;          // sensors come from SDL3
        cfg.target_fps = 60;
        cfg.memory_limit_override_bytes = static_cast<uint64_t>(std::max<int64_t>(0, nue_config_get_int("ios.memory_limit_mb", 0))) << 20;   // 0 = derive from the OS
        cfg.adapter_mode = 1;
        cfg.on_event = on_ios_event;
        nue_ios_init(&cfg);
    };
    if ([NSThread isMainThread]) start(); else dispatch_async(dispatch_get_main_queue(), start);
    nue_internal_register_extension("nue.ios.v1", &g_ext);
    g_power_have_last = false;
    poll_power(true);
    g_inited = true;
    return NUE_OK;
}

void ios_shutdown(void) {
    if (!g_inited.exchange(false)) return;
    // Undo everything: idle timer, frame-rate request, crash handlers.
    nue_ios_set_keep_awake(0);
    g_keep_awake_active = false;
    apply_fps(0);
    ios_crash_uninstall();
    auto stop = ^{ nue_ios_shutdown(); };
    if ([NSThread isMainThread]) stop(); else dispatch_sync(dispatch_get_main_queue(), stop);
}

void ios_pump(void) { if (g_inited) poll_power(false); }

constexpr uint64_t bit(nue_capability c) { return 1ull << static_cast<unsigned>(c); }   // every capability index is below 64 today
static_assert(NUE_CAP_CRASH_HANDLER < 64 && NUE_CAP_CORE_CLASS < 64, "capabilities[1] would be needed");

constexpr uint64_t kCapabilities0 =
    bit(NUE_CAP_SAFE_AREA) | bit(NUE_CAP_HAPTICS) | bit(NUE_CAP_DATA_IMPORT) | bit(NUE_CAP_DATA_IMPORT_PICKER) |
    bit(NUE_CAP_THERMAL_STATE) | bit(NUE_CAP_BATTERY) | bit(NUE_CAP_LOW_POWER_MODE) | bit(NUE_CAP_KEEP_AWAKE) |
    bit(NUE_CAP_THREAD_PRIORITY) | bit(NUE_CAP_CORE_CLASS) | bit(NUE_CAP_CRASH_HANDLER);

const nue_platform_ops g_ops = {
    /* struct_size */ sizeof(nue_platform_ops),
    /* name */ "ios",
    /* capabilities: only what this layer provides. Game Mode is NOT listed: it can be declared but neither read nor requested. */
    { kCapabilities0, 0 },
    ios_init, ios_shutdown, ios_pump,
    /* system */ ios_system_info, ios_memory_info, ios_system_locale,
    /* storage */ ios_storage_root, &g_import_ops,
    /* power */ ios_power_info, ios_power_apply, /* timer_resolution_get */ nullptr,
    /* threads */ ios_thread_set_priority, ios_thread_get_priority, /* affinity: none on iOS */ nullptr, ios_thread_set_core_class, ios_thread_set_name,
    /* perf hints */ nullptr, nullptr, nullptr, nullptr, /* sleep_until */ nullptr,
    /* display */ ios_window_safe_area, /* gpu preference */ nullptr,
    /* haptics */ ios_haptics_available, ios_haptics_vibrate,
    /* crash */ ios_crash_install, ios_crash_uninstall, ios_crash_write_dump, /* release_input */ nullptr,
    /* log */ ios_log_write,
#if NUE_IOS_API_011
    /* pre_init: not needed (SDL_AUDIO_CATEGORY is read when the device opens, after config load, so init sets it) */ nullptr,
    /* owns_lifecycle: this layer drives NUE_EVENT_APP_* from UIApplication notifications; the backend must not */ NUE_TRUE,
    /* memory_info_ex */ ios_memory_info_ex,
    /* gpu_probe */ ios_gpu_probe,
    /* window_request_refresh */ ios_window_request_refresh,
    /* screen_keyboard: SDL3 text input covers it */ nullptr,
#endif
};

}  // namespace

extern "C" const nue_platform_ops* nue_platform_ios_get_ops(void) { return &g_ops; }
