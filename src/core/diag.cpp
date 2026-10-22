// Diagnostics: crash report framework, notes, breadcrumbs, assertions, counters, frame statistics, Chrome-trace zones.
#include "internal/core_internal.h"
#include "internal/os_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

namespace {

// ---- notes and breadcrumbs ----------------------------------------------------------------------------------------------------
struct DiagState {
    std::mutex mutex;
    std::map<std::string, std::string> notes;
    nue_crash_desc crash_desc;
    bool crash_installed = false;
    std::string report_root_native;
    uint32_t keep_reports = 10;
    nue_assert_handler assert_handler = nullptr;
    void* assert_user = nullptr;
    uint64_t report_serial = 0;
};
DiagState& D() { static DiagState* d = new DiagState(); return *d; }

constexpr int kCrumbs = 128;
constexpr int kCrumbLen = 160;
char g_crumbs[kCrumbs][kCrumbLen];
std::atomic<uint32_t> g_crumb_next{0};

std::string join_native(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }

std::string breadcrumb_text() {
    std::string out;
    uint32_t n = g_crumb_next.load();
    uint32_t first = n > kCrumbs ? n - kCrumbs : 0;
    for (uint32_t i = first; i < n; ++i) { out += "  "; out += g_crumbs[i % kCrumbs]; out += "\n"; }
    return out;
}

std::string iso_time() {
    std::time_t t = std::time_t(nue_time_unix_ms() / 1000);
    std::tm tmv;
    nuei::os_localtime(t, &tmv);
    char b[40];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

// Best effort report writer. Runs in the crash context: it allocates and takes locks the failing thread might hold, so every step
// tolerates failure and the platform layer must have written the dump (the part that matters) independently of this text.
const char* report_begin(const nue_crash_info* info, void*) {
    DiagState& d = D();
    static char dir_buf[1024];
    std::time_t t = std::time_t(nue_time_unix_ms() / 1000);
    std::tm tmv;
    nuei::os_localtime(t, &tmv);
    char stamp[48];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d-%llu", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  (unsigned long long)(d.report_serial++));
    std::string dir = join_native(d.report_root_native, stamp);
    if (!nuei::os_make_dirs(dir)) return nullptr;
    std::snprintf(dir_buf, sizeof(dir_buf), "%s", dir.c_str());

    std::string text;
    text += "NUE crash report\n================\n";
    text += "time: " + iso_time() + "\n";
    text += std::string("reason: ") + (info && info->reason ? info->reason : "(none)") + "\n";
    if (info) {
        char b[160];
        std::snprintf(b, sizeof(b), "fault address: 0x%llx\nfault pc: 0x%llx\nsignal/code: 0x%x\n", (unsigned long long)info->fault_address, (unsigned long long)info->fault_pc, info->signal_or_code);
        text += b;
    }
    text += std::string("nue: ") + nue_get_version_string() + "\n";
    const nuei::InitInfo& ii = nuei::init_info();
    text += "app: " + ii.app_name + " (" + ii.app_id + ") " + ii.app_version + "\n";
    text += "pointer size: " + std::to_string(NUE_POINTER_SIZE * 8) + " bit\n";
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    if (NUE_SUCCEEDED(nue_system_get_info(&si))) {
        text += std::string("os: ") + si.os_name + " " + si.os_version + "\n";
        text += std::string("cpu: ") + si.cpu_name + " (" + std::to_string(si.cpu_logical_cores) + " logical)\n";
        text += "memory: " + std::to_string(si.memory_total_bytes >> 20) + " MiB total, " + std::to_string(si.memory_available_bytes >> 20) + " MiB free\n";
        for (uint32_t i = 0; i < si.gpu_count && i < NUE_MAX_GPUS; ++i) text += std::string("gpu: ") + si.gpus[i].name + "\n";
    }
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (!d.notes.empty()) { text += "\nnotes:\n"; for (auto& kv : d.notes) text += "  " + kv.first + " = " + kv.second + "\n"; }
        if (d.crash_desc.guest_pc) {
            char b[64];
            std::snprintf(b, sizeof(b), "guest pc: 0x%llx\n", (unsigned long long)d.crash_desc.guest_pc(d.crash_desc.guest_pc_userdata));
            text += b;
        }
    }
    text += "\nbreadcrumbs (oldest first):\n" + breadcrumb_text();
    if (!d.crash_installed || (d.crash_desc.flags & NUE_CRASH_INCLUDE_LOG)) {
        size_t need = 0;
        nue_log_get_recent(nullptr, 0, &need);
        std::string log(need, '\0');
        if (need) { nue_log_get_recent(&log[0], need, &need); log.resize(need ? need - 1 : 0); }
        text += "\nlog tail:\n" + log + "\n";
    }
    FILE* f = nuei::fopen_utf8(join_native(dir, "report.txt"), "wb");
    if (f) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
    return dir_buf;
}

nue_bool report_end(const nue_crash_info* info, void*) {
    DiagState& d = D();
    nue_crash_callback cb = nullptr;
    void* ud = nullptr;
    { cb = d.crash_desc.callback; ud = d.crash_desc.userdata; }
    nue_log_flush();
    if (cb) return cb(ud, info);
    return NUE_TRUE;
}

void rotate_reports(DiagState& d) {
    std::vector<nuei::OsDirEntry> entries;
    if (!nuei::os_list_dir(d.report_root_native, &entries)) return;
    std::vector<std::string> dirs;
    for (auto& e : entries) if (e.kind == NUE_ENTRY_DIRECTORY) dirs.push_back(e.name);
    std::sort(dirs.begin(), dirs.end());
    while (dirs.size() > d.keep_reports) { nuei::os_remove_dir_tree(join_native(d.report_root_native, dirs.front())); dirs.erase(dirs.begin()); }
}

// ---- counters ----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxCounters = 1024;
struct Counter { std::atomic<int64_t> value{0}; const char* name = nullptr; nue_counter_kind kind = NUE_COUNTER_GAUGE; };
Counter g_counters[kMaxCounters];
std::atomic<uint32_t> g_counter_count{0};
std::mutex g_counter_mutex;
std::vector<std::string*> g_counter_names;

// ---- frame stats -------------------------------------------------------------------------------------------------------------
struct FrameSample { int64_t t_ns; double cpu_s; };
struct FrameState {
    std::mutex mutex;
    std::vector<FrameSample> ring;
    size_t head = 0, count = 0;
    uint64_t total = 0;
    double log_interval_s = 0;
    int64_t next_log_ns = 0;
};
FrameState& F() { static FrameState* f = new FrameState(); return *f; }

double thread_cpu_seconds() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return -1;
    auto ft = [](FILETIME x) { return double((uint64_t(x.dwHighDateTime) << 32) | x.dwLowDateTime) * 1e-7; };
    return ft(k) + ft(u);
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return -1;
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#else
    return -1;
#endif
}

// ---- trace -------------------------------------------------------------------------------------------------------------------
struct TraceState {
    std::mutex mutex;
    FILE* file = nullptr;
    bool first = true;
    int64_t t0 = 0;
};
TraceState& T() { static TraceState* t = new TraceState(); return *t; }
std::atomic<bool> g_trace_on{false};

void trace_event(char phase, const char* name) {
    TraceState& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    if (!t.file) return;
    double us = double(nue_time_ns() - t.t0) / 1000.0;
    std::fprintf(t.file, "%s{\"ph\":\"%c\",\"pid\":1,\"tid\":%llu,\"ts\":%.3f", t.first ? "" : ",\n", phase, (unsigned long long)nue_thread_current_id(), us);
    if (phase == 'B') {
        std::fputs(",\"name\":\"", t.file);
        for (const char* c = name; c && *c; ++c) { if (*c == '"' || *c == '\\') std::fputc('\\', t.file); std::fputc(*c, t.file); }
        std::fputc('"', t.file);
    }
    std::fputc('}', t.file);
    t.first = false;
}

}  // namespace

namespace nuei {

void diag_init(const InitInfo& info) {
    DiagState& d = D();
    { std::lock_guard<std::mutex> lock(d.mutex); d.notes.clear(); d.crash_installed = false; }
    g_crumb_next = 0;
    if (info.flags & NUE_INIT_NO_CRASH_HANDLER) return;
    nue_crash_desc cd;
    NUE_STRUCT_INIT(&cd);
    cd.flags = NUE_CRASH_DEFAULT;
    nue_result r = nue_diag_install_crash_handler(&cd);
    if (NUE_FAILED(r) && r != NUE_ERR_UNSUPPORTED) NUE_LOGW("diag", "crash handler not installed: %s", nue_get_last_error_message());
}

void diag_shutdown() {
    nue_diag_uninstall_crash_handler();
    nue_trace_stop();
    DiagState& d = D();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.notes.clear();
}

}  // namespace nuei

extern "C" {

NUE_API nue_result NUE_CALL nue_diag_install_crash_handler(const nue_crash_desc* desc) {
    if (!desc || desc->struct_size < sizeof(uint32_t) * 2) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_diag_install_crash_handler: desc is required");
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    const nue_platform_ops* p = nuei::platform();
    if (!p || !p->crash_install) return nuei::fail(NUE_ERR_UNSUPPORTED, "this platform layer has no crash handler");
    DiagState& d = D();
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.crash_desc = *desc;
        d.crash_desc.struct_size = sizeof(d.crash_desc);
        d.keep_reports = desc->keep_reports ? desc->keep_reports : 10;
    }
    std::string root = nuei::resolve_native_path(desc->report_root && *desc->report_root ? desc->report_root : "user:crash", true);
    if (root.empty()) return nuei::fail(NUE_ERR_NOT_FOUND, "crash report root cannot be resolved");
    nuei::os_make_dirs(root);
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.report_root_native = root;
        rotate_reports(d);
    }
    nue_crash_sink sink;
    NUE_STRUCT_INIT(&sink);
    sink.begin = report_begin;
    sink.end = report_end;
    sink.flags = desc->flags;
    nue_result r = p->crash_install(&d.crash_desc, &sink);
    if (NUE_SUCCEEDED(r)) d.crash_installed = true;
    return r;
}

NUE_API nue_result NUE_CALL nue_diag_uninstall_crash_handler(void) {
    DiagState& d = D();
    if (!d.crash_installed) return NUE_OK_NOOP;
    const nue_platform_ops* p = nuei::platform();
    if (p && p->crash_uninstall) p->crash_uninstall();
    d.crash_installed = false;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_diag_set_note(const char* key, const char* value) {
    if (!key || !*key) return NUE_ERR_INVALID_ARGUMENT;
    DiagState& d = D();
    std::lock_guard<std::mutex> lock(d.mutex);
    if (!value) { d.notes.erase(key); return NUE_OK; }
    if (d.notes.size() >= 64 && !d.notes.count(key)) return nuei::fail(NUE_ERR_OUT_OF_MEMORY, "too many diagnostic notes (64)");
    d.notes[std::string(key).substr(0, 128)] = std::string(value).substr(0, 256);
    return NUE_OK;
}

NUE_API void NUE_CALL nue_diag_breadcrumb(const char* message) {
    if (!message) return;
    uint32_t i = g_crumb_next.fetch_add(1);
    char* slot = g_crumbs[i % kCrumbs];
    std::snprintf(slot, kCrumbLen, "%.3f %s", double(nue_time_ns()) / 1e9, message);
}

NUE_API nue_result NUE_CALL nue_diag_write_report(const char* reason, char* dir_buffer, size_t dir_capacity, size_t* out_size) {
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    DiagState& d = D();
    if (d.report_root_native.empty()) {
        std::string root = nuei::resolve_native_path("user:crash", true);
        if (root.empty()) return NUE_ERR_NOT_FOUND;
        nuei::os_make_dirs(root);
        d.report_root_native = root;
    }
    nue_crash_info info;
    NUE_STRUCT_INIT(&info);
    info.reason = reason ? reason : "manual report";
    const char* dir = report_begin(&info, nullptr);
    if (!dir) return nuei::fail(NUE_ERR_IO, "cannot create a report folder under '%s'", d.report_root_native.c_str());
    const nue_platform_ops* p = nuei::platform();
    if (p && p->crash_write_dump) p->crash_write_dump(dir, d.crash_installed ? d.crash_desc.flags : NUE_CRASH_DEFAULT);
    rotate_reports(d);
    return nuei::copy_out_string(dir, std::strlen(dir), dir_buffer, dir_capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_diag_list_reports(char* buffer, size_t capacity, size_t* out_size) {
    DiagState& d = D();
    std::string root = d.report_root_native.empty() ? nuei::resolve_native_path("user:crash", false) : d.report_root_native;
    std::string text;
    std::vector<nuei::OsDirEntry> entries;
    if (!root.empty() && nuei::os_list_dir(root, &entries)) {
        std::vector<std::string> dirs;
        for (auto& e : entries) if (e.kind == NUE_ENTRY_DIRECTORY) dirs.push_back(e.name);
        std::sort(dirs.begin(), dirs.end(), std::greater<std::string>());
        for (auto& n : dirs) text += join_native(root, n) + "\n";
    }
    return nuei::copy_out_string(text, buffer, capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_diag_clear_reports(void) {
    DiagState& d = D();
    std::string root = d.report_root_native.empty() ? nuei::resolve_native_path("user:crash", false) : d.report_root_native;
    if (root.empty()) return NUE_OK;
    std::vector<nuei::OsDirEntry> entries;
    if (nuei::os_list_dir(root, &entries)) for (auto& e : entries) if (e.kind == NUE_ENTRY_DIRECTORY) nuei::os_remove_dir_tree(join_native(root, e.name));
    return NUE_OK;
}

static volatile int g_stack_sink;
static int recurse(int n) { volatile char pad[4096]; pad[0] = char(n); if (n < 0) return 0; return recurse(n + 1) + pad[0]; }

NUE_API nue_result NUE_CALL nue_diag_test_crash(nue_test_crash_kind kind) {
    if (!nuei::deterministic() && !nue_config_get_bool("diag.allow_test_crash", NUE_FALSE))
        return nuei::fail(NUE_ERR_ACCESS_DENIED, "test crashes are disabled (NUE_INIT_DETERMINISTIC or config diag.allow_test_crash = true)");
    nue_diag_breadcrumb("nue_diag_test_crash");
    switch (kind) {
        case NUE_TEST_CRASH_NULL_WRITE: { volatile int* p = nullptr; *p = 1; break; }
        case NUE_TEST_CRASH_ABORT: std::abort();
        case NUE_TEST_CRASH_DIVIDE_BY_ZERO: { volatile int z = 0; g_stack_sink = 1 / z; break; }
        case NUE_TEST_CRASH_STACK_OVERFLOW: g_stack_sink = recurse(0); break;
        default: return NUE_ERR_INVALID_ARGUMENT;
    }
    return NUE_OK;
}

NUE_API void NUE_CALL nue_diag_set_assert_handler(nue_assert_handler handler, void* userdata) {
    DiagState& d = D();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.assert_handler = handler;
    d.assert_user = userdata;
}

NUE_API nue_assert_action NUE_CALL nue_diag_assert_failed(const char* expression, const char* message, const char* file, int line) {
    DiagState& d = D();
    nue_assert_handler h;
    void* ud;
    { std::lock_guard<std::mutex> lock(d.mutex); h = d.assert_handler; ud = d.assert_user; }
    NUE_LOGF("assert", "%s:%d: assertion failed: %s%s%s", file ? file : "?", line, expression ? expression : "?", message ? " - " : "", message ? message : "");
    if (h) return h(ud, expression, message, file, line);
#if defined(NDEBUG)
    return NUE_ASSERT_ABORT;
#else
    return NUE_ASSERT_BREAK;
#endif
}

// ---- counters ----------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_counter_register(const char* name, nue_counter_kind kind, nue_counter_id* out_id) {
    if (!name || !out_id) return NUE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_counter_mutex);
    uint32_t n = g_counter_count.load();
    for (uint32_t i = 0; i < n; ++i) if (std::strcmp(g_counters[i].name, name) == 0) { *out_id = i + 1; return NUE_OK; }
    if (n >= kMaxCounters) return nuei::fail(NUE_ERR_OUT_OF_MEMORY, "too many counters (%u)", kMaxCounters);
    std::string* owned = new std::string(name);
    g_counter_names.push_back(owned);
    g_counters[n].name = owned->c_str();
    g_counters[n].kind = kind;
    g_counters[n].value = 0;
    g_counter_count.store(n + 1);
    *out_id = n + 1;
    return NUE_OK;
}
NUE_API void NUE_CALL nue_counter_set(nue_counter_id id, int64_t v) { if (id && id <= g_counter_count.load()) g_counters[id - 1].value.store(v, std::memory_order_relaxed); }
NUE_API void NUE_CALL nue_counter_add(nue_counter_id id, int64_t dv) { if (id && id <= g_counter_count.load()) g_counters[id - 1].value.fetch_add(dv, std::memory_order_relaxed); }
NUE_API int64_t NUE_CALL nue_counter_get(nue_counter_id id) { return (id && id <= g_counter_count.load()) ? g_counters[id - 1].value.load(std::memory_order_relaxed) : 0; }
NUE_API nue_result NUE_CALL nue_counter_snapshot(nue_counter_value* out, uint32_t capacity, uint32_t* out_count) {
    uint32_t n = g_counter_count.load();
    if (out_count) *out_count = n;
    for (uint32_t i = 0; out && i < capacity && i < n; ++i) { out[i].name = g_counters[i].name; out[i].kind = g_counters[i].kind; out[i].value = g_counters[i].value.load(); }
    return NUE_OK;
}

// ---- frame stats -------------------------------------------------------------------------------------------------------------

NUE_API void NUE_CALL nue_frame_mark(void) {
    FrameState& f = F();
    FrameSample s{nue_time_ns(), thread_cpu_seconds()};
    bool do_log = false;
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        if (f.ring.empty()) f.ring.resize(8192);
        f.ring[f.head] = s;
        f.head = (f.head + 1) % f.ring.size();
        if (f.count < f.ring.size()) ++f.count;
        ++f.total;
        if (f.log_interval_s > 0 && s.t_ns >= f.next_log_ns) { do_log = true; f.next_log_ns = s.t_ns + int64_t(f.log_interval_s * 1e9); }
    }
    if (do_log) {
        nue_frame_stats st;
        NUE_STRUCT_INIT(&st);
        if (NUE_SUCCEEDED(nue_frame_get_stats(0, &st)))
            NUE_LOGI("fps", "fps=%.1f frame ms avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f cpu=%.0f%%", st.fps, st.frame_ms_avg, st.frame_ms_p50, st.frame_ms_p95,
                     st.frame_ms_p99, st.frame_ms_max, st.cpu_busy_percent);
    }
}

NUE_API nue_result NUE_CALL nue_frame_get_stats(double window_seconds, nue_frame_stats* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    if (window_seconds <= 0) window_seconds = 5.0;
    FrameState& f = F();
    std::vector<FrameSample> win;
    uint64_t total;
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        total = f.total;
        if (f.count == 0) { nue_frame_stats z; NUE_STRUCT_INIT(&z); z.cpu_busy_percent = -1; uint32_t keep = out->struct_size; std::memcpy(out, &z, keep < sizeof(z) ? keep : sizeof(z)); out->struct_size = keep; return NUE_OK; }
        const int64_t newest = f.ring[(f.head + f.ring.size() - 1) % f.ring.size()].t_ns;
        const int64_t limit = newest - int64_t(window_seconds * 1e9);
        for (size_t i = 0; i < f.count; ++i) {
            const FrameSample& s = f.ring[(f.head + f.ring.size() - 1 - i) % f.ring.size()];
            if (s.t_ns < limit) break;
            win.push_back(s);
        }
        std::reverse(win.begin(), win.end());
    }
    nue_frame_stats st;
    NUE_STRUCT_INIT(&st);
    st.total_frames = total;
    st.cpu_busy_percent = -1;
    if (win.size() >= 2) {
        std::vector<double> ms;
        for (size_t i = 1; i < win.size(); ++i) ms.push_back(double(win[i].t_ns - win[i - 1].t_ns) / 1e6);
        double sum = 0, mx = 0;
        for (double v : ms) { sum += v; mx = std::max(mx, v); }
        std::sort(ms.begin(), ms.end());
        auto pct = [&](double p) { size_t idx = size_t(std::ceil(p * double(ms.size()))) ; idx = idx == 0 ? 0 : idx - 1; return ms[std::min(idx, ms.size() - 1)]; };
        st.window_frames = uint32_t(ms.size());
        st.window_seconds = double(win.back().t_ns - win.front().t_ns) / 1e9;
        st.fps = st.window_seconds > 0 ? double(ms.size()) / st.window_seconds : 0;
        st.frame_ms_avg = sum / double(ms.size());
        st.frame_ms_p50 = pct(0.50); st.frame_ms_p95 = pct(0.95); st.frame_ms_p99 = pct(0.99); st.frame_ms_max = mx;
        if (win.front().cpu_s >= 0 && win.back().cpu_s >= 0 && st.window_seconds > 0)
            st.cpu_busy_percent = 100.0 * (win.back().cpu_s - win.front().cpu_s) / st.window_seconds;
    }
    uint32_t keep = out->struct_size;
    std::memcpy(out, &st, keep < sizeof(st) ? keep : sizeof(st));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_frame_set_log_interval(double seconds) {
    FrameState& f = F();
    std::lock_guard<std::mutex> lock(f.mutex);
    f.log_interval_s = seconds > 0 ? seconds : 0;
    f.next_log_ns = nue_time_ns() + int64_t(f.log_interval_s * 1e9);
    return NUE_OK;
}

// ---- trace -------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_trace_start(const char* path) {
    if (!path) return NUE_ERR_INVALID_ARGUMENT;
    std::string native = nuei::resolve_native_path(path, true);
    if (native.empty()) return nuei::fail(NUE_ERR_NOT_FOUND, "trace path '%s' cannot be resolved", path);
    nuei::os_make_parent_dirs(native);
    TraceState& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    if (t.file) return NUE_ERR_BUSY;
    t.file = nuei::fopen_utf8(native, "wb");
    if (!t.file) return nuei::fail(NUE_ERR_IO, "cannot open trace file '%s'", native.c_str());
    std::fputs("[\n", t.file);
    t.first = true;
    t.t0 = nue_time_ns();
    g_trace_on = true;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_trace_stop(void) {
    TraceState& t = T();
    std::lock_guard<std::mutex> lock(t.mutex);
    g_trace_on = false;
    if (!t.file) return NUE_OK_NOOP;
    std::fputs("\n]\n", t.file);
    std::fclose(t.file);
    t.file = nullptr;
    return NUE_OK;
}

NUE_API nue_bool NUE_CALL nue_trace_enabled(void) { return g_trace_on.load(std::memory_order_relaxed) ? NUE_TRUE : NUE_FALSE; }
NUE_API void NUE_CALL nue_trace_begin(const char* name) { trace_event('B', name); }
NUE_API void NUE_CALL nue_trace_end(void) { trace_event('E', nullptr); }

}  // extern "C"
