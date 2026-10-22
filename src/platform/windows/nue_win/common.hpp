// NUE Windows backend: shared helpers (status type, logging sink, UTF-8/UTF-16, run-time loaded Win32 entry points).
// Everything in this backend lives in namespace nue::win and has no dependency on SDL3 or on the core API headers, so it can be
// unit-tested alone and adapted to the core API by a thin glue layer. Builds as x86 and x64 with MSVC and clang.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace nue::win {

// Outcome of an operation that talks to the OS. `ok` false carries the Win32 error / HRESULT and a static description of the step.
struct Status {
    bool ok = true;
    uint32_t code = 0;            // GetLastError() or HRESULT bits
    const char* what = "";        // static string: the failing step
    explicit operator bool() const { return ok; }
    static Status success() { return {}; }
    static Status fail(const char* step, uint32_t code_) { Status s; s.ok = false; s.code = code_; s.what = step; return s; }
    static Status last_error(const char* step) { return fail(step, GetLastError()); }
    static Status hresult(const char* step, HRESULT hr) { return fail(step, uint32_t(hr)); }
};

// Tri-state used by capability probes: a feature can be absent on this Windows, present, or not determinable.
enum class Support : uint8_t { Unknown, No, Yes };
const char* support_name(Support s);

std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

// Text of a Win32 error or HRESULT (system message, UTF-8, without the trailing newline).
std::string error_message(uint32_t code);

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };
using LogSink = void (*)(void* user, LogLevel level, const char* message);
// The core layer installs its own logger here; the default sink writes to OutputDebugStringA.
void set_log_sink(LogSink sink, void* user);
void log(LogLevel level, const char* format, ...);

// Run-time linking: GetProcAddress with a typed result; the module stays loaded for the process lifetime.
template <class F>
inline F load_proc(const wchar_t* module, const char* name) {
    HMODULE m = GetModuleHandleW(module);
    if (!m) m = LoadLibraryExW(module, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

// Windows version as reported by RtlGetVersion (GetVersionEx lies without a manifest).
struct OsVersion { uint32_t major = 0, minor = 0, build = 0; };
OsVersion os_version();
inline bool at_least_windows10(uint32_t build = 0) { OsVersion v = os_version(); return v.major > 10 || (v.major == 10 && v.build >= build); }
inline bool is_windows11() { return at_least_windows10(22000); }

// Registry value helpers (HKCU / HKLM, 64-bit view). Return false when the key or value is missing or of another type.
bool reg_read_dword(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD& out);
bool reg_read_string(HKEY root, const wchar_t* subkey, const wchar_t* value, std::wstring& out);

}  // namespace nue::win
