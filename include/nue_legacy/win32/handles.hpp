// Handle table: every kernel object the guest holds (files, events, mutexes, threads, find handles, mappings...) is a 32-bit value that this
// table maps to a reference-counted object. The value encodes a slot and a generation, so a closed handle that is used again fails with
// ERROR_INVALID_HANDLE instead of reaching a later object (docs/ABI.md 2.3 of NUE). Values are multiples of 4 and below 0x40000, so old code that
// keeps them in 16 or 18 bits or compares them with small constants keeps working.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include <atomic>
#include <vector>

namespace nue::legacy::win32 {

enum class ObjectKind : uint8_t { file, find, mapping, event, mutex, semaphore, thread, process, registry_key, other };

class Object {
public:
    explicit Object(ObjectKind k) : kind(k) {}
    virtual ~Object() = default;
    const ObjectKind kind;
    // Called when the last handle to the object is closed (the object itself lives on while others hold it).
    virtual void on_last_handle_closed() {}
    std::atomic<uint32_t> handle_count{0};
};

class HandleTable {
public:
    // `bias` is added to every value the table hands out. A host that still has other handles (the real operating system's) gives the table a bias so
    // that the two ranges cannot meet: `owns` then says whose a value is.
    explicit HandleTable(uint32_t bias = 0) : bias_(bias) { slots_.resize(1); }
    bool owns(Handle h) const { return h >= bias_ && h - bias_ < 0x40000u; }
    // Returns 0 when the table is full.
    Handle add(std::shared_ptr<Object> object);
    std::shared_ptr<Object> get(Handle h) const;
    template <class T> std::shared_ptr<T> get_as(Handle h, ObjectKind kind) const {
        auto o = get(h);
        return o && o->kind == kind ? std::static_pointer_cast<T>(o) : nullptr;
    }
    // Closes one handle. false when the value is not a live handle. The generation moves on, so the value is not valid again.
    bool close(Handle h);
    // A second handle to the same object (DuplicateHandle).
    Handle duplicate(Handle h);
    size_t live() const;

    static constexpr uint32_t kMaxSlots = 0x2000;

private:
    struct Slot { std::shared_ptr<Object> object; uint32_t generation = 1; };
    Handle encode(uint32_t index, uint32_t generation) const { return bias_ + (((generation & 7u) << 13 | index) << 2); }
    bool decode(Handle h, uint32_t& index) const;

    uint32_t bias_ = 0;
    mutable std::mutex m_;
    std::vector<Slot> slots_;                 // slot 0 is never used: handle 0 is NULL
    std::vector<uint32_t> free_;
};

}  // namespace nue::legacy::win32
