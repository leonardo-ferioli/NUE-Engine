#include "nue_win/common.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace nue::win {

const char* support_name(Support s) {
    switch (s) {
        case Support::Yes: return "yes";
        case Support::No: return "no";
        default: return "unknown";
    }
}

std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string error_message(uint32_t code) {
    wchar_t* buffer = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                             reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (n && buffer) {
        while (n && (buffer[n - 1] == L'\r' || buffer[n - 1] == L'\n' || buffer[n - 1] == L' ' || buffer[n - 1] == L'.')) --n;
        text = to_utf8(std::wstring_view(buffer, n));
    }
    if (buffer) LocalFree(buffer);
    char tail[32];
    std::snprintf(tail, sizeof tail, " (0x%08X)", code);
    return text + tail;
}

namespace {
struct SinkState { LogSink sink = nullptr; void* user = nullptr; };
std::atomic<SinkState*> g_sink{nullptr};
}  // namespace

void set_log_sink(LogSink sink, void* user) {
    // The previous state is leaked on purpose (a few bytes): a logging thread may still be reading it.
    g_sink.store(sink ? new SinkState{sink, user} : nullptr);
}

void log(LogLevel level, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    if (SinkState* s = g_sink.load()) {
        s->sink(s->user, level, buffer);
        return;
    }
    static const char* const names[] = {"debug", "info", "warn", "error"};
    char line[1100];
    std::snprintf(line, sizeof line, "[nue-win %s] %s\n", names[unsigned(level) & 3], buffer);
    OutputDebugStringA(line);
}

OsVersion os_version() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const OsVersion v = [] {
        OsVersion r;
        if (auto fn = load_proc<Fn>(L"ntdll.dll", "RtlGetVersion")) {
            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof info;
            if (fn(&info) == 0) { r.major = info.dwMajorVersion; r.minor = info.dwMinorVersion; r.build = info.dwBuildNumber; }
        }
        return r;
    }();
    return v;
}

bool reg_read_dword(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD& out) {
    DWORD type = 0, size = sizeof(DWORD), data = 0;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, &type, &data, &size) != ERROR_SUCCESS) return false;
    out = data;
    return true;
}

bool reg_read_string(HKEY root, const wchar_t* subkey, const wchar_t* value, std::wstring& out) {
    DWORD size = 0;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &size) != ERROR_SUCCESS) return false;
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer.data(), &size) != ERROR_SUCCESS) return false;
    buffer.resize(wcslen(buffer.c_str()));
    out = std::move(buffer);
    return true;
}

}  // namespace nue::win
