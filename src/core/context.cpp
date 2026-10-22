// Engine context: nue_init / nue_shutdown, allocator, build info, extensions, shared helpers (fail, copy_out_string).
#include "internal/core_internal.h"
#include "internal/os_util.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#ifndef NUE_GIT_COMMIT
#define NUE_GIT_COMMIT "unknown"
#endif
#ifndef NUE_BUILD_TYPE_STRING
#define NUE_BUILD_TYPE_STRING "unknown"
#endif
#ifndef NUE_VERSION_STRING_LITERAL
#define NUE_VERSION_STRING_LITERAL "0.1.0"
#endif

namespace {

struct Context {
    std::mutex mutex;                    // serialises init/shutdown
    int refcount = 0;
    nuei::InitInfo info;
    const nue_backend* backend = nullptr;
    const nue_platform_ops* platform = nullptr;
    std::atomic<uint64_t> event_thread{0};
    nue_allocator allocator{};
    bool has_allocator = false;
    std::mutex ext_mutex;
    std::map<std::string, const void*> extensions;
};

Context& ctx() {
    static Context* c = new Context();
    return *c;
}

std::atomic<bool> g_initialized{false};
std::atomic<const nue_backend*> g_backend{nullptr};
std::atomic<const nue_platform_ops*> g_platform{nullptr};
std::atomic<bool> g_headless{false};
std::atomic<bool> g_deterministic{false};
std::atomic<uint32_t> g_subsystems{0};

struct AllocHeader {
    void* raw;
    size_t payload_size;
};

std::string str_or(const char* s, const char* fallback = "") { return s ? s : fallback; }

}  // namespace

// ---- helpers shared by the core -------------------------------------------------------------------------------------------------

namespace nuei {

bool initialized() { return g_initialized.load(); }
const InitInfo& init_info() { return ctx().info; }
const nue_backend* backend() { return g_backend.load(); }
const nue_platform_ops* platform() {
    const nue_platform_ops* p = g_platform.load();
    return p ? p : nue_platform_generic_get_ops();
}
bool subsystem_enabled(nue_subsystem_mask mask) { return (g_subsystems.load() & mask) == mask; }
bool headless() { return g_headless.load(); }
bool deterministic() { return g_deterministic.load(); }

nue_result fail(nue_result code, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    nue_set_last_error("%s", buf);
    return code;
}

nue_result require_init() {
    if (g_initialized.load()) return NUE_OK;
    return fail(NUE_ERR_NOT_INITIALIZED, "the engine is not initialised (call nue_init first)");
}

nue_result copy_out_string(const char* text, size_t length, char* buffer, size_t capacity, size_t* out_size) {
    if (out_size) *out_size = length + 1;
    if (buffer && capacity) {
        size_t n = length < capacity - 1 ? length : capacity - 1;
        std::memcpy(buffer, text, n);
        buffer[n] = 0;
    }
    return NUE_OK;
}

void* alloc_mem(size_t size, size_t alignment) {
    Context& c = ctx();
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    size_t total = size + alignment + sizeof(AllocHeader);
    void* raw = c.has_allocator && c.allocator.alloc ? c.allocator.alloc(c.allocator.userdata, total, sizeof(void*)) : std::malloc(total);
    if (!raw) return nullptr;
    uintptr_t addr = (reinterpret_cast<uintptr_t>(raw) + sizeof(AllocHeader) + alignment - 1) & ~(uintptr_t(alignment) - 1);
    AllocHeader* h = reinterpret_cast<AllocHeader*>(addr) - 1;
    h->raw = raw;
    h->payload_size = size;
    return reinterpret_cast<void*>(addr);
}

void free_mem(void* p) {
    if (!p) return;
    Context& c = ctx();
    AllocHeader* h = static_cast<AllocHeader*>(p) - 1;
    if (c.has_allocator && c.allocator.free) c.allocator.free(c.allocator.userdata, h->raw, 0);
    else std::free(h->raw);
}

}  // namespace nuei

void* nue_internal_alloc(size_t size) { return nuei::alloc_mem(size); }
void nue_internal_free(void* ptr) { nuei::free_mem(ptr); }

void nue_internal_register_extension(const char* name, const void* table) {
    if (!name) return;
    Context& c = ctx();
    std::lock_guard<std::mutex> lock(c.ext_mutex);
    if (table) c.extensions[name] = table; else c.extensions.erase(name);
}

void nuei::extensions_shutdown() {
    Context& c = ctx();
    std::lock_guard<std::mutex> lock(c.ext_mutex);
    c.extensions.clear();
}

// ---- public API ----------------------------------------------------------------------------------------------------------------

extern "C" {

NUE_API nue_result NUE_CALL nue_init(const nue_init_desc* user_desc) {
    Context& c = ctx();
    std::lock_guard<std::mutex> lock(c.mutex);

    nue_init_desc desc;
    NUE_STRUCT_INIT(&desc);
    if (user_desc) {
        size_t n = user_desc->struct_size < sizeof(desc) ? user_desc->struct_size : sizeof(desc);
        if (n < sizeof(uint32_t) * 2) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_init: struct_size is too small (%u)", user_desc->struct_size);
        std::memcpy(&desc, user_desc, n);
        desc.struct_size = sizeof(desc);
    } else {
        desc.api_version = NUE_API_VERSION;
    }
    if (desc.api_version == 0) desc.api_version = NUE_API_VERSION;
    if (NUE_VERSION_GET_MAJOR(desc.api_version) != NUE_VERSION_MAJOR ||
        (NUE_VERSION_GET_MAJOR(desc.api_version) == NUE_VERSION_MAJOR && NUE_VERSION_GET_MINOR(desc.api_version) > NUE_VERSION_MINOR)) {
        return nuei::fail(NUE_ERR_VERSION_MISMATCH, "nue_init: the caller was built against API %u.%u.%u, this library is %u.%u.%u",
                          NUE_VERSION_GET_MAJOR(desc.api_version), NUE_VERSION_GET_MINOR(desc.api_version), NUE_VERSION_GET_PATCH(desc.api_version),
                          NUE_VERSION_MAJOR, NUE_VERSION_MINOR, NUE_VERSION_PATCH);
    }

    if (c.refcount > 0) { ++c.refcount; return NUE_OK; }

    nuei::InitInfo& info = c.info;
    info = nuei::InitInfo();
    info.desc = desc;
    info.app_name = str_or(desc.app_name, "NUE application");
    info.app_id = str_or(desc.app_id, "nue-app");
    info.org_name = str_or(desc.org_name, "NUE");
    info.app_version = str_or(desc.app_version, "");
    info.game_data_path = str_or(desc.game_data_path);
    info.user_data_path = str_or(desc.user_data_path);
    info.config_file = desc.config_file ? desc.config_file : "settings:nue.ini";
    info.log_file = desc.log_file ? desc.log_file : "user:logs/" + info.app_id + ".log";
    info.backend = desc.backend;
    info.loop_model = desc.loop_model;
    info.subsystems = desc.subsystems ? desc.subsystems : NUE_SUBSYSTEM_ALL_DEFAULT;
    info.flags = desc.flags;
    info.log_level = desc.log_level ? desc.log_level : NUE_LOG_INFO;
    info.event_queue_capacity = desc.event_queue_capacity ? desc.event_queue_capacity : 4096;
    info.desc.app_name = info.desc.app_id = info.desc.org_name = info.desc.app_version = nullptr;   // pointers die with the call
    info.desc.game_data_path = info.desc.user_data_path = info.desc.config_file = info.desc.log_file = nullptr;
    info.desc.allocator = nullptr;

    if (desc.allocator && desc.allocator->struct_size >= sizeof(uint32_t) * 2 + sizeof(void*)) {
        c.allocator = *desc.allocator;
        c.has_allocator = c.allocator.alloc && c.allocator.free;
    } else {
        c.has_allocator = false;
    }

    const bool headless = (info.flags & NUE_INIT_HEADLESS) != 0;
    g_headless = headless;
    g_deterministic = (info.flags & NUE_INIT_DETERMINISTIC) != 0;
    g_subsystems = info.subsystems;

    // platform layer, then backend
    switch (info.backend) {
        case NUE_BACKEND_NULL: c.backend = nue_backend_null_get(); break;
        case NUE_BACKEND_SDL3:
#if defined(NUE_HAS_SDL3)
            c.backend = nue_backend_sdl3_get();
#else
            c.backend = nullptr;
#endif
            if (!c.backend) return nuei::fail(NUE_ERR_UNSUPPORTED, "nue_init: this build has no SDL3 backend");
            break;
        default:
#if defined(NUE_HAS_SDL3)
            c.backend = (headless || g_deterministic.load()) ? nue_backend_null_get() : nue_backend_sdl3_get();
#else
            c.backend = nue_backend_null_get();
#endif
            break;
    }
    if (g_deterministic.load() && c.backend != nue_backend_null_get())
        return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_init: NUE_INIT_DETERMINISTIC requires the null backend");
    c.platform = nue_platform_get_ops();
    g_platform = c.platform;
    g_backend = c.backend;

    nuei::log_init(info);
    nuei::config_init();

    nue_backend_init binit;
    NUE_STRUCT_INIT(&binit);
    binit.desc = &info.desc;
    binit.headless = headless ? NUE_TRUE : NUE_FALSE;
    binit.deterministic = g_deterministic.load() ? NUE_TRUE : NUE_FALSE;
    nue_result r = c.backend->init(&binit);
    if (NUE_FAILED(r)) {
        NUE_LOGE("core", "backend '%s' failed to start: %s", c.backend->name, nue_get_last_error_message());
        g_backend = nullptr; g_platform = nullptr;
        nuei::log_shutdown();
        return r;
    }
    if (c.platform->init) {
        r = c.platform->init(&binit);
        if (NUE_FAILED(r)) NUE_LOGW("core", "platform layer '%s' init failed: %s (continuing without it)", c.platform->name, nue_get_last_error_message());
    }

    r = nuei::events_init(info.event_queue_capacity);
    if (NUE_FAILED(r)) {
        if (c.platform->shutdown) c.platform->shutdown();
        c.backend->shutdown();
        g_backend = nullptr; g_platform = nullptr;
        nuei::log_shutdown();
        return r;
    }
    c.event_thread = nuei::current_thread_id();
    c.refcount = 1;
    g_initialized = true;

    nuei::vfs_init(info);
    nuei::log_open_file_sink(info);
    nuei::power_init();
    if (!(info.flags & NUE_INIT_NO_CONFIG_LOAD) && !info.config_file.empty()) {
        if (nue_vfs_exists(info.config_file.c_str())) {
            nue_result lr = nue_config_load(info.config_file.c_str());
            if (NUE_FAILED(lr)) NUE_LOGW("core", "settings file '%s' could not be read: %s", info.config_file.c_str(), nue_get_last_error_message());
        }
    }
    nuei::diag_init(info);
    NUE_LOGI("core", "NUE %s started: app '%s' (%s), backend %s, platform layer %s, %s, %d-bit", nue_get_version_string(), info.app_name.c_str(),
             info.app_id.c_str(), c.backend->name, c.platform->name, headless ? "headless" : "windowed", NUE_POINTER_SIZE * 8);
    return NUE_OK;
}

NUE_API void NUE_CALL nue_shutdown(void) {
    Context& c = ctx();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (c.refcount == 0) return;
    if (--c.refcount > 0) return;

    NUE_LOGI("core", "shutting down");
    nuei::diag_shutdown();
    nuei::gfx_shutdown_all();
    nuei::mixer_shutdown_all();
    nuei::audio_shutdown();
    nuei::io_shutdown();
    nuei::windows_shutdown();
    nuei::power_shutdown();
    if (c.platform && c.platform->shutdown) c.platform->shutdown();
    nuei::events_shutdown();
    if (c.backend) c.backend->shutdown();
    g_initialized = false;
    nuei::vfs_shutdown();
    nuei::config_shutdown();
    nuei::extensions_shutdown();
    nuei::log_shutdown();
    g_backend = nullptr;
    g_platform = nullptr;
    g_headless = false;
    g_deterministic = false;
    g_subsystems = 0;
    c.backend = nullptr;
    c.platform = nullptr;
    c.has_allocator = false;
}

NUE_API nue_bool NUE_CALL nue_is_initialized(void) { return nuei::initialized() ? NUE_TRUE : NUE_FALSE; }

NUE_API nue_result NUE_CALL nue_get_build_info(nue_build_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    static const char* backends =
#if defined(NUE_HAS_SDL3)
        "null,sdl3";
#else
        "null";
#endif
    nue_build_info b;
    NUE_STRUCT_INIT(&b);
    b.version = NUE_API_VERSION;
    b.version_string = NUE_VERSION_STRING_LITERAL;
    b.git_commit = NUE_GIT_COMMIT;
#if defined(__clang__)
    b.compiler = "clang " __clang_version__;
#elif defined(_MSC_VER)
    b.compiler = "msvc";
#elif defined(__GNUC__)
    b.compiler = "gcc " __VERSION__;
#else
    b.compiler = "unknown";
#endif
    b.build_type = NUE_BUILD_TYPE_STRING;
    b.backends = backends;
    b.platform_layer = nuei::platform()->name ? nuei::platform()->name : "generic";
#if defined(NUE_HAS_SDL3)
    b.sdl_version = "3.4.18";
#else
    b.sdl_version = "";
#endif
    b.pointer_size = sizeof(void*);
#if defined(NUE_BUILD_SHARED)
    b.shared_library = NUE_TRUE;
#endif
#if defined(NUE_HAS_GFX)
    b.has_gfx = NUE_TRUE;
#endif
#if !defined(NDEBUG)
    b.debug_build = NUE_TRUE;
#endif
    size_t n = out->struct_size < sizeof(b) ? out->struct_size : sizeof(b);
    uint32_t keep = out->struct_size;
    std::memcpy(out, &b, n);
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API const void* NUE_CALL nue_get_extension(const char* name) {
    if (!name) return nullptr;
    Context& c = ctx();
    std::lock_guard<std::mutex> lock(c.ext_mutex);
    auto it = c.extensions.find(name);
    return it == c.extensions.end() ? nullptr : it->second;
}

NUE_API void* NUE_CALL nue_alloc(size_t size, size_t alignment) { return nuei::alloc_mem(size, alignment); }

NUE_API void* NUE_CALL nue_realloc(void* ptr, size_t new_size, size_t alignment) {
    if (!ptr) return nuei::alloc_mem(new_size, alignment);
    AllocHeader* h = static_cast<AllocHeader*>(ptr) - 1;
    size_t old = h->payload_size;
    void* n = nuei::alloc_mem(new_size, alignment);
    if (!n) return nullptr;
    std::memcpy(n, ptr, old < new_size ? old : new_size);
    nuei::free_mem(ptr);
    return n;
}

NUE_API void NUE_CALL nue_free(void* ptr) { nuei::free_mem(ptr); }

NUE_API nue_result NUE_CALL nue_set_event_thread(void) {
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    ctx().event_thread = nuei::current_thread_id();
    return NUE_OK;
}

NUE_API nue_bool NUE_CALL nue_is_event_thread(void) {
    return ctx().event_thread.load() == nuei::current_thread_id() ? NUE_TRUE : NUE_FALSE;
}

}  // extern "C"

// ---- platform selection ---------------------------------------------------------------------------------------------------------

namespace {
const nue_platform_ops g_generic_ops = [] {
    nue_platform_ops ops;
    std::memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.name = "generic";
    return ops;
}();
}  // namespace

extern "C" {

const nue_platform_ops* nue_platform_generic_get_ops(void) { return &g_generic_ops; }

const nue_platform_ops* nue_platform_get_ops(void) {
    const nue_platform_ops* p = nullptr;
#if defined(NUE_HAS_PLATFORM_WINDOWS)
    p = nue_platform_windows_get_ops();
#elif defined(NUE_HAS_PLATFORM_LINUX)
    p = nue_platform_linux_get_ops();
#elif defined(NUE_HAS_PLATFORM_ANDROID)
    p = nue_platform_android_get_ops();
#elif defined(NUE_HAS_PLATFORM_IOS)
    p = nue_platform_ios_get_ops();
#elif defined(NUE_HAS_PLATFORM_MACOS)
    p = nue_platform_macos_get_ops();
#endif
    return p ? p : &g_generic_ops;
}

}  // extern "C"
