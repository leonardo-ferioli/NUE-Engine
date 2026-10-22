#include "nue_win/display.hpp"

#include <shellscalingapi.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shcore.lib")

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace nue::win {

FitRect fit_aspect(uint32_t cw, uint32_t ch, int32_t ax, int32_t ay, uint32_t aw, uint32_t ah) {
    FitRect r;
    if (!cw || !ch || !aw || !ah) return r;
    const uint64_t lhs = uint64_t(cw) * ah, rhs = uint64_t(aw) * ch;   // compare cw/ch with aw/ah without floating point
    if (lhs == rhs) { r.width = aw; r.height = ah; }
    else if (lhs > rhs) { r.width = aw; r.height = std::max<uint32_t>(1, uint32_t(uint64_t(aw) * ch / cw)); }
    else { r.height = ah; r.width = std::max<uint32_t>(1, uint32_t(uint64_t(ah) * cw / ch)); }
    r.x = ax + int32_t((aw - r.width) / 2);
    r.y = ay + int32_t((ah - r.height) / 2);
    return r;
}

// ---- DPI ----------------------------------------------------------------------------------------------------------------------

const char* dpi_awareness_name(DpiAwareness a) {
    switch (a) {
        case DpiAwareness::Unaware: return "unaware";
        case DpiAwareness::System: return "system";
        case DpiAwareness::PerMonitor: return "per-monitor";
        case DpiAwareness::PerMonitorV2: return "per-monitor v2";
        default: return "unknown";
    }
}

DpiAwareness query_process_dpi_awareness() {
    using GetThreadCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(void);
    using Equal = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    auto get = load_proc<GetThreadCtx>(L"user32.dll", "GetThreadDpiAwarenessContext");
    auto eq = load_proc<Equal>(L"user32.dll", "AreDpiAwarenessContextsEqual");
    if (!get || !eq) return DpiAwareness::Unknown;
    DPI_AWARENESS_CONTEXT c = get();
    if (eq(c, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return DpiAwareness::PerMonitorV2;
    if (eq(c, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return DpiAwareness::PerMonitor;
    if (eq(c, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)) return DpiAwareness::System;
    if (eq(c, DPI_AWARENESS_CONTEXT_UNAWARE) || eq(c, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED)) return DpiAwareness::Unaware;
    return DpiAwareness::Unknown;
}

DpiAwareness enable_best_dpi_awareness(Status* detail) {
    using SetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    Status last = Status::success();
    if (auto set = load_proc<SetCtx>(L"user32.dll", "SetProcessDpiAwarenessContext")) {
        if (set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) { if (detail) *detail = last; return DpiAwareness::PerMonitorV2; }
        last = Status::last_error("SetProcessDpiAwarenessContext(V2)");   // ERROR_ACCESS_DENIED: already set (manifest or earlier call)
        if (set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) { if (detail) *detail = last; return DpiAwareness::PerMonitor; }
    }
    if (SUCCEEDED(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) { if (detail) *detail = last; return DpiAwareness::PerMonitor; }
    if (detail) *detail = last;
    return query_process_dpi_awareness();   // whatever is in effect (a manifest may already have decided)
}

uint32_t window_dpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    if (auto fn = load_proc<Fn>(L"user32.dll", "GetDpiForWindow")) { UINT d = fn(hwnd); if (d) return d; }
    return 96;
}

uint32_t monitor_dpi(HMONITOR m) {
    UINT x = 96, y = 96;
    if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y)) && x) return x;
    return 96;
}

// ---- Monitors -----------------------------------------------------------------------------------------------------------------

namespace {

struct ConfigEntry {
    std::string friendly;
    uint32_t num = 0, den = 0;
    Support hdr_supported = Support::Unknown, hdr_enabled = Support::Unknown, wide = Support::Unknown;
    uint32_t bits = 0;
    float sdr_white = 0;
};

std::map<std::wstring, ConfigEntry> query_display_config() {
    std::map<std::wstring, ConfigEntry> result;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) return result;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return result;
    for (UINT32 i = 0; i < np; ++i) {
        const auto& p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof src, p.sourceInfo.adapterId, p.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        ConfigEntry e;
        e.num = p.targetInfo.refreshRate.Numerator;
        e.den = p.targetInfo.refreshRate.Denominator;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof tgt, p.targetInfo.adapterId, p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) e.friendly = to_utf8(tgt.monitorFriendlyDeviceName);
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac{};
        ac.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof ac, p.targetInfo.adapterId, p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&ac.header) == ERROR_SUCCESS) {
            e.hdr_supported = ac.advancedColorSupported ? Support::Yes : Support::No;
            e.hdr_enabled = ac.advancedColorEnabled ? Support::Yes : Support::No;
            e.wide = ac.wideColorEnforced ? Support::Yes : Support::No;
            e.bits = ac.bitsPerColorChannel;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof white, p.targetInfo.adapterId, p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) e.sdr_white = white.SDRWhiteLevel * 80.0f / 1000.0f;   // 1000 = 80 nits
        result[src.viewGdiDeviceName] = std::move(e);
    }
    return result;
}

DisplayMode mode_from_devmode(const DEVMODEW& d) {
    DisplayMode m;
    m.width = d.dmPelsWidth;
    m.height = d.dmPelsHeight;
    m.bits_per_pixel = d.dmBitsPerPel;
    m.refresh_hz = d.dmDisplayFrequency;
    m.interlaced = (d.dmDisplayFlags & DM_INTERLACED) != 0;
    return m;
}

bool fill_monitor(HMONITOR h, const std::map<std::wstring, ConfigEntry>& config, MonitorInfo& out) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(h, &mi)) return false;
    out = MonitorInfo{};
    out.handle = h;
    out.device_name = mi.szDevice;
    out.monitor_rect = mi.rcMonitor;
    out.work_rect = mi.rcWork;
    out.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    out.dpi = monitor_dpi(h);
    out.scale = out.dpi / 96.0;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0)) out.current = mode_from_devmode(dm);
    auto it = config.find(out.device_name);
    if (it != config.end()) {
        const ConfigEntry& e = it->second;
        out.friendly_name = e.friendly;
        out.refresh_numerator = e.num;
        out.refresh_denominator = e.den;
        out.hdr_supported = e.hdr_supported;
        out.hdr_enabled = e.hdr_enabled;
        out.wide_color_enforced = e.wide;
        out.bits_per_color = e.bits;
        out.sdr_white_level_nits = e.sdr_white;
    }
    return true;
}

BOOL CALLBACK collect_monitor(HMONITOR h, HDC, LPRECT, LPARAM param) {
    static_cast<std::vector<HMONITOR>*>(reinterpret_cast<void*>(param))->push_back(h);
    return TRUE;
}

}  // namespace

bool enumerate_monitors(std::vector<MonitorInfo>& out) {
    out.clear();
    std::vector<HMONITOR> handles;
    EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&handles));
    const auto config = query_display_config();
    for (HMONITOR h : handles) {
        MonitorInfo m;
        if (fill_monitor(h, config, m)) out.push_back(std::move(m));
    }
    std::stable_sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) { return a.primary && !b.primary; });
    return !out.empty();
}

bool monitor_info_from_handle(HMONITOR h, MonitorInfo& out) { return fill_monitor(h, query_display_config(), out); }

bool monitor_of_window(HWND hwnd, MonitorInfo& out) {
    HMONITOR h = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return h && monitor_info_from_handle(h, out);
}

RECT virtual_screen_rect() {
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

// ---- Modes ----------------------------------------------------------------------------------------------------------------------

std::vector<DisplayMode> enumerate_display_modes(const std::wstring& device, bool include_raw) {
    std::vector<DisplayMode> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    const DWORD flags = include_raw ? EDS_RAWMODE : 0;
    for (DWORD i = 0; EnumDisplaySettingsExW(device.empty() ? nullptr : device.c_str(), i, &dm, flags); ++i) {
        DisplayMode m = mode_from_devmode(dm);
        if (m.width && m.height && std::find(modes.begin(), modes.end(), m) == modes.end()) modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end(), [](const DisplayMode& a, const DisplayMode& b) {
        if (a.width != b.width) return a.width < b.width;
        if (a.height != b.height) return a.height < b.height;
        if (a.bits_per_pixel != b.bits_per_pixel) return a.bits_per_pixel < b.bits_per_pixel;
        return a.refresh_hz < b.refresh_hz;
    });
    return modes;
}

bool find_display_mode(const std::vector<DisplayMode>& modes, uint32_t w, uint32_t h, uint32_t hz, DisplayMode& out) {
    const DisplayMode* best = nullptr;
    for (const auto& m : modes) {
        if (m.width != w || m.height != h || m.interlaced) continue;
        if (!best) { best = &m; continue; }
        const bool m_ok = m.refresh_hz <= hz, b_ok = best->refresh_hz <= hz;
        if (m_ok != b_ok) { if (m_ok) best = &m; continue; }
        if (m_ok ? m.refresh_hz > best->refresh_hz : m.refresh_hz < best->refresh_hz) best = &m;
        else if (m.refresh_hz == best->refresh_hz && m.bits_per_pixel > best->bits_per_pixel) best = &m;
    }
    if (!best) return false;
    out = *best;
    return true;
}

namespace {
DEVMODEW devmode_for(const DisplayMode& m) {
    DEVMODEW d{};
    d.dmSize = sizeof d;
    d.dmPelsWidth = m.width;
    d.dmPelsHeight = m.height;
    d.dmBitsPerPel = m.bits_per_pixel;
    d.dmDisplayFrequency = m.refresh_hz;
    d.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | (m.refresh_hz ? DM_DISPLAYFREQUENCY : 0);
    return d;
}
Status change_result(LONG r, const char* what) {
    if (r == DISP_CHANGE_SUCCESSFUL) return Status::success();
    return Status::fail(what, uint32_t(r));   // DISP_CHANGE_* codes
}
}  // namespace

Status test_display_mode(const std::wstring& device, const DisplayMode& m) {
    DEVMODEW d = devmode_for(m);
    return change_result(ChangeDisplaySettingsExW(device.c_str(), &d, nullptr, CDS_TEST, nullptr), "ChangeDisplaySettingsEx(CDS_TEST)");
}

namespace {
std::mutex g_changed_mutex;
std::set<std::wstring> g_changed_devices;
}  // namespace

Status set_display_mode(const std::wstring& device, const DisplayMode& m) {
    DEVMODEW d = devmode_for(m);
    Status s = change_result(ChangeDisplaySettingsExW(device.c_str(), &d, nullptr, CDS_FULLSCREEN, nullptr), "ChangeDisplaySettingsEx(CDS_FULLSCREEN)");
    if (s.ok) { std::lock_guard<std::mutex> g(g_changed_mutex); g_changed_devices.insert(device); }
    return s;
}

Status restore_display_mode(const std::wstring& device) {
    Status s = change_result(ChangeDisplaySettingsExW(device.c_str(), nullptr, nullptr, 0, nullptr), "ChangeDisplaySettingsEx(restore)");
    if (s.ok) { std::lock_guard<std::mutex> g(g_changed_mutex); g_changed_devices.erase(device); }
    return s;
}

void restore_all_changed_display_modes() {
    // try_lock: this runs from crash handlers, where the lock may be held by the thread that crashed; then the display mode is left to Windows,
    // which restores CDS_FULLSCREEN modes itself when the process ends.
    std::unique_lock<std::mutex> g(g_changed_mutex, std::try_to_lock);
    if (!g.owns_lock()) return;
    std::set<std::wstring> devices;
    devices.swap(g_changed_devices);
    g.unlock();
    for (const auto& d : devices) ChangeDisplaySettingsExW(d.c_str(), nullptr, nullptr, 0, nullptr);
}

}  // namespace nue::win
