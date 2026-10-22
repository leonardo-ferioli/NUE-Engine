#include "lx_report.hpp"

#include <cstdio>

#include "lx_policy.hpp"
#include "lx_sysinfo.hpp"
#include "lx_text.hpp"
#include "lx_xdg.hpp"

namespace nue::lx {

namespace {

struct Out {
    std::string s;
    void section(const char* t) { s += "\n== "; s += t; s += " ==\n"; }
    void kv(const std::string& k, const std::string& v) {
        char head[64];
        std::snprintf(head, sizeof head, "  %-22s ", k.c_str());
        s += head;
        s += v;
        s += '\n';
    }
    void list(const char* label, const std::vector<std::string>& v) {
        for (const auto& x : v) { s += "  "; s += label; s += ": "; s += x; s += '\n'; }
    }
};
const char* yn(bool b) { return b ? "yes" : "no"; }
std::string cpus(const std::vector<int>& v) { return v.empty() ? "-" : format_cpulist(v); }
std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%04x", v);
    return b;
}

}  // namespace

std::string build_system_report(const IFileSystem& fs, const IEnv& env, const ReportOptions& opt, ReportData* data) {
    Out o;
    o.s = "NUE Linux platform report (" + (opt.fixture_label.empty() ? std::string("this machine") : opt.fixture_label) + ")\n";

    MachineInfo m = read_machine_info(fs);
    o.section("machine");
    o.kv("distro", m.os.pretty_name.empty() ? m.os.id : m.os.pretty_name);
    o.kv("kernel", m.kernel_release);
    o.kv("built for", m.arch);
    o.kv("cpu", m.cpu.model_name + (m.cpu.vendor.empty() ? "" : " [" + m.cpu.vendor + "]"));
    o.kv("logical / physical", std::to_string(m.topo.cpus.size()) + " / " + std::to_string(m.topo.physical_core_count()));
    o.kv("memory", std::to_string(m.mem.total_kb / 1024) + " MiB total, " + std::to_string(m.mem.available_kb / 1024) + " MiB available");
    o.kv("board", m.dmi_vendor + " " + m.dmi_product);
    o.kv("wsl / container", std::string(yn(m.is_wsl)) + " / " + yn(m.in_container));

    o.section("cpu topology");
    o.kv("hybrid", std::string(yn(m.topo.hybrid)) + (m.topo.hybrid ? " (" + m.topo.hybrid_source + ")" : ""));
    o.kv("governor / driver", m.topo.governor + " / " + m.topo.scaling_driver + (m.topo.epp.empty() ? "" : " / epp " + m.topo.epp));
    AffinityPlan ap = plan_affinity(m.topo);
    o.kv("pinning recommended", yn(ap.use_pinning));
    o.kv("main / render", cpus(ap.main_thread) + " / " + cpus(ap.render_thread));
    o.kv("workers", cpus(ap.workers));
    o.kv("background", cpus(ap.background));

    o.section("power and thermal");
    PowerInfo pw = read_power(fs);
    o.kv("on battery", yn(pw.on_battery));
    for (const auto& b : pw.batteries)
        o.kv(b.name, std::to_string(b.percent) + "%" + (b.charging ? " charging" : "") + (b.seconds_remaining >= 0 ? ", " + std::to_string(b.seconds_remaining / 60) + " min" : ""));
    auto th = read_thermal(fs);
    o.kv("hottest sensor", th.empty() ? "n/a" : std::to_string(static_cast<int>(max_celsius(th))) + " C (" + std::to_string(th.size()) + " sensors)");

    SessionInfo ses = detect_session(env, fs);
    o.section("session");
    o.kv("display server", to_string(ses.display_server));
    o.kv("session / desktop", ses.session_type + " / " + ses.desktop);
    o.kv("steam deck / steamos", std::string(yn(ses.steam_deck)) + (ses.deck_model.empty() ? "" : " (" + ses.deck_model + ")") + " / " + yn(ses.steamos));
    o.kv("gamescope", std::string(yn(ses.gamescope)) + (ses.gamescope ? " (" + ses.gamescope_reason + ")" : ""));
    o.kv("steam client / runtime", std::string(yn(ses.steam_client)) + " / " + (ses.steam_runtime ? ses.steam_runtime_name : "no"));
    o.kv("flatpak / snap / appimage", std::string(yn(ses.flatpak)) + " / " + yn(ses.snap) + " / " + yn(ses.appimage));
    o.kv("headless", yn(ses.headless));
    o.list("note", ses.notes);

    XdgDirs xdg = resolve_xdg(env, fs, env.get_or("HOME"));
    AppDirs app = app_dirs(xdg, "nue/probe");
    o.section("directories");
    o.kv("data", app.data);
    o.kv("config", app.config);
    o.kv("cache", app.cache);
    o.kv("state", app.state);
    o.kv("runtime dir", xdg.runtime_dir.empty() ? "(unset)" : xdg.runtime_dir);

    o.section("graphics");
    GraphicsProbe gp;
    gp.drm = read_drm_devices(fs);
    gp.icds = read_vulkan_icds(env, fs, xdg);
    if (opt.live_graphics) {
        gp.vk = probe_vulkan();
        gp.gl = probe_gl_libraries();
    }
    for (const auto& d : gp.drm)
        o.kv(d.card, std::string(vendor_name(d.vendor)) + " " + hex(d.vendor) + ":" + hex(d.device) + " driver=" + d.kernel_driver + (d.boot_vga ? " boot" : "") + (d.platform ? " soc" : ""));
    for (const auto& i : gp.icds) o.kv("vulkan icd", i.driver + " -> " + i.library_path + (i.software ? " (software)" : ""));
    if (opt.live_graphics) {
        o.kv("vulkan loader", yn(gp.vk.loader_found));
        for (const auto& d : gp.vk.devices) o.kv("vulkan device", d.name + " type=" + std::to_string(d.type));
        if (!gp.vk.error.empty()) o.kv("vulkan error", gp.vk.error);
        o.kv("egl/gles2/gl/opengl", std::string(yn(gp.gl.egl)) + "/" + yn(gp.gl.gles2) + "/" + yn(gp.gl.gl) + "/" + yn(gp.gl.opengl));
    } else {
        o.kv("live probe", "skipped (use --live)");
    }
    RendererPlan rp = plan_renderer(gp, PlanRequest(), ses);
    o.kv("renderer plan", std::string(bgfx_renderer_name(rp.api)) + (rp.device_name.empty() ? "" : " on " + rp.device_name));
    for (auto a : rp.fallbacks) o.kv("fallback", bgfx_renderer_name(a));
    for (const auto& e : rp.env) o.kv("set env", e.first + "=" + e.second);
    o.list("note", rp.notes);
    o.list("warning", rp.warnings);

    o.section("audio / video / input policy");
    AudioAvailability av = detect_audio(env, fs, xdg);
    o.kv("pipewire/pulse/alsa", std::string(yn(av.pipewire_socket)) + "/" + yn(av.pulse_socket) + "/" + yn(av.alsa_devices));
    HintSet ah = plan_audio(av, AudioOptions(), ses);
    for (const auto& h : ah.hints) o.kv(h.first, h.second);
    o.list("warning", ah.warnings);
    VideoOptions vo;
    vo.headless = opt.headless;
    VideoPlan vp = plan_video(vo, ses);
    for (const auto& h : vp.hints.hints) o.kv(h.first, h.second);
    o.kv("fullscreen", vp.fullscreen == FullscreenMode::Borderless ? "borderless" : vp.fullscreen == FullscreenMode::Exclusive ? "exclusive" : "windowed");
    o.kv("video mode change", yn(vp.allow_video_mode_change));
    o.list("note", vp.hints.notes);
    auto devs = read_input_devices(fs);
    for (const auto& d : devs)
        if (d.is_gamepad) o.kv("gamepad", d.name + " " + hex(d.vendor) + ":" + hex(d.product) + (d.steam_virtual ? " (Steam virtual)" : ""));
    InputAdvice ia = check_input_setup(fs);
    o.list("input", ia.notes);

    if (data) {
        data->session = ses;
        data->graphics = gp;
        data->renderer = rp;
    }
    return o.s;
}

}  // namespace nue::lx
