// Bounded thread-safe event queue between the Android main/UI threads (producers) and the engine thread.
// State-like events (display, thermal, safe area ...) coalesce: a newer event replaces an older queued one of
// the same type so a slow consumer never sees stale values and the queue cannot overflow because of them.
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>

#include "nue_android.h"

namespace nue::android {

class EventQueue {
public:
    explicit EventQueue(size_t capacity = 256) : capacity_(capacity) {}
    void push(const NueAndroidEvent& e);
    bool poll(NueAndroidEvent* out);
    uint32_t dropped() const;
    size_t size() const;
    void clear();

    static bool coalesces(uint32_t type);

private:
    mutable std::mutex m_;
    std::deque<NueAndroidEvent> q_;
    size_t capacity_;
    uint32_t dropped_ = 0;
};

inline NueAndroidEvent make_event(uint32_t type, int32_t i0 = 0, int32_t i1 = 0, int32_t i2 = 0, int32_t i3 = 0) {
    NueAndroidEvent e{};
    e.type = type;
    e.i[0] = i0;
    e.i[1] = i1;
    e.i[2] = i2;
    e.i[3] = i3;
    return e;
}

} // namespace nue::android
