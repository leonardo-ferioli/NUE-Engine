// Periodic / one-shot timers. Real mode: one thread per timer sleeping until its next tick. Deterministic mode (null backend): no threads; the
// virtual clock calls timers_run_due() after every step, so tests see exact, repeatable tick sequences.
#include "internal/core_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct nue_timer_s {
    nue_timer_desc desc;
    std::string name;
    std::atomic<int64_t> period_ns{0};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> in_callback{false};
    std::atomic<uint64_t> tick{0};
    int64_t next_ns = 0;
    bool virtual_mode = false;
    std::thread thread;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    std::atomic<std::thread::id> callback_thread{};
};

namespace {

struct TimerRegistry {
    std::mutex mutex;
    std::vector<nue_timer> virtual_timers;
};
TimerRegistry& R() { static TimerRegistry* r = new TimerRegistry(); return *r; }

void fire(nue_timer t) {
    t->in_callback = true;
    t->callback_thread = std::this_thread::get_id();
    uint64_t n = ++t->tick;
    t->desc.callback(t->desc.userdata, t, n);
    t->callback_thread = std::thread::id();
    t->in_callback = false;
}

void timer_thread(nue_timer t) {
    nuei::set_thread_name(t->name.empty() ? "nue-timer" : t->name.c_str());
    if (t->desc.priority != NUE_THREAD_PRIORITY_DEFAULT) nue_thread_set_priority(nullptr, t->desc.priority);
    while (!t->cancelled.load()) {
        int64_t now = nuei::now_ns();
        if (now < t->next_ns) {
            // coarse wait on the condition variable (cancel/period change wake it), the last stretch with a precise sleep
            int64_t remaining = t->next_ns - now;
            if (remaining > 2000000) {
                std::unique_lock<std::mutex> lock(t->wake_mutex);
                t->wake_cv.wait_for(lock, std::chrono::nanoseconds(remaining - 1500000), [&] { return t->cancelled.load(); });
                continue;
            }
            nue_sleep_until_ns(t->next_ns, NUE_SLEEP_PRECISE);
        }
        if (t->cancelled.load()) break;
        fire(t);
        if (t->desc.one_shot) { t->cancelled = true; break; }
        int64_t period = t->period_ns.load();
        t->next_ns += period;
        int64_t after = nuei::now_ns();
        if (t->next_ns <= after) t->next_ns = after + period - ((after - t->next_ns) % period);   // missed ticks are coalesced, not replayed
    }
}

}  // namespace

namespace nuei {

void timers_run_due(int64_t now) {
    std::vector<nue_timer> due;
    {
        TimerRegistry& r = R();
        std::lock_guard<std::mutex> lock(r.mutex);
        due = r.virtual_timers;
    }
    for (nue_timer t : due) {
        while (!t->cancelled.load() && t->next_ns <= now) {
            fire(t);
            if (t->desc.one_shot) { t->cancelled = true; break; }
            t->next_ns += t->period_ns.load();
        }
    }
}

}  // namespace nuei

extern "C" {

NUE_API nue_result NUE_CALL nue_timer_create(const nue_timer_desc* desc, nue_timer* out) {
    if (!desc || !out || !desc->callback || desc->period_ns <= 0) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_timer_create: callback and a positive period are required");
    *out = nullptr;
    nue_timer t = new (std::nothrow) nue_timer_s();
    if (!t) return NUE_ERR_OUT_OF_MEMORY;
    t->desc = *desc;
    t->name = desc->name ? desc->name : "";
    t->desc.name = nullptr;
    t->period_ns = desc->period_ns;
    t->next_ns = nuei::now_ns() + (desc->first_delay_ns > 0 ? desc->first_delay_ns : desc->period_ns);
    t->virtual_mode = nuei::deterministic();
    if (t->virtual_mode) {
        TimerRegistry& r = R();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.virtual_timers.push_back(t);
    } else {
        t->thread = std::thread(timer_thread, t);
    }
    *out = t;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_timer_cancel(nue_timer t) {
    if (!t) return NUE_ERR_INVALID_HANDLE;
    t->cancelled = true;
    t->wake_cv.notify_all();
    return NUE_OK;
}

NUE_API void NUE_CALL nue_timer_destroy(nue_timer t) {
    if (!t) return;
    t->cancelled = true;
    t->wake_cv.notify_all();
    if (t->virtual_mode) {
        TimerRegistry& r = R();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.virtual_timers.erase(std::remove(r.virtual_timers.begin(), r.virtual_timers.end(), t), r.virtual_timers.end());
    } else if (t->thread.joinable()) {
        if (t->callback_thread.load() == std::this_thread::get_id()) { nue_set_last_error("nue_timer_destroy called from the timer's own callback; use nue_timer_cancel"); return; }
        t->thread.join();
    }
    delete t;
}

NUE_API nue_result NUE_CALL nue_timer_set_period(nue_timer t, int64_t period_ns) {
    if (!t) return NUE_ERR_INVALID_HANDLE;
    if (period_ns <= 0) return NUE_ERR_INVALID_ARGUMENT;
    t->period_ns = period_ns;
    t->wake_cv.notify_all();
    return NUE_OK;
}

}  // extern "C"
