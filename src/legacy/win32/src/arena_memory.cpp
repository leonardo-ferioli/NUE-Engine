#include "nue_legacy/win32/arena_memory.hpp"
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <stdexcept>

namespace nue::legacy::win32 {

ArenaMemory::ArenaMemory(uint32_t total_bytes, uint32_t window_bytes) : total_(total_bytes), window_bytes_(window_bytes) {
    window_bytes_ = (window_bytes_ + 0xFFFFu) & ~0xFFFFu;
    zone_base_ = kWindowBase + window_bytes_;
    if (zone_base_ >= total_ || total_ - zone_base_ < (1u << 20)) throw std::runtime_error("ArenaMemory: the allocation zone does not fit");
    block_ = static_cast<uint8_t*>(std::calloc(total_, 1));
    if (!block_) throw std::bad_alloc();
    free_[zone_base_] = total_ - zone_base_;
}
ArenaMemory::~ArenaMemory() { std::free(block_); }

uint32_t ArenaMemory::alloc(uint32_t bytes) {
    std::lock_guard lock(m_);
    const uint32_t size = (std::max<uint32_t>(bytes, 1) + 15u) & ~15u;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < size) continue;
        const uint32_t address = it->first, left = it->second - size;
        free_.erase(it);
        if (left) free_[address + size] = left;
        live_[address] = size;
        std::memset(block_ + address, 0, size);
        return address;
    }
    return 0;
}

void ArenaMemory::free(uint32_t address) {
    std::lock_guard lock(m_);
    auto it = live_.find(address);
    if (it == live_.end()) return;
    uint32_t size = it->second, start = address;
    live_.erase(it);
    auto next = free_.lower_bound(address);
    if (next != free_.end() && next->first == address + size) { size += next->second; next = free_.erase(next); }
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) { start = prev->first; size += prev->second; free_.erase(prev); }
    }
    free_[start] = size;
}

uint8_t* ArenaMemory::host(uint32_t address, uint32_t bytes) {
    if (address < kWindowBase || uint64_t(address) + bytes > total_) return nullptr;
    return block_ + address;
}

uint32_t ArenaMemory::address_of(const void* host_pointer) {
    const auto* p = static_cast<const uint8_t*>(host_pointer);
    if (p < block_ + kWindowBase || p >= block_ + total_) return 0;
    return uint32_t(p - block_);
}

}  // namespace nue::legacy::win32
