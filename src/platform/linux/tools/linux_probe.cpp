// linux_probe: prints what the machine (or a captured machine tree) supports, using exactly the
// code the engine uses to decide its settings. Opens no window and no audio device.
//
//   linux_probe                     probe this machine
//   linux_probe --live              also load libvulkan/libEGL/... and enumerate GPUs (Linux)
//   linux_probe --root DIR          read /proc, /sys, /etc from DIR instead of "/" (fixtures)
//   linux_probe --env NAME=VALUE    with --root: define an environment variable (repeatable)
//   linux_probe --headless          plan for headless operation (CI)
#include <cstdio>
#include <cstring>
#include <string>

#include "lx_fs.hpp"
#include "lx_gpu.hpp"
#include "lx_pathcase.hpp"
#include "lx_policy.hpp"
#include "lx_session.hpp"
#include "lx_sysinfo.hpp"
#include "lx_text.hpp"
#include "lx_xdg.hpp"

#if defined(NUE_LINUX_RUNTIME)
#include "lx_runtime.hpp"
#endif

using namespace nue::lx;

namespace {

void section(const char* t) { std::printf("\n== %s ==\n", t); }
void kv(const char* k, const std::string& v) { std::printf("  %-22s %s\n", k, v.c_str()); }
const char* yn(bool b) { return b ? "yes" : "no"; }
std::string cpus(const std::vector<int>& v) { return v.empty() ? "-" : format_cpulist(v); }
void list(const char* label, const std::vector<std::string>& v) {
    for (const auto& s : v) std::printf("  %s: %s\n", label, s.c_str());
}
std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%04x", v);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    std::string root;
    bool live = false, headless = false;
    MapEnv fixture_env;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--live")) live = true;
        else if (!std::strcmp(argv[i], "--headless")) headless = true;
        else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!std::strcmp(argv[i], "--env") && i + 1 < argc) {
            std::string a = argv[++i];
            size_t eq = a.find('=');
            if (eq != std::string::npos) fixture_env.vars[a.substr(0, eq)] = a.substr(eq + 1);
        } else {
            std::printf("usage: linux_probe [--live] [--headless] [--root DIR [--env NAME=VALUE]...]\n");
            return std::strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }
    StdFs fs(root);
    ProcessEnv process_env;
    const IEnv& env = root.empty() ? static_cast<const IEnv&>(process_env) : static_cast<const IEnv&>(fixture_env);

    std::printf("NUE Linux platform probe (%s)\n", root.empty() ? "this machine" : ("fixture " + root).c_str());

    MachineInfo m = read_machine_info(fs);
    section("machine");
    kv("distro", m.os.pretty_name.empty() ? m.os.id : m.os.pretty_name);
    kv("kernel", m.kernel_release);
    kv("probe built for", m.arch);
    kv("cpu", m.cpu.model_name + (m.cpu.vendor.empty() ? "" : " [" + m.cpu.vendor + "]"));
    kv("logical / physical", std::to_string(m.topo.cpus.size()) + " / " + std::to_string(m.topo.physical_core_count()));
    kv("memory", std::to_string(m.mem.total_kb / 1024) + " MiB total, " + std::to_string(m.mem.available_kb / 1024) + " MiB available");
    kv("board", m.dmi_vendor + " " + m.dmi_product);
    kv("wsl / container", std::string(yn(m.is_wsl)) + " / " + yn(m.in_container));

    section("cpu topology");
    kv("hybrid", std::string(yn(m.topo.hybrid)) + (m.topo.hybrid ? " (" + m.topo.hybrid_source + ")" : ""));
    kv("governor / driver", m.topo.governor + " / " + m.topo.scaling_driver + (m.topo.epp.empty() ? "" : " / epp " + m.topo.epp));
    AffinityPlan ap = plan_affinity(m.topo);
    kv("pinning recommended", yn(ap.use_pinning));
    kv("main / render", cpus(ap.main_thread) + " / " + cpus(ap.render_thread));
    kv("workers", cpus(ap.workers));
    kv("background", cpus(ap.background));

    section("power and thermal");
    PowerInfo pw = read_power(fs);
    kv("on battery", yn(pw.on_battery));
    for (const auto& b : pw.batteries)
        kv(b.name.c_str(), std::to_string(b.percent) + "%" + (b.charging ? " charging" : "") + (b.seconds_remaining >= 0 ? ", " + std::to_string(b.seconds_remaining / 60) + " min" : ""));
    auto th = read_thermal(fs);
    kv("hottest sensor", th.empty() ? "n/a" : std::to_string(static_cast<int>(max_celsius(th))) + " C (" + std::to_string(th.size()) + " sensors)");

    SessionInfo ses = detect_session(env, fs);
    section("session");
    kv("display server", to_string(ses.display_server));
    kv("session / desktop", ses.session_type + " / " + ses.desktop);
    kv("steam deck / steamos", std::string(yn(ses.steam_deck)) + (ses.deck_model.empty() ? "" : " (" + ses.deck_model + ")") + " / " + yn(ses.steamos));
    kv("gamescope", std::string(yn(ses.gamescope)) + (ses.gamescope ? " (" + ses.gamescope_reason + ")" : ""));
    kv("steam client / runtime", std::string(yn(ses.steam_client)) + " / " + (ses.steam_runtime ? ses.steam_runtime_name : "no"));
    kv("flatpak / snap / appimage", std::string(yn(ses.flatpak)) + " / " + yn(ses.snap) + " / " + yn(ses.appimage));
    kv("headless", yn(ses.headless));
    list("note", ses.notes);

    XdgDirs xdg = resolve_xdg(env, fs, env.get_or("HOME"));
    AppDirs app = app_dirs(xdg, "nue/probe");
    section("directories");
    kv("data", app.data);
    kv("config", app.config);
    kv("cache", app.cache);
    kv("state", app.state);
    kv("runtime dir", xdg.runtime_dir.empty() ? "(unset)" : xdg.runtime_dir);

    section("graphics");
    GraphicsProbe gp;
    gp.drm = read_drm_devices(fs);
    gp.icds = read_vulkan_icds(env, fs, xdg);
    if (live) {
        gp.vk = probe_vulkan();
        gp.gl = probe_gl_libraries();
    }
    for (const auto& d : gp.drm)
        kv(d.card.c_str(), std::string(vendor_name(d.vendor)) + " " + hex(d.vendor) + ":" + hex(d.device) + " driver=" + d.kernel_driver + (d.boot_vga ? " boot" : "") + (d.platform ? " soc" : ""));
    for (const auto& i : gp.icds) kv("vulkan icd", i.driver + " -> " + i.library_path + (i.software ? " (software)" : ""));
    if (live) {
        kv("vulkan loader", yn(gp.vk.loader_found));
        for (const auto& d : gp.vk.devices) kv("vulkan device", d.name + " type=" + std::to_string(d.type));
        if (!gp.vk.error.empty()) kv("vulkan error", gp.vk.error);
        kv("egl/gles2/gl/opengl", std::string(yn(gp.gl.egl)) + "/" + yn(gp.gl.gles2) + "/" + yn(gp.gl.gl) + "/" + yn(gp.gl.opengl));
    } else {
        kv("live probe", "skipped (use --live)");
    }
    RendererPlan rp = plan_renderer(gp, PlanRequest(), ses);
    kv("renderer plan", std::string(bgfx_renderer_name(rp.api)) + (rp.device_name.empty() ? "" : " on " + rp.device_name));
    for (auto a : rp.fallbacks) kv("fallback", bgfx_renderer_name(a));
    for (const auto& e : rp.env) kv("set env", e.first + "=" + e.second);
    list("note", rp.notes);
    list("warning", rp.warnings);

    section("audio / video / input policy");
    AudioAvailability av = detect_audio(env, fs, xdg);
    kv("pipewire/pulse/alsa", std::string(yn(av.pipewire_socket)) + "/" + yn(av.pulse_socket) + "/" + yn(av.alsa_devices));
    AudioOptions ao;
    HintSet ah = plan_audio(av, ao, ses);
    for (const auto& h : ah.hints) kv(h.first.c_str(), h.second);
    list("warning", ah.warnings);
    VideoOptions vo;
    vo.headless = headless;
    VideoPlan vp = plan_video(vo, ses);
    for (const auto& h : vp.hints.hints) kv(h.first.c_str(), h.second);
    kv("fullscreen", vp.fullscreen == FullscreenMode::Borderless ? "borderless" : vp.fullscreen == FullscreenMode::Exclusive ? "exclusive" : "windowed");
    kv("video mode change", yn(vp.allow_video_mode_change));
    list("note", vp.hints.notes);
    auto devs = read_input_devices(fs);
    for (const auto& d : devs)
        if (d.is_gamepad) kv("gamepad", d.name + " " + hex(d.vendor) + ":" + hex(d.product) + (d.steam_virtual ? " (Steam virtual)" : ""));
    InputAdvice ia = check_input_setup(fs);
    list("input", ia.notes);

#if defined(NUE_LINUX_RUNTIME)
    if (root.empty()) {
        section("runtime services");
        print_runtime_capabilities();
    }
#endif
    std::printf("\n");
    return 0;
}
