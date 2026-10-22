// windows_probe: prints what this Windows machine supports, as far as the NUE Windows backend can tell. Console only: no window is shown, no
// sound is played, no GPU device is created (add --probe-gpu to also test D3D11/D3D12 feature levels, which can wake a sleeping discrete GPU),
// nothing is changed on the system. Output is plain text; --json is not offered on purpose (the numbers are for people).
#include <cstdio>
#include <cstring>
#include <string>

#include "nue_win/audio.hpp"
#include "nue_win/cpu.hpp"
#include "nue_win/diag.hpp"
#include "nue_win/display.hpp"
#include "nue_win/gamemode.hpp"
#include "nue_win/gpu.hpp"
#include "nue_win/input.hpp"
#include "nue_win/power.hpp"
#include "nue_win/storage.hpp"

using namespace nue::win;

static void heading(const char* t) { std::printf("\n== %s\n", t); }
static const char* yn(bool b) { return b ? "yes" : "no"; }

int main(int argc, char** argv) {
    bool probe_gpu = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--probe-gpu")) probe_gpu = true;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const OsVersion os = os_version();
    std::printf("NUE Windows backend probe -- Windows %u.%u build %u (%s), %s process\n", os.major, os.minor, os.build, is_windows11() ? "Windows 11" : "Windows 10 or older", sizeof(void*) == 4 ? "32-bit" : "64-bit");

    heading("CPU");
    CpuTopology cpu;
    if (query_cpu_topology(cpu)) {
        std::printf("%s [%s]\n%u logical / %u physical, groups %u, SMT %s, hybrid (P/E cores) %s%s\n", cpu.brand.c_str(), cpu.vendor.c_str(), cpu.logical_processors, cpu.physical_cores, cpu.processor_groups,
                    yn(cpu.smt), yn(cpu.hybrid), cpu.wow64 ? ", running under WOW64 (32-bit on 64-bit Windows)" : "");
        if (cpu.hybrid) std::printf("performance CPU sets: %zu, efficiency CPU sets: %zu\n", cpu.performance_set_ids.size(), cpu.efficiency_set_ids.size());
    } else {
        std::printf("CPU Sets not available\n");
    }
    const CpuFrequencyInfo f = query_cpu_frequency();
    if (f.valid) std::printf("frequency: current %u MHz, max %u MHz, limit %u MHz%s\n", f.current_mhz, f.max_mhz, f.limit_mhz, f.limited() ? "  (LIMITED: thermal or policy)" : "");

    heading("Power and scheduling");
    const PowerStatus p = query_power_status();
    std::printf("AC power: %s, battery: %s %d%%, energy saver: %s\n", support_name(p.on_ac), yn(p.has_battery), p.battery_percent, yn(p.energy_saver));
    std::printf("effective power mode: %s\n", power_mode_name(query_effective_power_mode()));
    ThrottlingState ts;
    if (process_query_power_throttling(ts).ok) std::printf("EcoQoS: automatic (system decides)=%s, timer-resolution throttling opt-out available=%s\n", yn(!ts.execution_speed_forced), yn(is_windows11()));
    TimerResolutionInfo tr;
    if (query_timer_resolution(tr)) std::printf("system timer: coarsest %.3f ms, finest %.3f ms, current %.3f ms\n", tr.minimum_ms, tr.maximum_ms, tr.current_ms);
    PreciseSleeper sleeper;
    std::printf("high-resolution waitable timer: %s\n", yn(sleeper.high_resolution()));
    {
        MmcssScope m;
        Status s = m.begin(L"Games");
        std::printf("MMCSS 'Games' task: %s\n", s.ok ? "available" : error_message(s.code).c_str());
    }
    for (const auto& z : read_thermal_zones()) std::printf("thermal zone %s: %.1f C (passive limit %.0f%%)\n", z.name.c_str(), z.celsius, z.passive_limit_percent);

    heading("GPU");
    GpuEnumeration g;
    if (enumerate_gpus(g, true, probe_gpu).ok) {
        std::printf("DXGI factory6 (GPU preference enumeration): %s, tearing / VRR present supported: %s, hybrid graphics: %s\n", yn(g.factory6), yn(g.tearing_supported), yn(g.hybrid));
        for (const auto& a : g.adapters) {
            std::printf("#%u %s [%s %04X:%04X] %s%s%s\n   VRAM %.0f MiB, shared %.0f MiB, driver %s, budget %.0f MiB, outputs %zu%s%s\n", a.index, a.name.c_str(), vendor_name(a.vendor_id), a.vendor_id, a.device_id,
                        a.software ? "software " : "", a.integrated ? "integrated" : "", a.discrete ? "discrete" : "", a.dedicated_video_memory / 1048576.0, a.shared_system_memory / 1048576.0,
                        a.driver_version.c_str(), a.memory.budget / 1048576.0, a.outputs.size(), static_cast<int>(a.index) == g.high_performance_index ? "  <- DXGI high-performance choice" : "",
                        static_cast<int>(a.index) == g.power_saving_index ? "  <- power-saving choice" : "");
            if (probe_gpu) std::printf("   D3D11 FL11_0: %s, D3D12 FL11_0: %s\n", yn(a.d3d11_level_11_0), yn(a.d3d12_level_11_0));
        }
    }
    const GraphicsSystemSettings gs = query_graphics_system_settings();
    std::printf("hardware-accelerated GPU scheduling: %s, windowed-game optimizations: %s, windowed VRR: %s, TDR delay %us%s\n", support_name(gs.hardware_scheduling), support_name(gs.game_mode_optimizations_windowed),
                support_name(gs.variable_refresh_rate_windowed), gs.tdr_delay_seconds ? gs.tdr_delay_seconds : 2, gs.tdr_disabled ? " (TDR DISABLED)" : "");
    AppGpuPreference pref;
    get_app_gpu_preference(current_exe_path(), pref);
    const DiscreteGpuExports ex = query_discrete_gpu_exports();
    std::printf("this exe: per-app GPU preference %d (0 system, 1 power saving, 2 high performance); exports NvOptimusEnablement=%s AmdPowerXpress=%s\n", int(pref), yn(ex.nvidia), yn(ex.amd));

    heading("Displays");
    std::printf("DPI awareness of this process: %s\n", dpi_awareness_name(enable_best_dpi_awareness()));
    std::vector<MonitorInfo> mons;
    if (enumerate_monitors(mons)) {
        for (const auto& m : mons) {
            const auto modes = enumerate_display_modes(m.device_name);
            std::printf("%s%s \"%s\": %dx%d @ %.3f Hz, DPI %u (%.0f%%), HDR supported %s / on %s, %u bits, %zu modes\n", to_utf8(m.device_name).c_str(), m.primary ? " (primary)" : "", m.friendly_name.c_str(), m.width(),
                        m.height(), m.refresh_exact(), m.dpi, m.scale * 100, support_name(m.hdr_supported), support_name(m.hdr_enabled), m.bits_per_color, modes.size());
        }
    }

    heading("Game Mode and gaming state");
    const GameModeInfo gm = query_game_mode_info();
    std::printf("Game Mode allowed by user: %s, active now: %s (effective power mode %s)\n", support_name(gm.game_mode_allowed), yn(gm.game_mode_active), power_mode_name(gm.power_mode));
    std::printf("Game Bar enabled: %s, running: %s, background capture: %s, capture blocked by policy: %s\n", support_name(gm.game_bar_enabled), yn(gm.game_bar_running), support_name(gm.background_capture_enabled),
                support_name(gm.capture_forbidden_by_policy));
    std::printf("notification state: %s; fullscreen optimizations disabled for this exe: %s\n", notification_state_name(gm.notification_state), support_name(gm.fullscreen_optimizations_disabled));

    heading("Input");
    XInput xi;
    std::printf("XInput: %s (%s), Guide button: %s\n", yn(xi.available()), xi.dll_name(), yn(xi.uses_guide_button()));
    int pads = 0;
    for (int s = 0; s < XInput::MaxPads; ++s) { GamepadState st; if (xi.poll(s, st)) ++pads; }
    std::printf("XInput pads connected: %d\n", pads);
    const GameInputProbe gi = probe_game_input();
    std::printf("GameInput: dll %s, create %s, redistributable %s (%s)\n", yn(gi.dll_present), yn(gi.create_ok), yn(gi.redistributable_present), gi.note.c_str());
    std::vector<RawDeviceInfo> raw;
    if (enumerate_raw_devices(raw)) {
        int mice = 0, kb = 0, hid = 0;
        for (const auto& d : raw) { mice += d.type == RawDeviceInfo::Mouse; kb += d.type == RawDeviceInfo::Keyboard; hid += d.type == RawDeviceInfo::Hid; }
        std::printf("Raw Input devices: %d mice, %d keyboards, %d HID\n", mice, kb, hid);
    }

    heading("Audio (read only)");
    std::vector<AudioDeviceInfo> out;
    if (enumerate_audio_devices(AudioFlow::Render, out).ok) {
        for (const auto& d : out)
            std::printf("%s%s: mix %u Hz %uch %s; exclusive period %.2f..%.2f ms; shared %.2f..%.2f ms%s\n", d.name.c_str(), d.is_default ? " [default]" : "", d.mix_format.sample_rate, d.mix_format.channels,
                        sample_format_name(d.mix_format.format), d.minimum_period_ms, d.default_period_ms, d.shared_minimum_period_ms, d.shared_default_period_ms, d.supports_low_latency_shared ? "  (IAudioClient3 low latency)" : "");
        if (out.empty()) std::printf("no active render device\n");
    }

    heading("Storage");
    AppDirs dirs;
    AppDirOptions opt;
    opt.create = false;
    if (resolve_app_dirs("NUE", "Probe", opt, dirs).ok) {
        std::printf("saves:  %s%s\nconfig: %s\nlogs:   %s\ncrash dumps: %s\n", path_to_utf8(dirs.saves).c_str(), dirs.saves_on_onedrive ? "  (inside OneDrive)" : "", path_to_utf8(dirs.config).c_str(),
                    path_to_utf8(dirs.logs).c_str(), path_to_utf8(dirs.crash_dumps).c_str());
        std::printf("case sensitivity of the profile folder: %s; long paths enabled for this process: %s\n",
                    directory_case_mode(dirs.config.substr(0, dirs.config.find_last_of(L'\\'))) == CaseMode::Sensitive ? "SENSITIVE" : "insensitive/unknown", yn(long_paths_enabled()));
    }

    heading("Diagnostics");
    GpuEngineSampler gpu;
    std::printf("GPU engine performance counters: %s\n", gpu.open().ok ? "available" : "not available");
    const MemorySample mem = sample_memory();
    std::printf("process memory: working set %llu MiB, handles %u\n", (unsigned long long)(mem.working_set >> 20), mem.handle_count);
    std::printf("ETW provider NUE.Engine {c8e1a4b2-3d57-4f6a-9b0e-71d2f5a8c3e4}: %s\n", etw_register().ok ? "registered" : "failed");
    etw_unregister();
    return 0;
}
