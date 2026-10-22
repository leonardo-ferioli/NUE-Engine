#include "nue_win/gamemode.hpp"

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <sstream>

#pragma comment(lib, "shell32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace nue::win {

const char* notification_state_name(int q) {
    switch (q) {
        case QUNS_NOT_PRESENT: return "not present";
        case QUNS_BUSY: return "busy (full-screen app)";
        case QUNS_RUNNING_D3D_FULL_SCREEN: return "running D3D full screen";
        case QUNS_PRESENTATION_MODE: return "presentation mode";
        case QUNS_ACCEPTS_NOTIFICATIONS: return "accepts notifications";
        case QUNS_QUIET_TIME: return "quiet time";
        case QUNS_APP: return "app";
        default: return "unknown";
    }
}

namespace {
Support dword_flag(HKEY root, const wchar_t* key, const wchar_t* value, DWORD on_value = 1) {
    DWORD v = 0;
    if (!reg_read_dword(root, key, value, v)) return Support::Unknown;
    return v == on_value ? Support::Yes : Support::No;
}

bool process_running(std::initializer_list<const wchar_t*> names) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        for (const wchar_t* n : names) if (_wcsicmp(e.szExeFile, n) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}
}  // namespace

GameModeInfo query_game_mode_info() {
    GameModeInfo g;
    const wchar_t* bar = L"Software\\Microsoft\\GameBar";
    g.game_mode_allowed = dword_flag(HKEY_CURRENT_USER, bar, L"AutoGameModeEnabled");
    if (g.game_mode_allowed == Support::Unknown) g.game_mode_allowed = dword_flag(HKEY_CURRENT_USER, bar, L"AllowAutoGameMode");
    g.game_bar_enabled = dword_flag(HKEY_CURRENT_USER, bar, L"UseNexusForGameBarEnabled");
    g.background_capture_enabled = dword_flag(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled");
    DWORD allow = 1;
    if (reg_read_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR", allow)) g.capture_forbidden_by_policy = allow == 0 ? Support::Yes : Support::No;
    g.power_mode = query_effective_power_mode();
    g.game_mode_active = g.power_mode == EffectivePowerMode::GameMode;
    QUERY_USER_NOTIFICATION_STATE q{};
    if (SUCCEEDED(SHQueryUserNotificationState(&q))) {
        g.notification_state = int(q);
        g.windows_treats_us_as_fullscreen_game = q == QUNS_RUNNING_D3D_FULL_SCREEN;
    }
    g.game_bar_running = process_running({L"GameBar.exe", L"GameBarFTServer.exe", L"GameBarPresenceWriter.exe", L"XboxGameBarWidgets.exe"});
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]));
    if (n && n < sizeof exe / sizeof exe[0]) g.fullscreen_optimizations_disabled = query_fullscreen_optimizations_disabled(exe);
    return g;
}

Status window_exclude_from_capture(HWND hwnd, bool exclude) {
    DWORD wanted = exclude ? (at_least_windows10(19041) ? WDA_EXCLUDEFROMCAPTURE : WDA_MONITOR) : WDA_NONE;
    if (!SetWindowDisplayAffinity(hwnd, wanted)) return Status::last_error("SetWindowDisplayAffinity");
    return Status::success();
}

bool window_is_excluded_from_capture(HWND hwnd) {
    DWORD a = 0;
    return GetWindowDisplayAffinity(hwnd, &a) && a != WDA_NONE;
}

bool layers_contain(const std::wstring& layers, const wchar_t* flag) {
    std::wistringstream in(layers);
    std::wstring tok;
    while (in >> tok) if (_wcsicmp(tok.c_str(), flag) == 0) return true;
    return false;
}

std::wstring layers_with_flag(const std::wstring& layers, const wchar_t* flag, bool present) {
    std::wistringstream in(layers);
    std::wstring tok;
    std::vector<std::wstring> keep;
    while (in >> tok) if (_wcsicmp(tok.c_str(), flag) != 0 && tok != L"~") keep.push_back(tok);
    if (present) keep.push_back(flag);
    if (keep.empty()) return {};
    std::wstring out = L"~";   // "~" marks the layer string as a per-user-applied list
    for (const auto& t : keep) out += L" " + t;
    return out;
}

Support query_fullscreen_optimizations_disabled(const std::wstring& exe, const CompatLayerStore& store) {
    std::wstring layers;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
        if (reg_read_string(root, store.subkey, exe.c_str(), layers) && layers_contain(layers, L"DISABLEDXMAXIMIZEDWINDOWEDMODE")) return Support::Yes;
    return Support::No;
}

Status set_fullscreen_optimizations_disabled(const std::wstring& exe, bool disabled, const CompatLayerStore& store) {
    std::wstring layers;
    reg_read_string(HKEY_CURRENT_USER, store.subkey, exe.c_str(), layers);
    const std::wstring next = layers_with_flag(layers, L"DISABLEDXMAXIMIZEDWINDOWEDMODE", disabled);
    if (next.empty()) {
        LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, store.subkey, exe.c_str());
        return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) ? Status::success() : Status::fail("RegDeleteKeyValue(AppCompat layers)", uint32_t(r));
    }
    LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, store.subkey, exe.c_str(), REG_SZ, next.c_str(), DWORD((next.size() + 1) * sizeof(wchar_t)));
    return r == ERROR_SUCCESS ? Status::success() : Status::fail("RegSetKeyValue(AppCompat layers)", uint32_t(r));
}

}  // namespace nue::win
