// Event queue, producers' entry point (nue_internal_push_event), watchers, host loop, app lifecycle and the engine-owned loop.
#include "internal/core_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <vector>

namespace {

struct Watcher { nue_event_watcher fn; void* userdata; };

struct EventState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<nue_event> queue;
    uint32_t capacity = 4096;
    uint64_t dropped = 0;
    std::set<uint32_t> disabled;
    std::vector<Watcher> watchers;
    std::atomic<bool> quit_requested{false};
    std::atomic<int> app_state{NUE_APP_STATE_FOREGROUND};
    bool active = false;
};

EventState& es() {
    static EventState* s = new EventState();
    return *s;
}

bool droppable(uint32_t type) {
    return type == NUE_EVENT_MOUSE_MOTION || type == NUE_EVENT_GAMEPAD_AXIS || type == NUE_EVENT_SENSOR_UPDATE || type == NUE_EVENT_FINGER_MOTION;
}

void push_internal(nue_event ev, bool run_input_state) {
    EventState& s = es();
    if (!s.active) return;
    if (ev.timestamp_ns == 0) ev.timestamp_ns = nuei::now_ns();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.disabled.count(ev.type)) return;
    }
    std::vector<nue_event> derived;
    if (run_input_state) nuei::input_on_event(ev, derived);
    if (ev.type == NUE_EVENT_QUIT) s.quit_requested = true;

    std::vector<Watcher> watchers;
    { std::lock_guard<std::mutex> lock(s.mutex); watchers = s.watchers; }
    bool queue_it = true;
    for (const Watcher& w : watchers) if (w.fn && !w.fn(w.userdata, &ev)) queue_it = false;
    if (queue_it) {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.queue.size() >= s.capacity) {
            // make room: drop the oldest high-rate event, else drop the oldest event of any kind (never silently the newest)
            auto it = std::find_if(s.queue.begin(), s.queue.end(), [](const nue_event& e) { return droppable(e.type); });
            if (it == s.queue.end()) it = s.queue.begin();
            s.queue.erase(it);
            ++s.dropped;
        }
        s.queue.push_back(ev);
    }
    s.cv.notify_all();
    const nue_backend* b = nuei::backend();
    if (b && b->wake) b->wake();
    for (const nue_event& d : derived) push_internal(d, false);
}

}  // namespace

namespace nuei {

nue_result events_init(uint32_t capacity) {
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.queue.clear();
    s.capacity = capacity ? capacity : 4096;
    s.dropped = 0;
    s.disabled.clear();
    s.watchers.clear();
    s.quit_requested = false;
    s.app_state = NUE_APP_STATE_FOREGROUND;
    s.active = true;
    input_state_reset();
    return NUE_OK;
}

void events_shutdown() {
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.dropped) NUE_LOGW("events", "%llu events were dropped because the queue (capacity %u) was full", (unsigned long long)s.dropped, s.capacity);
    s.active = false;
    s.queue.clear();
    s.watchers.clear();
}

}  // namespace nuei

extern "C" {

void nue_internal_push_event(const nue_event* event) { if (event) push_internal(*event, true); }

void nue_internal_set_app_state(nue_app_state state) {
    EventState& s = es();
    int prev = s.app_state.exchange(int(state));
    if (prev == int(state)) return;
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    auto emit = [&](nue_event_type t) { e.type = t; push_internal(e, true); };
    switch (state) {
        case NUE_APP_STATE_BACKGROUND: emit(NUE_EVENT_APP_PAUSING); emit(NUE_EVENT_APP_PAUSED); break;
        case NUE_APP_STATE_FOREGROUND:
            if (prev == NUE_APP_STATE_BACKGROUND) { emit(NUE_EVENT_APP_RESUMING); emit(NUE_EVENT_APP_RESUMED); }
            break;
        case NUE_APP_STATE_TERMINATING: emit(NUE_EVENT_APP_TERMINATING); break;
        case NUE_APP_STATE_INACTIVE: break;
    }
}

NUE_API nue_app_state NUE_CALL nue_app_get_state(void) { return nue_app_state(es().app_state.load()); }

NUE_API int32_t NUE_CALL nue_pump_events(void) {
    if (!nuei::initialized()) return 0;
    const nue_backend* b = nuei::backend();
    int32_t n = b && b->pump ? b->pump() : 0;
    const nue_platform_ops* p = nuei::platform();
    if (p && p->pump) p->pump();
    nuei::power_poll_and_notify();
    nuei::windows_after_pump();
    return n;
}

NUE_API nue_bool NUE_CALL nue_poll_event(nue_event* out) {
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.queue.empty()) return NUE_FALSE;
    if (out) *out = s.queue.front();
    s.queue.pop_front();
    return NUE_TRUE;
}

NUE_API nue_bool NUE_CALL nue_wait_event(nue_event* out, int32_t timeout_ms) {
    EventState& s = es();
    const int64_t deadline = timeout_ms < 0 ? INT64_MAX : nuei::now_ns() + int64_t(timeout_ms) * 1000000;
    for (;;) {
        nue_pump_events();
        if (nue_poll_event(out)) return NUE_TRUE;
        int64_t remaining_ns = deadline == INT64_MAX ? INT64_MAX : deadline - nuei::now_ns();
        if (remaining_ns <= 0) return NUE_FALSE;
        if (nuei::deterministic()) { nue_sleep_ns(remaining_ns < 1000000 ? remaining_ns : 1000000, NUE_SLEEP_COARSE); continue; }
        const nue_backend* b = nuei::backend();
        int32_t slice_ms = remaining_ns == INT64_MAX ? 50 : int32_t(std::min<int64_t>(50, remaining_ns / 1000000 + 1));
        if (b && b->wait) { b->wait(slice_ms); continue; }
        std::unique_lock<std::mutex> lock(s.mutex);
        s.cv.wait_for(lock, std::chrono::milliseconds(slice_ms), [&] { return !s.queue.empty(); });
    }
}

NUE_API int32_t NUE_CALL nue_peek_events(nue_event* out, int32_t max_count) {
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    int32_t n = 0;
    for (auto it = s.queue.begin(); it != s.queue.end() && n < max_count; ++it, ++n) if (out) out[n] = *it;
    return n;
}

NUE_API nue_result NUE_CALL nue_push_event(const nue_event* event) {
    if (!event) return NUE_ERR_INVALID_ARGUMENT;
    if (!es().active) return nuei::fail(NUE_ERR_NOT_INITIALIZED, "nue_push_event: the engine is not initialised");
    push_internal(*event, true);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_push_user_event(int32_t code, uint64_t data1, uint64_t data2) {
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.type = NUE_EVENT_USER;
    e.payload.user.code = code;
    e.payload.user.data1 = data1;
    e.payload.user.data2 = data2;
    return nue_push_event(&e);
}

NUE_API void NUE_CALL nue_flush_events(uint32_t first_type, uint32_t last_type) {
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.queue.erase(std::remove_if(s.queue.begin(), s.queue.end(), [&](const nue_event& e) { return e.type >= first_type && e.type <= last_type; }), s.queue.end());
}

NUE_API void NUE_CALL nue_set_event_enabled(uint32_t type, nue_bool enabled) {
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (enabled) s.disabled.erase(type); else s.disabled.insert(type);
}

NUE_API nue_result NUE_CALL nue_event_add_watcher(nue_event_watcher fn, void* userdata) {
    if (!fn) return NUE_ERR_INVALID_ARGUMENT;
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.watchers.push_back({fn, userdata});
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_event_remove_watcher(nue_event_watcher fn, void* userdata) {
    EventState& s = es();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (size_t i = 0; i < s.watchers.size(); ++i)
        if (s.watchers[i].fn == fn && s.watchers[i].userdata == userdata) { s.watchers.erase(s.watchers.begin() + long(i)); return NUE_OK; }
    return NUE_ERR_NOT_FOUND;
}

NUE_API void NUE_CALL nue_app_request_quit(void) {
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.type = NUE_EVENT_QUIT;
    if (es().active) push_internal(e, true); else es().quit_requested = true;
}

NUE_API nue_bool NUE_CALL nue_app_quit_requested(void) { return es().quit_requested.load() ? NUE_TRUE : NUE_FALSE; }

NUE_API int32_t NUE_CALL nue_app_run(const nue_init_desc* init, const nue_app_callbacks* cb, int argc, char** argv) {
    if (!cb) return 2;
    nue_init_desc desc;
    NUE_STRUCT_INIT(&desc);
    if (init) {
        size_t n = init->struct_size < sizeof(desc) ? init->struct_size : sizeof(desc);
        std::memcpy(&desc, init, n);
        desc.struct_size = sizeof(desc);
    }
    desc.api_version = init ? desc.api_version : NUE_API_VERSION;
    desc.loop_model = NUE_LOOP_ENGINE;
    nue_result r = nue_init(&desc);
    if (NUE_FAILED(r)) return 3;
    int32_t exit_code = 0;
    nue_result final_result = NUE_OK;
    if (cb->on_init) {
        r = cb->on_init(cb->userdata, argc, argv);
        if (NUE_FAILED(r)) { NUE_LOGE("app", "on_init failed: %s", nue_get_last_error_message()); final_result = r; exit_code = 1; }
    }
    bool running = NUE_SUCCEEDED(final_result);
    while (running) {
        // In the background there is nothing to render: sleep in the event wait instead of spinning.
        nue_event ev;
        bool have = nue_app_get_state() == NUE_APP_STATE_BACKGROUND ? nue_wait_event(&ev, 100) : (nue_pump_events(), nue_poll_event(&ev));
        while (have && running) {
            if (cb->on_event) {
                nue_app_result ar = cb->on_event(cb->userdata, &ev);
                if (ar != NUE_APP_CONTINUE) { running = false; exit_code = ar == NUE_APP_QUIT_FAILURE ? 1 : 0; break; }
            }
            if (ev.type == NUE_EVENT_QUIT && !cb->on_event) running = false;
            have = nue_poll_event(&ev) != 0;
        }
        if (!running) break;
        if (nue_app_get_state() != NUE_APP_STATE_BACKGROUND && cb->on_frame) {
            nue_app_result ar = cb->on_frame(cb->userdata);
            if (ar != NUE_APP_CONTINUE) { running = false; exit_code = ar == NUE_APP_QUIT_FAILURE ? 1 : 0; }
        } else if (!cb->on_frame && nue_app_quit_requested()) {
            running = false;
        }
    }
    if (cb->on_quit) cb->on_quit(cb->userdata, final_result);
    nue_shutdown();
    return exit_code;
}

}  // extern "C"
