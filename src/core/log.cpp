// Logging: levels, per-tag overrides, sinks (stderr, rotating file, platform, callback, memory ring).
#include "internal/core_internal.h"
#include "internal/os_util.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Sink {
    nue_log_sink_id id = 0;
    nue_log_sink_kind kind = NUE_LOG_SINK_STDERR;
    nue_log_level min_level = NUE_LOG_TRACE;
    FILE* file = nullptr;
    std::string path;
    uint64_t bytes = 0;
    uint32_t max_bytes = 0;
    uint32_t keep_files = 3;
    nue_log_callback callback = nullptr;
    void* userdata = nullptr;
    std::string ring;
    uint32_t ring_capacity = 0;
};

struct LogState {
    std::mutex mutex;
    std::vector<Sink> sinks;
    nue_log_level level = NUE_LOG_INFO;
    std::map<std::string, nue_log_level> tag_levels;
    nue_log_sink_id next_id = 1;
    bool defaults_done = false;
};

LogState& state() {
    static LogState* s = new LogState();   // leaked on purpose: logging must work during static destruction
    return *s;
}

const char* level_letter(nue_log_level l) {
    switch (l) {
        case NUE_LOG_TRACE: return "T";
        case NUE_LOG_DEBUG: return "D";
        case NUE_LOG_INFO: return "I";
        case NUE_LOG_WARN: return "W";
        case NUE_LOG_ERROR: return "E";
        case NUE_LOG_FATAL: return "F";
        default: return "?";
    }
}

void add_default_stderr_locked(LogState& s) {
    Sink k;
    k.id = s.next_id++;
    k.kind = NUE_LOG_SINK_STDERR;
    k.min_level = NUE_LOG_WARN;
    s.sinks.push_back(std::move(k));
    s.defaults_done = true;
}

void rotate_file_locked(Sink& k) {
    if (k.file) { std::fclose(k.file); k.file = nullptr; }
    // path.(keep) is dropped, path.N -> path.N+1, path -> path.1
    for (uint32_t i = k.keep_files; i >= 1; --i) {
        std::string from = i == 1 ? k.path : k.path + "." + std::to_string(i - 1);
        std::string to = k.path + "." + std::to_string(i);
        nuei::os_remove_file(to);
        nuei::os_rename_file(from, to);
    }
    k.file = nuei::fopen_utf8(k.path, "ab");
    k.bytes = 0;
}

void write_to_sink_locked(Sink& k, const nue_log_record& rec, const std::string& line) {
    switch (k.kind) {
        case NUE_LOG_SINK_STDERR:
            std::fwrite(line.data(), 1, line.size(), stderr);
            break;
        case NUE_LOG_SINK_FILE:
            if (!k.file) break;
            if (k.max_bytes && k.bytes + line.size() > k.max_bytes) rotate_file_locked(k);
            if (k.file) { std::fwrite(line.data(), 1, line.size(), k.file); k.bytes += line.size(); if (rec.level >= NUE_LOG_ERROR) std::fflush(k.file); }
            break;
        case NUE_LOG_SINK_PLATFORM: {
            const nue_platform_ops* p = nuei::platform();
            if (p && p->log_write) p->log_write(rec.level, rec.tag, rec.message);
            break;
        }
        case NUE_LOG_SINK_CALLBACK:
            if (k.callback) k.callback(k.userdata, &rec);
            break;
        case NUE_LOG_SINK_MEMORY_RING:
            k.ring += line;
            if (k.ring.size() > k.ring_capacity) k.ring.erase(0, k.ring.size() - k.ring_capacity);
            break;
    }
}

}  // namespace

namespace nuei {

void log_init(const InitInfo& info) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.defaults_done) add_default_stderr_locked(s);
    s.level = static_cast<nue_log_level>(info.log_level ? info.log_level : NUE_LOG_INFO);
    bool have_ring = false;
    bool have_platform = false;
    for (const Sink& k : s.sinks) { have_ring |= k.kind == NUE_LOG_SINK_MEMORY_RING; have_platform |= k.kind == NUE_LOG_SINK_PLATFORM; }
    if (!have_ring) {
        Sink k;
        k.id = s.next_id++;
        k.kind = NUE_LOG_SINK_MEMORY_RING;
        k.ring_capacity = 64 * 1024;
        s.sinks.push_back(std::move(k));
    }
    if (!have_platform && platform() && platform()->log_write) {
        Sink k;
        k.id = s.next_id++;
        k.kind = NUE_LOG_SINK_PLATFORM;
        k.min_level = NUE_LOG_INFO;
        s.sinks.push_back(std::move(k));
    }
}

void log_open_file_sink(const InitInfo& info) {
    if (info.log_file.empty() || (info.flags & NUE_INIT_NO_LOG_FILE)) return;
    nue_log_sink_desc d;
    NUE_STRUCT_INIT(&d);
    d.kind = NUE_LOG_SINK_FILE;
    d.min_level = NUE_LOG_TRACE;
    d.path = info.log_file.c_str();
    nue_log_sink_id id = 0;
    nue_result r = nue_log_add_sink(&d, &id);
    if (NUE_FAILED(r)) NUE_LOGW("log", "could not open the log file '%s' (%s)", info.log_file.c_str(), nue_get_last_error_message());
}

void log_shutdown() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (Sink& k : s.sinks) if (k.file) { std::fclose(k.file); k.file = nullptr; }
    s.sinks.clear();
    s.tag_levels.clear();
    s.defaults_done = false;
    s.level = NUE_LOG_INFO;
}

}  // namespace nuei

extern "C" {

NUE_API void NUE_CALL nue_log_writev(nue_log_level level, const char* tag, const char* file, int line, const char* fmt, va_list args) {
    LogState& s = state();
    char stack_buf[1024];
    std::string heap_buf;
    const char* message = stack_buf;
    {
        va_list copy;
        va_copy(copy, args);
        int n = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt ? fmt : "", copy);
        va_end(copy);
        if (n < 0) { stack_buf[0] = 0; }
        else if (size_t(n) >= sizeof(stack_buf)) {
            heap_buf.resize(size_t(n) + 1);
            std::vsnprintf(&heap_buf[0], heap_buf.size(), fmt ? fmt : "", args);
            heap_buf.resize(size_t(n));
            message = heap_buf.c_str();
        }
    }
    if (!tag) tag = "nue";

    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.defaults_done) add_default_stderr_locked(s);
    nue_log_level threshold = s.level;
    if (!s.tag_levels.empty()) { auto it = s.tag_levels.find(tag); if (it != s.tag_levels.end()) threshold = it->second; }
    if (level < threshold) return;

    nue_log_record rec;
    NUE_STRUCT_INIT(&rec);
    rec.level = level;
    rec.tag = tag;
    rec.message = message;
    rec.time_ns = nuei::now_ns();
    rec.unix_time_ms = nue_time_unix_ms();
    rec.thread_id = nuei::current_thread_id();
    rec.file = file;
    rec.line = line;

    // "2026-10-22 14:03:07.512 I [tag] message\n"
    std::time_t secs = std::time_t(rec.unix_time_ms / 1000);
    std::tm tmv;
    nuei::os_localtime(secs, &tmv);
    char head[64];
    std::snprintf(head, sizeof(head), "%04d-%02d-%02d %02d:%02d:%02d.%03d %s [", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                  tmv.tm_min, tmv.tm_sec, int(rec.unix_time_ms % 1000), level_letter(level));
    std::string text = head;
    text += tag;
    text += "] ";
    text += message;
    text += '\n';

    for (Sink& k : s.sinks) if (level >= k.min_level) write_to_sink_locked(k, rec, text);
}

NUE_API void NUE_CALL nue_log_write(nue_log_level level, const char* tag, const char* file, int line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    nue_log_writev(level, tag, file, line, fmt, args);
    va_end(args);
}

NUE_API void NUE_CALL nue_log_set_level(nue_log_level level) { LogState& s = state(); std::lock_guard<std::mutex> lock(s.mutex); s.level = level; }
NUE_API nue_log_level NUE_CALL nue_log_get_level(void) { LogState& s = state(); std::lock_guard<std::mutex> lock(s.mutex); return s.level; }

NUE_API void NUE_CALL nue_log_set_tag_level(const char* tag, nue_log_level level) {
    if (!tag) return;
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (level == 0) s.tag_levels.erase(tag); else s.tag_levels[tag] = level;
}

NUE_API nue_result NUE_CALL nue_log_add_sink(const nue_log_sink_desc* desc, nue_log_sink_id* out_id) {
    if (!desc) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_log_add_sink: desc is NULL");
    Sink k;
    k.kind = desc->kind;
    k.min_level = desc->min_level ? desc->min_level : NUE_LOG_TRACE;
    switch (desc->kind) {
        case NUE_LOG_SINK_STDERR:
        case NUE_LOG_SINK_PLATFORM:
            break;
        case NUE_LOG_SINK_FILE: {
            if (!desc->path || !*desc->path) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "file sink needs a path");
            std::string native = nuei::resolve_native_path(desc->path, true);
            if (native.empty()) return nuei::fail(NUE_ERR_NOT_FOUND, "log file path '%s' cannot be resolved", desc->path);
            nuei::os_make_parent_dirs(native);
            k.path = native;
            k.max_bytes = desc->max_bytes ? desc->max_bytes : 8u * 1024 * 1024;
            k.keep_files = desc->keep_files ? desc->keep_files : 3;
            // start each run with a fresh file, keeping the previous ones as path.1 ...
            k.file = nuei::fopen_utf8(native, "ab");
            if (!k.file) return nuei::fail(NUE_ERR_IO, "cannot open log file '%s'", native.c_str());
            std::fseek(k.file, 0, SEEK_END);
            k.bytes = uint64_t(std::ftell(k.file));
            if (k.bytes > 0) rotate_file_locked(k);
            break;
        }
        case NUE_LOG_SINK_CALLBACK:
            if (!desc->callback) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "callback sink needs a callback");
            k.callback = desc->callback;
            k.userdata = desc->userdata;
            break;
        case NUE_LOG_SINK_MEMORY_RING:
            k.ring_capacity = desc->max_bytes ? desc->max_bytes : 64 * 1024;
            break;
        default:
            return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "unknown log sink kind %d", int(desc->kind));
    }
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.defaults_done) add_default_stderr_locked(s);
    k.id = s.next_id++;
    if (out_id) *out_id = k.id;
    s.sinks.push_back(std::move(k));
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_log_remove_sink(nue_log_sink_id id) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (size_t i = 0; i < s.sinks.size(); ++i) {
        if (s.sinks[i].id != id) continue;
        if (s.sinks[i].file) std::fclose(s.sinks[i].file);
        s.sinks.erase(s.sinks.begin() + long(i));
        return NUE_OK;
    }
    return NUE_ERR_NOT_FOUND;
}

NUE_API void NUE_CALL nue_log_flush(void) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (Sink& k : s.sinks) if (k.file) std::fflush(k.file);
    std::fflush(stderr);
}

NUE_API nue_result NUE_CALL nue_log_get_recent(char* buffer, size_t capacity, size_t* out_size) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (const Sink& k : s.sinks) if (k.kind == NUE_LOG_SINK_MEMORY_RING) return nuei::copy_out_string(k.ring, buffer, capacity, out_size);
    return nuei::copy_out_string("", 0, buffer, capacity, out_size);
}

}  // extern "C"
