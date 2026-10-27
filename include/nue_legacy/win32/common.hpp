// legacy_win32 - Level A: the Win32 subset a translated 32-bit game needs, as plain handle-based C++.
//
// Rules of this library (docs/NUE_HANDOVER_WIN32_DINPUT.md):
//   * no Windows header, no guest pointer, no COM object in any interface: the facade (the code that knows the game) decodes the guest's calling
//     convention and memory and calls these classes with host values;
//   * every service of the host comes through the platform interfaces (platform/*.hpp, shaped like NUE's native API): the library never calls the OS;
//   * Win32 *semantics* live here: handle values, error codes, wait rules, path rules, FILETIME arithmetic, code pages... so they can be tested
//     headless and, where Windows is the host, against the real implementation.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <mutex>
#include <string>
#include <vector>

namespace nue::legacy::win32 {

using Bytes = std::span<const std::byte>;           // data crossing into the library, valid for the call (same as platform::Bytes)
using MutableBytes = std::span<std::byte>;
using Handle = uint32_t;                       // what the guest sees as HANDLE / HKEY / HWND... (0 = NULL)
constexpr Handle kInvalidHandle = 0xFFFFFFFFu; // INVALID_HANDLE_VALUE
constexpr Handle kCurrentProcessPseudoHandle = 0xFFFFFFFFu, kCurrentThreadPseudoHandle = 0xFFFFFFFEu;   // GetCurrentProcess(), GetCurrentThread()

// The Win32 error codes the subset uses (GetLastError values). Zero means success.
namespace err {
enum : uint32_t {
    success = 0, invalid_function = 1, file_not_found = 2, path_not_found = 3, too_many_open_files = 4, access_denied = 5, invalid_handle = 6,
    not_enough_memory = 8, invalid_data = 13, out_of_memory = 14, no_more_files = 18, sharing_violation = 32, handle_eof = 38, not_supported = 50,
    file_exists = 80, invalid_parameter = 87, broken_pipe = 109, insufficient_buffer = 122, invalid_name = 123, call_not_implemented = 120,
    mod_not_found = 126, proc_not_found = 127, dir_not_empty = 145, already_exists = 183, envvar_not_found = 203, more_data = 234,
    wait_timeout = 258, no_more_items = 259, not_owner = 288, too_many_posts = 298, io_pending = 997, invalid_flags = 1004,
    invalid_window_handle = 1400, class_already_exists = 1410, class_does_not_exist = 1411, invalid_thread_id = 1444, no_token = 1008,
    invalid_cursor_handle = 1402, invalid_index = 1413, not_found = 1168, old_win_version = 1150, operation_aborted = 995, no_unicode_translation = 1113,
};
}

// A result with the Win32 error that explains a failure (0 when the call succeeded). The facade stores `error` in the guest's last-error slot
// where the API sets it.
template <class T> struct Ret {
    T value{};
    uint32_t error = 0;
    explicit operator bool() const { return error == 0; }
};
template <class T> inline Ret<T> ok(T v) { return {v, 0}; }
template <class T> inline Ret<T> fail(uint32_t error, T v = T{}) { return {v, error}; }

// Wait results (WaitForSingleObject / WaitForMultipleObjects).
constexpr uint32_t kWaitObject0 = 0, kWaitAbandoned0 = 0x80, kWaitIoCompletion = 0xC0, kWaitTimeout = 0x102, kWaitFailed = 0xFFFFFFFFu;
constexpr uint32_t kInfinite = 0xFFFFFFFFu;

}  // namespace nue::legacy::win32
