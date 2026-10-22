// Tests for the machine-description modules, run against in-memory fixtures that reproduce
// what a Steam Deck, an Intel hybrid laptop and an arm64 board expose in /proc and /sys.
#include "fixtures.hpp"
#include "lx_gpu.hpp"
#include "lx_policy.hpp"
#include "lx_session.hpp"
#include "lx_sysinfo.hpp"
#include "lx_test.hpp"
#include "lx_text.hpp"
#include "lx_xdg.hpp"

using namespace nue::lx;

TEST(xdg_defaults_and_overrides) {
    MemFs fs;
    MapEnv env;
    env.vars["HOME"] = "/home/leo";
    XdgDirs x = resolve_xdg(env, fs);
    CHECK_EQ(x.data_home, std::string("/home/leo/.local/share"));
    CHECK_EQ(x.config_home, std::string("/home/leo/.config"));
    CHECK_EQ(x.cache_home, std::string("/home/leo/.cache"));
    CHECK_EQ(x.state_home, std::string("/home/leo/.local/state"));
    CHECK(x.runtime_dir.empty());
    CHECK_EQ(x.data_dirs.size(), size_t(2));

    env.vars["XDG_DATA_HOME"] = "relative/ignored";
    env.vars["XDG_CACHE_HOME"] = "/mnt/fast/cache/";
    env.vars["XDG_DATA_DIRS"] = "/opt/a:/usr/share:/opt/a:rel";
    env.vars["XDG_RUNTIME_DIR"] = "/run/user/1000";
    x = resolve_xdg(env, fs);
    CHECK_EQ(x.data_home, std::string("/home/leo/.local/share"));  // relative path rejected
    CHECK_EQ(x.cache_home, std::string("/mnt/fast/cache"));
    CHECK_EQ(x.data_dirs.size(), size_t(2));  // de-duplicated, relative dropped
    CHECK_EQ(x.runtime_dir, std::string("/run/user/1000"));

    AppDirs a = app_dirs(x, "nue/nfsu2");
    CHECK_EQ(a.saves, std::string("/home/leo/.local/share/nue/nfsu2/saves"));
    CHECK_EQ(a.crashes, std::string("/home/leo/.local/state/nue/nfsu2/crashes"));
}

TEST(xdg_user_dirs_and_flatpak) {
    MemFs fs;
    fs.add_file("/home/leo/.config/user-dirs.dirs",
                "# comment\nXDG_DESKTOP_DIR=\"$HOME/Escritorio\"\nXDG_DOCUMENTS_DIR=\"$HOME/Documentos\"\nXDG_DOWNLOAD_DIR=\"/data/dl\"\n");
    MapEnv env;
    env.vars["HOME"] = "/home/leo";
    XdgDirs x = resolve_xdg(env, fs);
    CHECK_EQ(x.documents, std::string("/home/leo/Documentos"));
    CHECK_EQ(x.downloads, std::string("/data/dl"));
    // Inside Flatpak the runtime sets the XDG variables, which must be honoured verbatim.
    env.vars["XDG_DATA_HOME"] = "/home/leo/.var/app/org.nue.game/data";
    x = resolve_xdg(env, fs);
    CHECK_EQ(app_dirs(x, "game").data, std::string("/home/leo/.var/app/org.nue.game/data/game"));
}

TEST(sysinfo_parsers) {
    CpuInfoText c = parse_cpuinfo(fixtures::kCpuInfoX86);
    CHECK_EQ(c.processors, 2);
    CHECK_EQ(c.vendor, std::string("AuthenticAMD"));
    CHECK(c.model_name.find("Custom APU 0405") != std::string::npos);
    CHECK(cpu_has_flag(c, "avx2"));
    CHECK(!cpu_has_flag(c, "avx512f"));
    CHECK(!c.is_arm);

    CpuInfoText a = parse_cpuinfo(fixtures::kCpuInfoArm);
    CHECK(a.is_arm);
    CHECK_EQ(a.processors, 4);
    CHECK_EQ(a.arm_parts.size(), size_t(4));
    CHECK_EQ(a.arm_parts[0], std::string("0x41:0xd08"));
    CHECK(a.model_name.find("Raspberry Pi 4") != std::string::npos);

    MemInfo m = parse_meminfo("MemTotal:       16384000 kB\nMemFree:  1000 kB\nMemAvailable:   8000000 kB\nSwapTotal: 100 kB\nSwapFree: 50 kB\n");
    CHECK_EQ(m.total_kb, uint64_t(16384000));
    CHECK_EQ(m.available_kb, uint64_t(8000000));

    OsRelease o = parse_os_release("NAME=\"SteamOS\"\nPRETTY_NAME=\"SteamOS Holo\"\nID=steamos\nID_LIKE=arch\nVARIANT_ID=steamdeck\nBUILD_ID=20240101.1\n");
    CHECK_EQ(o.id, std::string("steamos"));
    CHECK_EQ(o.pretty_name, std::string("SteamOS Holo"));
    CHECK_EQ(o.id_like, std::string("arch"));

    CpuTimes t1, t2;
    CHECK(parse_proc_stat_cpu("cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 1 1 1 1\n", t1));
    CHECK(parse_proc_stat_cpu("cpu  200 0 200 1000 0 0 0 0 0 0\n", t2));
    double busy = cpu_busy_fraction(t1, t2);
    CHECK(busy > 0.499 && busy < 0.501);  // 200 busy jiffies out of 400 elapsed

    ProcStatus ps = parse_proc_status("Name:\tx\nVmRSS:\t  1234 kB\nThreads:\t17\nCpus_allowed_list:\t0-7\n");
    CHECK_EQ(ps.vm_rss_kb, uint64_t(1234));
    CHECK_EQ(ps.threads, 17);
    CHECK_EQ(ps.cpus_allowed_list, std::string("0-7"));
}

TEST(topology_steam_deck_is_not_hybrid) {
    MemFs fs;
    fixtures::steam_deck(fs);
    CpuTopology t = read_cpu_topology(fs);
    CHECK_EQ(t.cpus.size(), size_t(8));
    CHECK(!t.hybrid);
    CHECK_EQ(t.physical_core_count(), 4);
    CHECK_EQ(t.governor, std::string("powersave"));
    CHECK_EQ(t.scaling_driver, std::string("amd-pstate-epp"));
    AffinityPlan p = plan_affinity(t);
    CHECK(!p.use_pinning);  // on a uniform CPU the kernel scheduler is left alone
    CHECK_EQ(p.main_thread.size(), size_t(1));
    CHECK_EQ(p.render_thread.size(), size_t(1));
    CHECK(p.background.empty());
}

TEST(topology_intel_hybrid) {
    MemFs fs;
    fixtures::intel_hybrid(fs);  // 2 P-cores with HT (cpu0-3) + 4 E-cores (cpu4-7)
    CpuTopology t = read_cpu_topology(fs);
    CHECK(t.hybrid);
    CHECK_EQ(t.hybrid_source, std::string("intel-sysfs"));
    CHECK_EQ(format_cpulist(t.cpus_of_class(CoreClass::Efficiency)), std::string("4-7"));
    CHECK_EQ(format_cpulist(t.cpus_of_class(CoreClass::Performance)), std::string("0-3"));
    AffinityPlan p = plan_affinity(t);
    CHECK(p.use_pinning);
    CHECK_EQ(format_cpulist(p.background), std::string("4-7"));
    // main + render use the two P-cores' first threads; SMT siblings go to the workers
    CHECK_EQ(p.main_thread[0], 0);
    CHECK_EQ(p.render_thread[0], 2);
    CHECK_EQ(format_cpulist(p.workers), std::string("1,3"));
}

TEST(topology_arm_big_little_by_capacity) {
    MemFs fs;
    fixtures::arm_big_little(fs);  // cpu0-3 capacity 512, cpu4-5 capacity 1024
    CpuTopology t = read_cpu_topology(fs);
    CHECK(t.hybrid);
    CHECK_EQ(t.hybrid_source, std::string("arm-capacity"));
    CHECK_EQ(format_cpulist(t.cpus_of_class(CoreClass::Efficiency)), std::string("0-3"));
    CHECK_EQ(format_cpulist(t.cpus_of_class(CoreClass::Performance)), std::string("4-5"));
}

TEST(power_and_thermal) {
    MemFs fs;
    fixtures::steam_deck(fs);
    PowerInfo p = read_power(fs);
    CHECK(p.on_battery);
    CHECK(!p.ac_online);
    CHECK_EQ(p.batteries.size(), size_t(1));  // the controller battery (scope=Device) is ignored
    CHECK_EQ(p.best_percent(), 64);
    // 20 Wh left at 10 W
    CHECK_EQ(p.batteries[0].seconds_remaining, int64_t(7200));
    auto th = read_thermal(fs);
    CHECK(th.size() >= 2);
    CHECK(max_celsius(th) > 70.4 && max_celsius(th) < 70.6);
    fs.add_file("/sys/class/power_supply/ACAD/type", "Mains\n");
    fs.add_file("/sys/class/power_supply/ACAD/online", "1\n");
    CHECK(!read_power(fs).on_battery);
}

TEST(session_steam_deck_gaming_mode) {
    MemFs fs;
    fixtures::steam_deck(fs);
    MapEnv env;
    env.vars["SteamDeck"] = "1";
    env.vars["SteamGamepadUI"] = "1";
    env.vars["SteamAppId"] = "1234";
    env.vars["DISPLAY"] = ":1";
    env.vars["XDG_SESSION_DESKTOP"] = "gamescope-wayland";
    SessionInfo s = detect_session(env, fs);
    CHECK(s.steam_deck);
    CHECK_EQ(s.deck_model, std::string("lcd"));
    CHECK(s.steamos);
    CHECK(s.gamescope);
    CHECK(s.steam_client);
    CHECK_EQ(s.display_server, DisplayServer::X11);
}

TEST(session_flatpak_snap_wsl_headless) {
    MemFs fs;
    fs.add_file("/.flatpak-info", "[Application]\nname=org.nue.Game\n");
    fs.add_file("/proc/version", "Linux version 5.15.90.1-microsoft-standard-WSL2");
    fs.add_dir("/mnt/wslg");
    MapEnv env;
    env.vars["FLATPAK_ID"] = "org.nue.Game";
    SessionInfo s = detect_session(env, fs);
    CHECK(s.flatpak && s.flatpak_id == "org.nue.Game");
    CHECK(s.wsl && s.wslg);
    CHECK(s.headless);
    CHECK(!s.container);
    MapEnv snap;
    snap.vars["SNAP"] = "/snap/g/1";
    snap.vars["SNAP_NAME"] = "g";
    snap.vars["WAYLAND_DISPLAY"] = "wayland-0";
    snap.vars["DISPLAY"] = ":0";
    SessionInfo t = detect_session(snap, MemFs());
    CHECK(t.snap && t.display_server == DisplayServer::Wayland && t.xwayland);
}

TEST(session_steam_runtime_container) {
    MemFs fs;
    fs.add_file("/etc/os-release", "NAME=\"Steam Runtime\"\nID=steamrt\nVERSION_ID=3\nVERSION_CODENAME=sniper\n");
    fs.add_dir("/run/pressure-vessel");
    SessionInfo s = detect_session(MapEnv(), fs);
    CHECK(s.steam_runtime);
    CHECK_EQ(s.steam_runtime_name, std::string("sniper"));
    CHECK(!s.container);
}

TEST(drm_and_icd_discovery) {
    MemFs fs;
    fixtures::hybrid_gpu_laptop(fs);
    auto drm = read_drm_devices(fs);
    CHECK_EQ(drm.size(), size_t(2));
    CHECK_EQ(drm[0].vendor, uint32_t(0x8086));
    CHECK(drm[0].boot_vga);
    CHECK_EQ(drm[0].kernel_driver, std::string("i915"));
    CHECK_EQ(drm[0].render_node, std::string("renderD128"));
    CHECK_EQ(drm[1].vendor, uint32_t(0x10de));
    CHECK(drm[1].discrete_guess);

    MapEnv env;
    env.vars["HOME"] = "/home/leo";
    XdgDirs x = resolve_xdg(env, fs);
    auto icds = read_vulkan_icds(env, fs, x);
    CHECK_EQ(icds.size(), size_t(2));
    bool anv = false, nv = false;
    for (auto& i : icds) {
        anv |= i.driver == "anv";
        nv |= i.driver == "nvidia";
        CHECK(!i.software);
    }
    CHECK(anv && nv);
    CHECK_EQ(icds[0].api_version >> 22, uint32_t(1));

    env.vars["VK_DRIVER_FILES"] = "/usr/share/vulkan/icd.d/lvp_icd.json";
    fs.add_file("/usr/share/vulkan/icd.d/lvp_icd.json", "{\"ICD\":{\"library_path\":\"libvulkan_lvp.so\",\"api_version\":\"1.3.0\"}}");
    icds = read_vulkan_icds(env, fs, x);
    CHECK_EQ(icds.size(), size_t(1));
    CHECK(icds[0].software && icds[0].driver == "lavapipe");
}

namespace {
GraphicsProbe hybrid_probe(MemFs& fs, bool live) {
    fixtures::hybrid_gpu_laptop(fs);
    MapEnv env;
    env.vars["HOME"] = "/home/leo";
    GraphicsProbe p;
    p.drm = read_drm_devices(fs);
    p.icds = read_vulkan_icds(env, fs, resolve_xdg(env, fs));
    p.gl.gl = p.gl.egl = p.gl.gles2 = true;
    if (live) {
        p.vk.attempted = p.vk.loader_found = p.vk.instance_ok = true;
        p.vk.ext_surface = p.vk.ext_wayland = p.vk.ext_xcb = true;
        p.vk.devices.push_back({"Intel(R) UHD Graphics", 0x8086, 0x9a49, (1u << 22) | (3u << 12), 0, 1});
        p.vk.devices.push_back({"NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2560, (1u << 22) | (3u << 12), 0, 2});
        p.vk.devices.push_back({"llvmpipe (LLVM 15.0.7, 256 bits)", 0x10005, 0, (1u << 22) | (3u << 12), 0, 4});
    }
    return p;
}
}  // namespace

TEST(renderer_plan_prefers_discrete_and_sets_prime_env) {
    MemFs fs;
    GraphicsProbe p = hybrid_probe(fs, true);
    SessionInfo s;
    s.display_server = DisplayServer::Wayland;
    RendererPlan r = plan_renderer(p, PlanRequest(), s);
    CHECK(r.api == GfxApi::Vulkan);
    CHECK(r.hybrid_gpu);
    CHECK_EQ(r.bgfx_vendor_id, uint16_t(0x10de));
    CHECK(r.device_name.find("RTX 3060") != std::string::npos);
    bool offload = false, select = false;
    for (auto& kv : r.env) {
        offload |= kv.first == "__NV_PRIME_RENDER_OFFLOAD" && kv.second == "1";
        select |= kv.first == "MESA_VK_DEVICE_SELECT" && kv.second == "10de:2560";
    }
    CHECK(offload && select);
    CHECK(r.fallbacks.size() >= 1);  // OpenGL / GLES behind Vulkan
    CHECK(!r.software);
}

TEST(renderer_plan_low_power_picks_integrated) {
    MemFs fs;
    GraphicsProbe p = hybrid_probe(fs, true);
    PlanRequest req;
    req.prefer_low_power = true;
    RendererPlan r = plan_renderer(p, req, SessionInfo());
    CHECK_EQ(r.bgfx_vendor_id, uint16_t(0x8086));
    for (auto& kv : r.env) CHECK(kv.first != "__NV_PRIME_RENDER_OFFLOAD");
    CHECK(r.env.empty() || r.env[0].first == "MESA_VK_DEVICE_SELECT");
}

TEST(renderer_plan_explicit_choice_and_unknown_choice) {
    MemFs fs;
    GraphicsProbe p = hybrid_probe(fs, true);
    PlanRequest req;
    req.vendor_id = 0x8086;
    req.device_id = 0x9a49;
    CHECK_EQ(plan_renderer(p, req, SessionInfo()).bgfx_vendor_id, uint16_t(0x8086));
    req.vendor_id = 0x1002;  // not present
    RendererPlan r = plan_renderer(p, req, SessionInfo());
    CHECK(!r.warnings.empty());
    CHECK_EQ(r.bgfx_vendor_id, uint16_t(0x10de));
}

TEST(renderer_plan_software_only_warns) {
    GraphicsProbe p;
    p.vk.attempted = p.vk.loader_found = p.vk.instance_ok = true;
    p.vk.ext_surface = true;
    p.vk.devices.push_back({"llvmpipe", 0x10005, 0, (1u << 22) | (3u << 12), 0, 4});
    p.gl.egl = true;
    RendererPlan r = plan_renderer(p, PlanRequest(), SessionInfo());
    CHECK(r.api == GfxApi::Vulkan && r.software);
    CHECK(!r.warnings.empty());
    PlanRequest ok;
    ok.allow_software = true;
    CHECK(plan_renderer(p, ok, SessionInfo()).warnings.empty());
}

TEST(renderer_plan_no_vulkan_falls_back_to_gles_on_soc) {
    MemFs fs;
    fixtures::raspberry_pi3(fs);  // vc4: GLES only, no Vulkan driver
    GraphicsProbe p;
    p.drm = read_drm_devices(fs);
    CHECK(p.drm.size() == 1 && p.drm[0].platform);
    p.vk.attempted = true;
    p.vk.loader_found = true;
    p.vk.error = "no physical device";
    p.gl.egl = p.gl.gles2 = true;
    RendererPlan r = plan_renderer(p, PlanRequest(), SessionInfo());
    CHECK(r.api == GfxApi::OpenGLES);
    CHECK(!r.notes.empty());
    GraphicsProbe nothing;
    RendererPlan n = plan_renderer(nothing, PlanRequest(), SessionInfo());
    CHECK(n.api == GfxApi::None && !n.warnings.empty());
}

TEST(renderer_plan_wayland_without_surface_extension_warns) {
    MemFs fs;
    GraphicsProbe p = hybrid_probe(fs, true);
    p.vk.ext_wayland = false;
    SessionInfo s;
    s.display_server = DisplayServer::Wayland;
    RendererPlan r = plan_renderer(p, PlanRequest(), s);
    bool warned = false;
    for (auto& w : r.warnings) warned |= w.find("wayland") != std::string::npos || w.find("Wayland") != std::string::npos;
    CHECK(warned);
}

TEST(audio_policy) {
    MemFs fs;
    fs.add_file("/run/user/1000/pipewire-0", "");
    fs.add_file("/run/user/1000/pulse/native", "");
    MapEnv env;
    env.vars["XDG_RUNTIME_DIR"] = "/run/user/1000";
    XdgDirs x = resolve_xdg(env, fs, "/home/u");
    AudioAvailability a = detect_audio(env, fs, x);
    CHECK(a.pipewire_socket && a.pulse_socket);
    HintSet h = plan_audio(a, AudioOptions(), SessionInfo());
    std::string drv, frames;
    for (auto& kv : h.hints) {
        if (kv.first == "SDL_AUDIO_DRIVER") drv = kv.second;
        if (kv.first == "SDL_AUDIO_DEVICE_SAMPLE_FRAMES") frames = kv.second;
    }
    CHECK_EQ(drv, std::string("pipewire,pulseaudio,alsa"));
    CHECK_EQ(frames, std::string("512"));
    bool pwlat = false;
    for (auto& kv : h.env) pwlat |= kv.first == "PIPEWIRE_LATENCY" && kv.second == "512/48000";
    CHECK(pwlat);

    AudioOptions nul;
    nul.null_device = true;
    HintSet hn = plan_audio(a, nul, SessionInfo());
    bool dummy = false;
    for (auto& kv : hn.hints) dummy |= kv.first == "SDL_AUDIO_DRIVER" && kv.second == "dummy";
    CHECK(dummy);

    MemFs empty;
    AudioAvailability none = detect_audio(MapEnv(), empty, XdgDirs());
    SessionInfo fp;
    fp.flatpak = true;
    HintSet hf = plan_audio(none, AudioOptions(), fp);
    CHECK(!hf.warnings.empty() && hf.warnings[0].find("Flatpak") != std::string::npos);
}

TEST(video_policy_gamescope_never_switches_modes) {
    SessionInfo s;
    s.display_server = DisplayServer::X11;
    s.gamescope = true;
    VideoOptions o;
    o.requested = FullscreenMode::Exclusive;
    VideoPlan p = plan_video(o, s);
    CHECK(!p.allow_video_mode_change);
    CHECK(p.fullscreen == FullscreenMode::Borderless);

    SessionInfo x11;
    x11.display_server = DisplayServer::X11;
    CHECK(plan_video(o, x11).allow_video_mode_change);  // plain X11 desktop may switch modes

    SessionInfo wl;
    wl.display_server = DisplayServer::Wayland;
    wl.wayland_session = true;
    VideoPlan w = plan_video(o, wl);
    CHECK(!w.allow_video_mode_change && w.fullscreen == FullscreenMode::Borderless);

    VideoOptions hl;
    hl.headless = true;
    VideoPlan hp = plan_video(hl, wl);
    bool off = false;
    for (auto& kv : hp.hints.hints) off |= kv.first == "SDL_VIDEO_DRIVER" && kv.second == "offscreen";
    CHECK(off);
}

TEST(input_devices_and_advice) {
    MemFs fs;
    fixtures::steam_deck(fs);
    auto devs = read_input_devices(fs);
    int pads = 0;
    bool deck = false, virt = false;
    for (auto& d : devs) {
        pads += d.is_gamepad;
        deck |= d.steam_deck;
        virt |= d.steam_virtual;
    }
    CHECK_EQ(pads, 2);
    CHECK(deck && virt);
    SessionInfo s;
    s.steam_client = true;
    HintSet h = plan_input(devs, s);
    bool hidapi = false;
    for (auto& kv : h.hints) hidapi |= kv.first == "SDL_JOYSTICK_HIDAPI_STEAMDECK";
    CHECK(!hidapi);  // behind Steam Input: no direct access
    s.steam_client = false;
    s.steam_deck = true;
    hidapi = false;
    for (auto& kv : plan_input(devs, s).hints) hidapi |= kv.first == "SDL_JOYSTICK_HIDAPI_STEAMDECK";
    CHECK(hidapi);
    InputAdvice adv = check_input_setup(fs);
    CHECK(adv.steam_rules_present);
}

TEST(machine_info_and_wsl) {
    MemFs fs;
    fixtures::steam_deck(fs);
    MachineInfo m = read_machine_info(fs);
    CHECK_EQ(m.dmi_product, std::string("Jupiter"));
    CHECK_EQ(m.os.id, std::string("steamos"));
    CHECK(!m.is_wsl);
    CHECK_EQ(m.topo.cpus.size(), size_t(8));
    CHECK_EQ(m.mem.total_kb, uint64_t(16384000));
}
