#include "event_queue.hpp"

namespace nue::android {

bool EventQueue::coalesces(uint32_t type) {
    switch (type) {
    case NUE_ANDROID_EVENT_THERMAL:
    case NUE_ANDROID_EVENT_GAME_MODE:
    case NUE_ANDROID_EVENT_BATTERY_SAVER:
    case NUE_ANDROID_EVENT_DISPLAY:
    case NUE_ANDROID_EVENT_SAFE_AREA:
    case NUE_ANDROID_EVENT_LAST_INPUT:
    case NUE_ANDROID_EVENT_IMPORT: // progress: only the newest counts (state changes are never lost: see below)
        return true;
    default: return false;
    }
}

void EventQueue::push(const NueAndroidEvent& e) {
    std::lock_guard<std::mutex> lock(m_);
    if (coalesces(e.type)) {
        // IMPORT events carry a state; only replace a queued progress event of the same state, never a state change.
        for (auto it = q_.rbegin(); it != q_.rend(); ++it) {
            if (it->type != e.type) continue;
            if (e.type == NUE_ANDROID_EVENT_IMPORT && it->i[0] != e.i[0]) break;
            *it = e;
            return;
        }
    }
    if (q_.size() >= capacity_) {
        // Drop the oldest coalescable event first; if there is none the new event is lost and counted.
        for (auto it = q_.begin(); it != q_.end(); ++it) {
            if (coalesces(it->type)) {
                q_.erase(it);
                q_.push_back(e);
                ++dropped_;
                return;
            }
        }
        ++dropped_;
        return;
    }
    q_.push_back(e);
}

bool EventQueue::poll(NueAndroidEvent* out) {
    std::lock_guard<std::mutex> lock(m_);
    if (q_.empty()) return false;
    *out = q_.front();
    q_.pop_front();
    return true;
}

uint32_t EventQueue::dropped() const {
    std::lock_guard<std::mutex> lock(m_);
    return dropped_;
}

size_t EventQueue::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return q_.size();
}

void EventQueue::clear() {
    std::lock_guard<std::mutex> lock(m_);
    q_.clear();
}

} // namespace nue::android
