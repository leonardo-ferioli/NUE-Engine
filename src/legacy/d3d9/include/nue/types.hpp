// Platform API: common types. No Windows type, no guest pointer, no 32-bit assumption (docs/PLATFORM_API.md, section 1).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nue {

// Opaque handle owned by the platform; 0 means "none". The tag makes handles of different kinds distinct types.
template <class Tag> struct Id {
    uint32_t value = 0;
    constexpr explicit operator bool() const { return value != 0; }
    friend constexpr bool operator==(Id, Id) = default;
};

enum class Status : int32_t {
    ok, invalid_argument, not_found, already_exists, unsupported, out_of_memory, io_error, would_block, timed_out, device_lost, closed
};

template <class T> struct Result {
    Status status = Status::ok;
    T value{};
    explicit operator bool() const { return status == Status::ok; }
};

// Data crosses the platform boundary as a span of host memory, valid for the duration of the call only.
using Bytes = std::span<const std::byte>;
using MutableBytes = std::span<std::byte>;

}  // namespace nue
