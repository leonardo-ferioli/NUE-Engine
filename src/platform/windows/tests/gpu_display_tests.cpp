#include <algorithm>
#include <cstdlib>

#include "nue_win/display.hpp"
#include "nue_win/gamemode.hpp"
#include "nue_win/gpu.hpp"
#include "test_framework.hpp"
#include "test_window.hpp"

using namespace nue::win;

static void test_gpu_enumeration() {
    TEST_BEGIN("gpu enumeration");
    GpuEnumeration e;
    CHECK_OK(enumerate_gpus(e, true, false));
    CHECK(!e.adapters.empty());
    INFO("factory6 %d, current %d, tearing %d, hybrid %d, high-perf idx %d, power-saving idx %d", int(e.factory6), int(e.factory_is_current), int(e.tearing_supported), int(e.hybrid),
         e.high_performance_index, e.power_saving_index);
    for (const auto& a : e.adapters) {
        INFO("#%u %s [%s %04X:%04X] vram %.0f MiB, shared %.0f MiB, driver %s, %s%s%s, outputs %zu, budget %.0f MiB / used %.0f MiB", a.index, a.name.c_str(), vendor_name(a.vendor_id), a.vendor_id,
             a.device_id, a.dedicated_video_memory / 1048576.0, a.shared_system_memory / 1048576.0, a.driver_version.c_str(), a.software ? "software" : "", a.integrated ? "integrated" : "",
             a.discrete ? "discrete" : "", a.outputs.size(), a.memory.budget / 1048576.0, a.memory.current_usage / 1048576.0);
        CHECK(!a.name.empty());
        CHECK(!(a.integrated && a.discrete));
        CHECK(!(a.software && (a.integrated || a.discrete)));
    }
    if (e.high_performance_index >= 0) CHECK(!e.adapters[size_t(e.high_performance_index)].software);
    if (std::getenv("NUE_TEST_GPU_PROBES")) {   // creates D3D devices (wakes a sleeping discrete GPU), so opt-in
        GpuEnumeration p;
        CHECK_OK(enumerate_gpus(p, false, true));
        for (const auto& a : p.adapters) INFO("probe %s: d3d11 FL11_0 %d, d3d12 FL11_0 %d, UMA/integrated %d", a.name.c_str(), int(a.d3d11_level_11_0), int(a.d3d12_level_11_0), int(a.integrated));
    }
    const auto g = query_graphics_system_settings();
    INFO("HAGS %s, windowed-game optimizations %s, windowed VRR %s, TdrDelay %us, TDR disabled %d", support_name(g.hardware_scheduling), support_name(g.game_mode_optimizations_windowed),
         support_name(g.variable_refresh_rate_windowed), g.tdr_delay_seconds, int(g.tdr_disabled));
}

static void test_integrated_heuristic() {
    TEST_BEGIN("integrated / discrete heuristic");
    const uint64_t MiB = 1048576ull, GiB = 1024 * MiB;
    CHECK(!looks_integrated(0x10DE, "NVIDIA GeForce RTX 4060 Laptop GPU", 8 * GiB));
    CHECK(!looks_integrated(0x10DE, "NVIDIA GeForce GT 1030", 2 * GiB));
    CHECK(looks_integrated(0x8086, "Intel(R) UHD Graphics 630", 128 * MiB));
    CHECK(looks_integrated(0x8086, "Intel(R) Arc(TM) Graphics", 128 * MiB));         // Meteor Lake iGPU
    CHECK(!looks_integrated(0x8086, "Intel(R) Arc(TM) A770 Graphics", 16 * GiB));
    CHECK(looks_integrated(0x1002, "AMD Radeon(TM) Graphics", 512 * MiB));           // Ryzen iGPU
    CHECK(looks_integrated(0x1002, "AMD Radeon(TM) 780M Graphics", 512 * MiB));
    CHECK(looks_integrated(0x1002, "AMD Radeon 890M", 2 * GiB));
    CHECK(looks_integrated(0x1002, "AMD Radeon(TM) Vega 8 Graphics", 2 * GiB));
    CHECK(!looks_integrated(0x1002, "AMD Radeon RX 6600", 8 * GiB));
    CHECK(!looks_integrated(0x1002, "AMD Radeon RX 580 Series", 4 * GiB));
    CHECK(!looks_integrated(0x1002, "AMD Radeon Pro W6800", 32 * GiB));
}

static void test_app_gpu_preference() {
    TEST_BEGIN("per-app GPU preference (test registry key)");
    const wchar_t* key = L"Software\\NUE\\Tests\\UserGpuPreferences";
    AppGpuPreferenceStore store{key};
    const std::wstring exe = L"C:\\Games\\NUE Test\\game.exe";
    AppGpuPreference p;
    CHECK_OK(get_app_gpu_preference(exe, p, store));
    CHECK(p == AppGpuPreference::Unspecified);
    CHECK_OK(set_app_gpu_preference(exe, AppGpuPreference::HighPerformance, store));
    CHECK_OK(get_app_gpu_preference(exe, p, store));
    CHECK(p == AppGpuPreference::HighPerformance);
    std::wstring raw;
    CHECK(reg_read_string(HKEY_CURRENT_USER, key, exe.c_str(), raw) && raw == L"GpuPreference=2;");   // exact format Windows Settings writes
    CHECK_OK(set_app_gpu_preference(exe, AppGpuPreference::PowerSaving, store));
    CHECK_OK(get_app_gpu_preference(exe, p, store));
    CHECK(p == AppGpuPreference::PowerSaving);
    CHECK_OK(set_app_gpu_preference(exe, AppGpuPreference::Unspecified, store));
    CHECK_OK(get_app_gpu_preference(exe, p, store));
    CHECK(p == AppGpuPreference::Unspecified);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\NUE\\Tests");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\NUE");
    // Real value for this test exe (read only)
    AppGpuPreference real;
    CHECK_OK(get_app_gpu_preference(current_exe_path(), real));
    INFO("system setting for this exe: %d (0 unspecified, 1 power saving, 2 high performance)", int(real));
}

static void test_discrete_exports() {
    TEST_BEGIN("discrete GPU exports");
    const auto x = query_discrete_gpu_exports();
    CHECK(x.nvidia && x.amd);   // this test executable is built with nue_win_request_discrete_gpu()
    CHECK(*reinterpret_cast<DWORD*>(GetProcAddress(GetModuleHandleW(nullptr), "NvOptimusEnablement")) == 1);
}

static void test_loss_classification() {
    TEST_BEGIN("device loss classification");
    CHECK(classify_hresult(S_OK) == GpuLoss::None);
    CHECK(classify_hresult(0x08760878) == GpuLoss::None);   // S_PRESENT_OCCLUDED
    CHECK(classify_hresult(HRESULT(0x88760868)) == GpuLoss::DeviceLost);
    CHECK(classify_hresult(HRESULT(0x88760869)) == GpuLoss::DeviceNotReset);
    CHECK(classify_hresult(HRESULT(0x88760870)) == GpuLoss::DeviceRemoved);
    CHECK(classify_hresult(HRESULT(0x88760874)) == GpuLoss::DeviceHung);
    CHECK(classify_hresult(HRESULT(0x887A0005)) == GpuLoss::DeviceRemoved);
    CHECK(classify_hresult(HRESULT(0x887A0006)) == GpuLoss::DeviceHung);
    CHECK(classify_hresult(HRESULT(0x887A0007)) == GpuLoss::DeviceReset);
    CHECK(classify_hresult(HRESULT(0x887A0020)) == GpuLoss::DriverInternalError);
    CHECK(classify_hresult(E_OUTOFMEMORY) == GpuLoss::OutOfMemory);
    CHECK(classify_hresult(HRESULT(0x8876086C)) == GpuLoss::InvalidCall);
    CHECK(classify_hresult(E_FAIL) == GpuLoss::Unknown);
    CHECK(is_loss(GpuLoss::DeviceRemoved) && !is_loss(GpuLoss::InvalidCall) && !is_loss(GpuLoss::None));
}

static void test_recovery() {
    TEST_BEGIN("GpuRecovery state machine");
    using A = GpuRecovery::Action;
    using S = GpuRecovery::State;
    const HRESULT LOST = HRESULT(0x88760868), NOTRESET = HRESULT(0x88760869), REMOVED = HRESULT(0x887A0005);
    {   // alt-tab in exclusive mode: lost for a while, then back without a reset
        GpuRecovery r;
        int lost = 0, rec = 0;
        r.on_lost = [&](GpuLoss) { ++lost; };
        r.on_recovered = [&] { ++rec; };
        CHECK(r.on_result(S_OK) == A::Continue);
        for (int i = 0; i < 50; ++i) CHECK(r.on_result(LOST) == A::WaitAndRetry);
        CHECK(r.state() == S::Lost && lost == 1);
        CHECK(r.on_result(S_OK) == A::Continue);
        CHECK(r.state() == S::Running && rec == 1 && r.total_recoveries() == 1);
    }
    {   // NOTRESET: reset works the second time
        GpuRecovery r;
        CHECK(r.on_result(LOST) == A::WaitAndRetry);
        CHECK(r.on_result(NOTRESET) == A::ResetDevice);
        r.on_reset_result(false);
        CHECK(r.state() == S::Lost && r.retry_delay_ms() == 200);
        CHECK(r.on_result(NOTRESET) == A::ResetDevice);
        r.on_reset_result(true);
        CHECK(r.state() == S::Running && r.retry_delay_ms() == 100);
    }
    {   // NOTRESET and the reset keeps failing: escalate to recreation after 3 attempts, then it works
        GpuRecovery r;
        for (int i = 0; i < 3; ++i) { CHECK(r.on_result(NOTRESET) == A::ResetDevice); r.on_reset_result(false); }
        CHECK(r.on_result(NOTRESET) == A::RecreateDevice);
        r.on_recreate_result(true);
        CHECK(r.state() == S::Running);
    }
    {   // driver reinstall: removed -> recreate; the first recreate fails (driver still installing), the second works
        GpuRecovery r;
        bool failed = false;
        r.on_failed = [&] { failed = true; };
        CHECK(r.on_result(REMOVED) == A::RecreateDevice);
        r.on_recreate_result(false);
        CHECK(r.state() == S::Lost);
        CHECK(r.on_result(REMOVED) == A::RecreateDevice);
        r.on_recreate_result(true);
        CHECK(r.state() == S::Running && !failed && r.total_losses() == 1 && r.total_recoveries() == 1);
    }
    {   // never recovers: give up after max_recreate_attempts
        GpuRecovery r;
        bool failed = false;
        r.on_failed = [&] { failed = true; };
        for (int i = 0; i < 3; ++i) { CHECK(r.on_result(REMOVED) == A::RecreateDevice); r.on_recreate_result(false); }
        CHECK(failed && r.state() == S::Failed);
        CHECK(r.on_result(REMOVED) == A::GiveUp);
        CHECK(r.on_result(S_OK) == A::GiveUp);
    }
    {   // errors that are not device loss never start a recovery
        GpuRecovery r;
        CHECK(r.on_result(E_FAIL) == A::Continue && r.on_result(E_OUTOFMEMORY) == A::Continue && r.on_result(HRESULT(0x8876086C)) == A::Continue);
        CHECK(r.state() == S::Running && r.total_losses() == 0);
    }
    {   // backoff cap
        GpuRecovery::Policy p;
        p.max_reset_attempts = 100;
        GpuRecovery r(p);
        for (int i = 0; i < 10; ++i) { r.on_result(NOTRESET); r.on_reset_result(false); }
        CHECK(r.retry_delay_ms() == p.max_delay_ms);
    }
}

static void test_adapter_watcher() {
    TEST_BEGIN("adapter watcher");
    AdapterWatcher w;
    CHECK_OK(w.start());
    INFO("event based: %d, adapters at start: %zu", int(w.uses_event()), w.adapters_at_start().size());
    CHECK(!w.adapters_at_start().empty());
    CHECK(!w.changed());
    w.inject_change_for_test();
    CHECK(w.changed());
    CHECK(!w.changed());
    w.stop();
}

static void test_fit_aspect() {
    TEST_BEGIN("fit_aspect");
    FitRect r = fit_aspect(1280, 720, 0, 0, 1920, 1080);
    CHECK(r.x == 0 && r.y == 0 && r.width == 1920 && r.height == 1080);
    r = fit_aspect(1280, 720, 0, 0, 1024, 768);   // 16:9 into 4:3: bars top and bottom
    CHECK(r.width == 1024 && r.height == 576 && r.x == 0 && r.y == 96);
    r = fit_aspect(640, 480, 100, 50, 1920, 1080);   // 4:3 into 16:9: bars left and right, offset area
    CHECK(r.height == 1080 && r.width == 1440 && r.x == 100 + 240 && r.y == 50);
    r = fit_aspect(0, 480, 0, 0, 100, 100);
    CHECK(r.width == 0 && r.height == 0);
    r = fit_aspect(1, 1000, 0, 0, 100, 100);       // extreme aspect never collapses to zero
    CHECK(r.width >= 1 && r.height == 100);
}

static void test_monitors() {
    TEST_BEGIN("dpi and monitors");
    Status detail;
    DpiAwareness got = enable_best_dpi_awareness(&detail);
    INFO("dpi awareness: %s (query: %s)", dpi_awareness_name(got), dpi_awareness_name(query_process_dpi_awareness()));
    CHECK(got == DpiAwareness::PerMonitorV2 || got == DpiAwareness::PerMonitor || got == DpiAwareness::System);

    std::vector<MonitorInfo> mons;
    CHECK(enumerate_monitors(mons));
    CHECK(!mons.empty() && mons[0].primary);
    for (const auto& m : mons) {
        INFO("%s \"%s\" %dx%d @ %u Hz (exact %.3f), dpi %u (%.0f%%), HDR supported %s enabled %s, bits %u, SDR white %.0f nits, work %ldx%ld", to_utf8(m.device_name).c_str(), m.friendly_name.c_str(), m.width(), m.height(),
             m.current.refresh_hz, m.refresh_exact(), m.dpi, m.scale * 100, support_name(m.hdr_supported), support_name(m.hdr_enabled), m.bits_per_color, m.sdr_white_level_nits,
             m.work_rect.right - m.work_rect.left, m.work_rect.bottom - m.work_rect.top);
        CHECK(m.width() > 0 && m.height() > 0);
        CHECK(m.dpi >= 96);
        CHECK(m.current.width > 0);
        const auto modes = enumerate_display_modes(m.device_name);
        CHECK(!modes.empty());
        INFO("  %zu display modes", modes.size());
        DisplayMode cur = m.current;
        CHECK(std::find(modes.begin(), modes.end(), cur) != modes.end());
        CHECK_OK(test_display_mode(m.device_name, cur));   // CDS_TEST: nothing changes
        DisplayMode best;
        CHECK(find_display_mode(modes, cur.width, cur.height, cur.refresh_hz, best) && best.width == cur.width && best.height == cur.height && best.refresh_hz <= cur.refresh_hz);
    }
    nuetest::HiddenWindow w;
    MonitorInfo mi;
    CHECK(monitor_of_window(w.hwnd, mi));
    CHECK(window_dpi(w.hwnd) >= 96);
    RECT v = virtual_screen_rect();
    CHECK(v.right > v.left && v.bottom > v.top);
}

static void test_find_mode() {
    TEST_BEGIN("find_display_mode (synthetic)");
    std::vector<DisplayMode> modes = {{1280, 720, 32, 60, false}, {1280, 720, 32, 120, false}, {1280, 720, 32, 144, false}, {1920, 1080, 32, 59, false}, {1920, 1080, 32, 60, false}, {1920, 1080, 32, 60, true}};
    DisplayMode m;
    CHECK(find_display_mode(modes, 1280, 720, 100, m) && m.refresh_hz == 60);       // closest not above
    CHECK(find_display_mode(modes, 1280, 720, 120, m) && m.refresh_hz == 120);
    CHECK(find_display_mode(modes, 1280, 720, 240, m) && m.refresh_hz == 144);
    CHECK(find_display_mode(modes, 1280, 720, 30, m) && m.refresh_hz == 60);        // nothing below: lowest above
    CHECK(find_display_mode(modes, 1920, 1080, 60, m) && m.refresh_hz == 60 && !m.interlaced);
    CHECK(!find_display_mode(modes, 800, 600, 60, m));
}

static void test_gamemode() {
    TEST_BEGIN("game mode and gaming state");
    const GameModeInfo g = query_game_mode_info();
    INFO("game mode allowed: %s, game bar: %s, background capture: %s, capture policy-blocked: %s", support_name(g.game_mode_allowed), support_name(g.game_bar_enabled),
         support_name(g.background_capture_enabled), support_name(g.capture_forbidden_by_policy));
    INFO("power mode: %s, game mode active: %d, notification state: %s, game bar running: %d, FSO disabled for this exe: %s", power_mode_name(g.power_mode), int(g.game_mode_active),
         notification_state_name(g.notification_state), int(g.game_bar_running), support_name(g.fullscreen_optimizations_disabled));
    CHECK(g.notification_state >= 0);

    CHECK(layers_contain(L"~ HIGHDPIAWARE DISABLEDXMAXIMIZEDWINDOWEDMODE", L"DISABLEDXMAXIMIZEDWINDOWEDMODE"));
    CHECK(!layers_contain(L"~ HIGHDPIAWARE", L"DISABLEDXMAXIMIZEDWINDOWEDMODE"));
    CHECK(layers_with_flag(L"", L"DISABLEDXMAXIMIZEDWINDOWEDMODE", true) == L"~ DISABLEDXMAXIMIZEDWINDOWEDMODE");
    CHECK(layers_with_flag(L"~ HIGHDPIAWARE", L"DISABLEDXMAXIMIZEDWINDOWEDMODE", true) == L"~ HIGHDPIAWARE DISABLEDXMAXIMIZEDWINDOWEDMODE");
    CHECK(layers_with_flag(L"~ HIGHDPIAWARE DISABLEDXMAXIMIZEDWINDOWEDMODE", L"DISABLEDXMAXIMIZEDWINDOWEDMODE", false) == L"~ HIGHDPIAWARE");
    CHECK(layers_with_flag(L"~ DISABLEDXMAXIMIZEDWINDOWEDMODE", L"DISABLEDXMAXIMIZEDWINDOWEDMODE", false).empty());

    const wchar_t* key = L"Software\\NUE\\Tests\\Layers";
    CompatLayerStore store{key};
    const std::wstring exe = L"C:\\Games\\NUE Test\\game.exe";
    CHECK(query_fullscreen_optimizations_disabled(exe, store) == Support::No);
    CHECK_OK(set_fullscreen_optimizations_disabled(exe, true, store));
    CHECK(query_fullscreen_optimizations_disabled(exe, store) == Support::Yes);
    CHECK_OK(set_fullscreen_optimizations_disabled(exe, false, store));
    CHECK(query_fullscreen_optimizations_disabled(exe, store) == Support::No);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\NUE\\Tests");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\NUE");

    nuetest::HiddenWindow w;
    CHECK(!window_is_excluded_from_capture(w.hwnd));
    CHECK_OK(window_exclude_from_capture(w.hwnd, true));
    CHECK(window_is_excluded_from_capture(w.hwnd));
    DWORD affinity = 0;
    GetWindowDisplayAffinity(w.hwnd, &affinity);
    INFO("display affinity value: 0x%X (0x11 = exclude from capture)", affinity);
    CHECK_OK(window_exclude_from_capture(w.hwnd, false));
    CHECK(!window_is_excluded_from_capture(w.hwnd));
}

int main() {
    test_fit_aspect();
    test_find_mode();
    test_monitors();
    test_gpu_enumeration();
    test_integrated_heuristic();
    test_app_gpu_preference();
    test_discrete_exports();
    test_loss_classification();
    test_recovery();
    test_adapter_watcher();
    test_gamemode();
    return finish("gpu_display_tests");
}
