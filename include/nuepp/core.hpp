// NUE C++ wrapper: results, engine context, threads and synchronisation, timers, configuration, logging helpers.
// Header only, C++17, no exceptions thrown by this code and none required (builds with -fno-exceptions). Everything is a thin RAII layer over
// the C API in nue/*.h; the C API remains the ABI. Objects are move-only and release their handle in the destructor.
#pragma once

#include "nue/nue.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace nuepp {

// ---- results -----------------------------------------------------------------------------------------------------------------------------
class Result {
public:
    Result() = default;
    Result(nue_result r) : code_(r) {}   // NOLINT: implicit on purpose, so C results convert at the call site
    bool ok() const { return code_ >= 0; }
    explicit operator bool() const { return ok(); }
    nue_result code() const { return code_; }
    const char* name() const { return nue_result_name(code_); }
    // Detail text of the last failure on this thread (valid until the next NUE call on it).
    static const char* last_error() { return nue_get_last_error_message(); }
    std::string describe() const { return std::string(name()) + ": " + last_error(); }
private:
    nue_result code_ = NUE_OK;
};

// Value or error. No exceptions: check ok() before value(). value() of a failed Expected returns a default constructed T.
template <class T>
class Expected {
public:
    Expected(T value) : value_(std::move(value)) {}      // NOLINT
    Expected(Result error) : result_(error) {}           // NOLINT
    bool ok() const { return result_.ok(); }
    explicit operator bool() const { return ok(); }
    const Result& result() const { return result_; }
    T& value() { return value_; }
    const T& value() const { return value_; }
    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }
    T& operator*() { return value_; }
private:
    T value_{};
    Result result_;
};

// Fills `size`-prefixed C structs: nuepp::init<nue_init_desc>() == NUE_STRUCT_INIT.
template <class T>
inline T init() { T t; NUE_STRUCT_INIT(&t); return t; }

// ---- engine context ----------------------------------------------------------------------------------------------------------------------
// Holds one reference on the engine: nue_init in the constructor, nue_shutdown in the destructor.
class Context {
public:
    Context() = default;
    explicit Context(const nue_init_desc& desc) { init(desc); }
    Context(Context&& o) noexcept : active_(o.active_), result_(o.result_) { o.active_ = false; }
    Context& operator=(Context&& o) noexcept { if (this != &o) { reset(); active_ = o.active_; result_ = o.result_; o.active_ = false; } return *this; }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context() { reset(); }

    Result init(const nue_init_desc& desc) {
        reset();
        nue_init_desc d = desc;
        if (d.api_version == 0) d.api_version = NUE_API_VERSION;
        result_ = Result(nue_init(&d));
        active_ = result_.ok();
        return result_;
    }
    void reset() { if (active_) { nue_shutdown(); active_ = false; } }
    bool ok() const { return active_; }
    const Result& result() const { return result_; }
    static bool initialized() { return nue_is_initialized() != 0; }
    static std::string version() { return nue_get_version_string(); }

private:
    bool active_ = false;
    Result result_;
};

// ---- synchronisation ---------------------------------------------------------------------------------------------------------------------
class Mutex {
public:
    explicit Mutex(bool recursive = false) { nue_mutex_create(recursive ? NUE_TRUE : NUE_FALSE, &m_); }
    ~Mutex() { nue_mutex_destroy(m_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock() { nue_mutex_lock(m_); }
    bool try_lock() { return nue_mutex_trylock(m_) != 0; }
    void unlock() { nue_mutex_unlock(m_); }
    nue_mutex handle() const { return m_; }
private:
    nue_mutex m_ = nullptr;
};

class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    Mutex& m_;
};

class Semaphore {
public:
    explicit Semaphore(uint32_t initial = 0) { nue_semaphore_create(initial, &s_); }
    ~Semaphore() { nue_semaphore_destroy(s_); }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    void post() { nue_semaphore_post(s_); }
    bool wait(int64_t timeout_ns = -1) { return nue_semaphore_wait(s_, timeout_ns) == NUE_OK; }
private:
    nue_semaphore s_ = nullptr;
};

// Runs a callable on a NUE thread. The callable must not throw. join() or the destructor waits for it.
class Thread {
public:
    Thread() = default;
    template <class F>
    explicit Thread(F fn, const char* name = nullptr, nue_thread_priority priority = NUE_THREAD_PRIORITY_DEFAULT) { start(std::move(fn), name, priority); }
    Thread(Thread&& o) noexcept : t_(o.t_), fn_(o.fn_) { o.t_ = nullptr; o.fn_ = nullptr; }
    Thread& operator=(Thread&& o) noexcept { if (this != &o) { join(); t_ = o.t_; fn_ = o.fn_; o.t_ = nullptr; o.fn_ = nullptr; } return *this; }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    ~Thread() { join(); }

    template <class F>
    Result start(F fn, const char* name = nullptr, nue_thread_priority priority = NUE_THREAD_PRIORITY_DEFAULT) {
        join();
        struct Holder { F fn; };
        Holder* h = new Holder{std::move(fn)};
        nue_thread_desc d = init<nue_thread_desc>();
        d.name = name;
        d.priority = priority;
        nue_result r = nue_thread_create(&d, [](void* p) -> int32_t { Holder* hh = static_cast<Holder*>(p); hh->fn(); delete hh; return 0; }, h, &t_);
        if (NUE_FAILED(r)) { delete h; t_ = nullptr; }
        return Result(r);
    }
    void join() { if (t_) { nue_thread_join(t_, nullptr); t_ = nullptr; } }
    bool joinable() const { return t_ != nullptr; }
private:
    nue_thread t_ = nullptr;
    void* fn_ = nullptr;
};

// ---- timers ------------------------------------------------------------------------------------------------------------------------------
class Timer {
public:
    Timer() = default;
    // The callable is invoked on the timer thread with the tick number; it must not throw and must outlive the timer (a pointer is kept).
    template <class F>
    Timer(F* callable, int64_t period_ns, bool one_shot = false) { start(callable, period_ns, one_shot); }
    Timer(Timer&& o) noexcept : t_(o.t_) { o.t_ = nullptr; }
    Timer& operator=(Timer&& o) noexcept { if (this != &o) { reset(); t_ = o.t_; o.t_ = nullptr; } return *this; }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    ~Timer() { reset(); }

    template <class F>
    Result start(F* callable, int64_t period_ns, bool one_shot = false) {
        reset();
        nue_timer_desc d = init<nue_timer_desc>();
        d.period_ns = period_ns;
        d.one_shot = one_shot ? NUE_TRUE : NUE_FALSE;
        d.userdata = callable;
        d.callback = [](void* u, nue_timer, uint64_t tick) { (*static_cast<F*>(u))(tick); };
        return Result(nue_timer_create(&d, &t_));
    }
    void cancel() { if (t_) nue_timer_cancel(t_); }
    void reset() { if (t_) { nue_timer_destroy(t_); t_ = nullptr; } }
private:
    nue_timer t_ = nullptr;
};

// ---- time --------------------------------------------------------------------------------------------------------------------------------
inline int64_t now_ns() { return nue_time_ns(); }
inline void sleep_ns(int64_t ns, bool precise = true) { nue_sleep_ns(ns, precise ? NUE_SLEEP_PRECISE : NUE_SLEEP_COARSE); }

// ---- configuration -----------------------------------------------------------------------------------------------------------------------
namespace config {
inline bool get(const char* key, bool def) { return nue_config_get_bool(key, def ? NUE_TRUE : NUE_FALSE) != 0; }
inline int64_t get(const char* key, int64_t def) { return nue_config_get_int(key, def); }
inline int get(const char* key, int def) { return int(nue_config_get_int(key, def)); }
inline double get(const char* key, double def) { return nue_config_get_double(key, def); }
inline std::string get(const char* key, const char* def) {
    size_t need = 0;
    nue_config_get_string(key, def, nullptr, 0, &need);
    std::string s(need ? need : 1, '\0');
    nue_config_get_string(key, def, &s[0], s.size(), &need);
    s.resize(need ? need - 1 : 0);
    return s;
}
inline Result set(const char* key, bool v) { return nue_config_set_bool(key, v ? NUE_TRUE : NUE_FALSE); }
inline Result set(const char* key, int64_t v) { return nue_config_set_int(key, v); }
inline Result set(const char* key, int v) { return nue_config_set_int(key, v); }
inline Result set(const char* key, double v) { return nue_config_set_double(key, v); }
inline Result set(const char* key, const char* v) { return nue_config_set_string(key, v); }
inline Result load(const char* path = nullptr) { return nue_config_load(path); }
inline Result save(const char* path = nullptr) { return nue_config_save(path); }
}  // namespace config

// ---- diagnostics helpers -----------------------------------------------------------------------------------------------------------------
// Trace zone for the Chrome-trace output: { nuepp::TraceZone z("update"); ... }
class TraceZone {
public:
    explicit TraceZone(const char* name) { NUE_TRACE_BEGIN(name); }
    ~TraceZone() { NUE_TRACE_END(); }
    TraceZone(const TraceZone&) = delete;
    TraceZone& operator=(const TraceZone&) = delete;
};

// Memory budget with RAII registration: report()/request() account against the engine's configurable limits (memory.h).
class Budget {
public:
    Budget() = default;
    Budget(const char* name, nue_budget_kind kind, nue_budget_evict_fn evict = nullptr, void* userdata = nullptr) {
        nue_budget_desc d = init<nue_budget_desc>();
        d.name = name; d.kind = kind; d.evict = evict; d.userdata = userdata;
        nue_budget_register(&d, &id_);
    }
    ~Budget() { if (id_) nue_budget_unregister(id_); }
    Budget(Budget&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    Budget& operator=(Budget&& o) noexcept { if (this != &o) { if (id_) nue_budget_unregister(id_); id_ = o.id_; o.id_ = 0; } return *this; }
    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;
    Result request(uint64_t bytes) { return nue_budget_request(id_, bytes); }
    void report(int64_t delta) { nue_budget_report(id_, delta); }
    nue_budget_state state() const { nue_budget_state s = init<nue_budget_state>(); nue_budget_get_state(id_, &s); return s; }
    nue_budget_id id() const { return id_; }
private:
    nue_budget_id id_ = 0;
};

}  // namespace nuepp
