// Clocks, precise sleep, threads, mutex/cond/semaphore, thread local storage.
#include "internal/core_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
#  include <pthread.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#endif

namespace nuei {

int64_t now_ns() {
    const nue_backend* b = backend();
    if (b && b->time_ns) return b->time_ns();
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t current_thread_id() {
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__linux__) || defined(__ANDROID__)
    return uint64_t(syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

void set_thread_name(const char* name) {
    if (!name) return;
    const nue_platform_ops* p = platform();
    if (p && p->thread_set_name && NUE_SUCCEEDED(p->thread_set_name(name))) return;
#if defined(_WIN32)
    // SetThreadDescription exists on Windows 10 1607+; resolve dynamically so older systems still load.
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static SetDesc fn = reinterpret_cast<SetDesc>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (fn) {
        wchar_t wide[64];
        int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 63);
        if (n > 0) { wide[n < 63 ? n : 63] = 0; fn(GetCurrentThread(), wide); }
    }
#elif defined(__linux__) || defined(__ANDROID__)
    char shortname[16];
    std::strncpy(shortname, name, 15);
    shortname[15] = 0;
    pthread_setname_np(pthread_self(), shortname);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#endif
}

}  // namespace nuei

namespace {

// ---- thread local storage -----------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxTls = 128;
std::mutex g_tls_mutex;
void (NUE_CALL* g_tls_destructors[kMaxTls])(void*);
bool g_tls_used[kMaxTls];

struct TlsValues {
    void* values[kMaxTls] = {};
    ~TlsValues() {
        for (int pass = 0; pass < 4; ++pass) {   // destructors may set values again
            bool any = false;
            for (uint32_t i = 0; i < kMaxTls; ++i) {
                void* v = values[i];
                if (!v) continue;
                values[i] = nullptr;
                void (NUE_CALL* d)(void*) = nullptr;
                { std::lock_guard<std::mutex> lock(g_tls_mutex); if (g_tls_used[i]) d = g_tls_destructors[i]; }
                if (d) { d(v); any = true; }
            }
            if (!any) break;
        }
    }
};
thread_local TlsValues t_tls;

// ---- sync objects -------------------------------------------------------------------------------------------------------------
}  // namespace

struct nue_mutex_s {
    std::mutex plain;
    std::recursive_mutex rec;
    bool recursive = false;
    void lock() { if (recursive) rec.lock(); else plain.lock(); }
    bool try_lock() { return recursive ? rec.try_lock() : plain.try_lock(); }
    void unlock() { if (recursive) rec.unlock(); else plain.unlock(); }
};

struct nue_cond_s { std::condition_variable_any cv; };

struct nue_semaphore_s {
    std::mutex m;
    std::condition_variable cv;
    uint32_t count = 0;
};

struct nue_thread_s {
    std::thread thread;
    nue_thread_fn fn = nullptr;
    void* userdata = nullptr;
    std::atomic<int32_t> exit_code{0};
    std::atomic<uint64_t> id{0};
    std::atomic<bool> detached{false};
    std::atomic<bool> finished{false};
    std::mutex gate;   // serialises detach vs. completion so exactly one side frees the object
    std::string name;
    nue_thread_priority priority = NUE_THREAD_PRIORITY_DEFAULT;
    uint64_t affinity = 0;
};

namespace {

void thread_main(nue_thread_s* t) {
    t->id = nuei::current_thread_id();
    if (!t->name.empty()) nuei::set_thread_name(t->name.c_str());
    if (t->priority != NUE_THREAD_PRIORITY_DEFAULT || t->affinity) {
        nue_native_thread_ref self;
        NUE_STRUCT_INIT(&self);
        const nue_platform_ops* p = nuei::platform();
        if (p && t->priority != NUE_THREAD_PRIORITY_DEFAULT && p->thread_set_priority) p->thread_set_priority(&self, t->priority);
        if (p && t->affinity && p->thread_set_affinity) p->thread_set_affinity(&self, t->affinity);
    }
    t->exit_code = t->fn(t->userdata);
    bool delete_self = false;
    {
        std::lock_guard<std::mutex> lock(t->gate);
        t->finished = true;
        delete_self = t->detached.load();
    }
    if (delete_self) delete t;   // detached: nobody else holds the pointer; the std::thread was detached in nue_thread_detach
}

}  // namespace

extern "C" {

NUE_API int64_t NUE_CALL nue_time_ns(void) { return nuei::now_ns(); }

NUE_API uint64_t NUE_CALL nue_time_counter(void) {
#if defined(_WIN32)
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return uint64_t(v.QuadPart);
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

NUE_API uint64_t NUE_CALL nue_time_counter_frequency(void) {
#if defined(_WIN32)
    static uint64_t f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return uint64_t(v.QuadPart); }();
    return f;
#else
    return 1000000000ull;
#endif
}

NUE_API int64_t NUE_CALL nue_time_unix_ms(void) {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int32_t nue_null_advance_time_internal(int64_t ns);   // backend_null.cpp (deterministic mode only)

NUE_API void NUE_CALL nue_sleep_until_ns(int64_t deadline_ns, nue_sleep_mode mode) {
    if (nuei::deterministic()) {
        int64_t now = nuei::now_ns();
        if (deadline_ns > now) nue_null_advance_time_internal(deadline_ns - now);
        return;
    }
    const nue_platform_ops* p = nuei::platform();
    if (p && p->sleep_until && NUE_SUCCEEDED(p->sleep_until(deadline_ns))) return;
    const int64_t spin_window = (mode == NUE_SLEEP_PRECISE) ? 1500000 : 0;   // 1.5 ms
    for (;;) {
        int64_t remaining = deadline_ns - nuei::now_ns();
        if (remaining <= 0) return;
        if (remaining > spin_window) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(remaining - spin_window));
            if (mode == NUE_SLEEP_COARSE) {
                // coarse mode: one OS sleep is the contract, do not loop until the deadline
                return;
            }
        } else {
            std::this_thread::yield();
        }
    }
}

NUE_API void NUE_CALL nue_sleep_ns(int64_t ns, nue_sleep_mode mode) {
    if (ns <= 0) { std::this_thread::yield(); return; }
    nue_sleep_until_ns(nuei::now_ns() + ns, mode);
}

// ---- threads ------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_thread_create(const nue_thread_desc* desc, nue_thread_fn fn, void* userdata, nue_thread* out_thread) {
    if (!fn || !out_thread) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_thread_create: fn and out_thread are required");
    *out_thread = nullptr;
    nue_thread_s* t = new (std::nothrow) nue_thread_s();
    if (!t) return NUE_ERR_OUT_OF_MEMORY;
    t->fn = fn;
    t->userdata = userdata;
    if (desc) {
        if (desc->name) t->name = desc->name;
        t->priority = desc->priority;
        t->affinity = desc->affinity_mask;
    }
    try {
        t->thread = std::thread(thread_main, t);   // desc->stack_size: std::thread cannot set it; platform layers may add a native path later
    } catch (...) {
        delete t;
        return nuei::fail(NUE_ERR_FAILED, "nue_thread_create: the OS refused to start a thread");
    }
    *out_thread = t;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_thread_join(nue_thread thread, int32_t* out_exit_code) {
    if (!thread) return NUE_ERR_INVALID_HANDLE;
    if (thread->thread.get_id() == std::this_thread::get_id()) return nuei::fail(NUE_ERR_BAD_STATE, "nue_thread_join: a thread cannot join itself");
    thread->thread.join();
    if (out_exit_code) *out_exit_code = thread->exit_code.load();
    delete thread;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_thread_detach(nue_thread thread) {
    if (!thread) return NUE_ERR_INVALID_HANDLE;
    bool delete_now = false;
    {
        std::lock_guard<std::mutex> lock(thread->gate);
        thread->thread.detach();
        if (thread->finished.load()) delete_now = true; else thread->detached = true;
    }
    if (delete_now) delete thread;
    return NUE_OK;
}

NUE_API uint64_t NUE_CALL nue_thread_current_id(void) { return nuei::current_thread_id(); }
NUE_API uint64_t NUE_CALL nue_thread_get_id(nue_thread thread) { return thread ? thread->id.load() : 0; }
NUE_API void NUE_CALL nue_thread_set_current_name(const char* name) { nuei::set_thread_name(name); }
NUE_API void NUE_CALL nue_thread_yield(void) { std::this_thread::yield(); }

// ---- mutex / cond / semaphore -------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_mutex_create(nue_bool recursive, nue_mutex* out_mutex) {
    if (!out_mutex) return NUE_ERR_INVALID_ARGUMENT;
    nue_mutex_s* m = new (std::nothrow) nue_mutex_s();
    if (!m) return NUE_ERR_OUT_OF_MEMORY;
    m->recursive = recursive != 0;
    *out_mutex = m;
    return NUE_OK;
}
NUE_API void NUE_CALL nue_mutex_destroy(nue_mutex mutex) { delete mutex; }
NUE_API void NUE_CALL nue_mutex_lock(nue_mutex mutex) { if (mutex) mutex->lock(); }
NUE_API nue_bool NUE_CALL nue_mutex_trylock(nue_mutex mutex) { return mutex && mutex->try_lock() ? NUE_TRUE : NUE_FALSE; }
NUE_API void NUE_CALL nue_mutex_unlock(nue_mutex mutex) { if (mutex) mutex->unlock(); }

NUE_API nue_result NUE_CALL nue_cond_create(nue_cond* out_cond) {
    if (!out_cond) return NUE_ERR_INVALID_ARGUMENT;
    *out_cond = new (std::nothrow) nue_cond_s();
    return *out_cond ? NUE_OK : NUE_ERR_OUT_OF_MEMORY;
}
NUE_API void NUE_CALL nue_cond_destroy(nue_cond cond) { delete cond; }
NUE_API nue_result NUE_CALL nue_cond_wait(nue_cond cond, nue_mutex mutex, int64_t timeout_ns) {
    if (!cond || !mutex) return NUE_ERR_INVALID_HANDLE;
    if (timeout_ns < 0) { cond->cv.wait(*mutex); return NUE_OK; }
    return cond->cv.wait_for(*mutex, std::chrono::nanoseconds(timeout_ns)) == std::cv_status::timeout ? NUE_ERR_TIMEOUT : NUE_OK;
}
NUE_API void NUE_CALL nue_cond_signal(nue_cond cond) { if (cond) cond->cv.notify_one(); }
NUE_API void NUE_CALL nue_cond_broadcast(nue_cond cond) { if (cond) cond->cv.notify_all(); }

NUE_API nue_result NUE_CALL nue_semaphore_create(uint32_t initial_value, nue_semaphore* out_sem) {
    if (!out_sem) return NUE_ERR_INVALID_ARGUMENT;
    nue_semaphore_s* s = new (std::nothrow) nue_semaphore_s();
    if (!s) return NUE_ERR_OUT_OF_MEMORY;
    s->count = initial_value;
    *out_sem = s;
    return NUE_OK;
}
NUE_API void NUE_CALL nue_semaphore_destroy(nue_semaphore sem) { delete sem; }
NUE_API nue_result NUE_CALL nue_semaphore_wait(nue_semaphore sem, int64_t timeout_ns) {
    if (!sem) return NUE_ERR_INVALID_HANDLE;
    std::unique_lock<std::mutex> lock(sem->m);
    if (timeout_ns < 0) {
        sem->cv.wait(lock, [&] { return sem->count > 0; });
    } else if (timeout_ns == 0) {
        if (sem->count == 0) return NUE_ERR_TIMEOUT;
    } else if (!sem->cv.wait_for(lock, std::chrono::nanoseconds(timeout_ns), [&] { return sem->count > 0; })) {
        return NUE_ERR_TIMEOUT;
    }
    --sem->count;
    return NUE_OK;
}
NUE_API void NUE_CALL nue_semaphore_post(nue_semaphore sem) {
    if (!sem) return;
    { std::lock_guard<std::mutex> lock(sem->m); ++sem->count; }
    sem->cv.notify_one();
}

// ---- TLS -----------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_tls_create(void (NUE_CALL* destructor)(void* value), nue_tls_id* out_id) {
    if (!out_id) return NUE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    for (uint32_t i = 1; i < kMaxTls; ++i) {   // slot 0 stays invalid so id 0 means "none"
        if (!g_tls_used[i]) { g_tls_used[i] = true; g_tls_destructors[i] = destructor; *out_id = i; return NUE_OK; }
    }
    return nuei::fail(NUE_ERR_OUT_OF_MEMORY, "nue_tls_create: all %u slots are in use", kMaxTls - 1);
}
NUE_API void NUE_CALL nue_tls_destroy(nue_tls_id id) {
    if (id == 0 || id >= kMaxTls) return;
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    g_tls_used[id] = false;
    g_tls_destructors[id] = nullptr;
}
NUE_API void* NUE_CALL nue_tls_get(nue_tls_id id) { return (id && id < kMaxTls) ? t_tls.values[id] : nullptr; }
NUE_API nue_result NUE_CALL nue_tls_set(nue_tls_id id, void* value) {
    if (id == 0 || id >= kMaxTls) return NUE_ERR_INVALID_ARGUMENT;
    t_tls.values[id] = value;
    return NUE_OK;
}

}  // extern "C"
