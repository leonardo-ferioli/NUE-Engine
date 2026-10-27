// Common value types of the legacy layers that need no Windows type and no guest pointer: handles, a status code, a result, spans of host memory.
// Shared by legacy_win32 and legacy_dinput (and available to the other legacy layers). `nue_result` is the native API's result type (nue/base.h).
#pragma once
#include "nue/base.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nue::legacy {

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

// Data crosses the interface as a span of host memory, valid for the duration of the call only.
using Bytes = std::span<const std::byte>;
using MutableBytes = std::span<std::byte>;

// The one place where the native API's result codes and Status meet.
inline Status status_from(nue_result r) {
    if (r >= 0) return Status::ok;
    switch (r) {
    case NUE_ERR_INVALID_ARGUMENT: case NUE_ERR_INVALID_HANDLE: case NUE_ERR_BUFFER_TOO_SMALL: return Status::invalid_argument;
    case NUE_ERR_NOT_FOUND: return Status::not_found;
    case NUE_ERR_ALREADY_EXISTS: return Status::already_exists;
    case NUE_ERR_UNSUPPORTED: return Status::unsupported;
    case NUE_ERR_OUT_OF_MEMORY: return Status::out_of_memory;
    case NUE_ERR_TIMEOUT: return Status::timed_out;
    case NUE_ERR_WOULD_BLOCK: case NUE_ERR_BUSY: return Status::would_block;
    case NUE_ERR_DEVICE_LOST: return Status::device_lost;
    case NUE_ERR_END_OF_DATA: return Status::closed;
    default: return Status::io_error;
    }
}
inline nue_result result_from(Status s) {
    switch (s) {
    case Status::ok: return NUE_OK;
    case Status::invalid_argument: return NUE_ERR_INVALID_ARGUMENT;
    case Status::not_found: return NUE_ERR_NOT_FOUND;
    case Status::already_exists: return NUE_ERR_ALREADY_EXISTS;
    case Status::unsupported: return NUE_ERR_UNSUPPORTED;
    case Status::out_of_memory: return NUE_ERR_OUT_OF_MEMORY;
    case Status::would_block: return NUE_ERR_WOULD_BLOCK;
    case Status::timed_out: return NUE_ERR_TIMEOUT;
    case Status::device_lost: return NUE_ERR_DEVICE_LOST;
    case Status::closed: return NUE_ERR_END_OF_DATA;
    default: return NUE_ERR_IO;
    }
}

}  // namespace nue::legacy
