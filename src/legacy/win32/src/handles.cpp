#include "nue_legacy/win32/handles.hpp"

namespace nue::legacy::win32 {

Handle HandleTable::add(std::shared_ptr<Object> object) {
    std::lock_guard lock(m_);
    uint32_t index;
    if (!free_.empty()) { index = free_.back(); free_.pop_back(); }
    else {
        if (slots_.size() >= kMaxSlots) return 0;
        index = uint32_t(slots_.size());
        slots_.emplace_back();
    }
    object->handle_count.fetch_add(1);
    slots_[index].object = std::move(object);
    return encode(index, slots_[index].generation);
}

bool HandleTable::decode(Handle h, uint32_t& index) const {
    if (!owns(h)) return false;
    h -= bias_;
    if (h == 0 || (h & 3) != 0 || h >= (8u << 15)) return false;
    index = (h >> 2) & 0x1FFFu;
    const uint32_t generation = (h >> 15) & 7u;
    if (index == 0 || index >= slots_.size() || !slots_[index].object) return false;
    return (slots_[index].generation & 7u) == generation;
}

std::shared_ptr<Object> HandleTable::get(Handle h) const {
    std::lock_guard lock(m_);
    uint32_t index;
    return decode(h, index) ? slots_[index].object : nullptr;
}

bool HandleTable::close(Handle h) {
    std::shared_ptr<Object> released;
    bool last = false;
    {
        std::lock_guard lock(m_);
        uint32_t index;
        if (!decode(h, index)) return false;
        released = std::move(slots_[index].object);
        slots_[index].object = nullptr;
        slots_[index].generation = (slots_[index].generation % 7u) + 1u;   // generations 1..7: 0 is never used
        free_.push_back(index);
        last = released->handle_count.fetch_sub(1) == 1;
    }
    if (last) released->on_last_handle_closed();
    return true;
}

Handle HandleTable::duplicate(Handle h) {
    std::shared_ptr<Object> o = get(h);
    return o ? add(o) : 0;
}

size_t HandleTable::live() const {
    std::lock_guard lock(m_);
    return slots_.size() - 1 - free_.size();
}

}  // namespace nue::legacy::win32
