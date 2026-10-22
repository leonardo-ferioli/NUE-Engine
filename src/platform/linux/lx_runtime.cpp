#include "lx_runtime.hpp"

#include <time.h>
#include <unistd.h>

#include <cstdio>

#include "lx_dbus.hpp"
#include "lx_diag.hpp"
#include "lx_dl.hpp"
#include "lx_fs.hpp"
#include "lx_gamemode.hpp"
#include "lx_sched.hpp"
#include "lx_storage.hpp"

namespace nue::lx {

RuntimeCaps query_runtime_caps() {
    RuntimeCaps c;
    c.libraries = {
        {"Vulkan loader", "libvulkan.so.1", "rendering (bgfx Vulkan backend)"},
        {"EGL", "libEGL.so.1", "GL ES / Wayland GL fallback"},
        {"OpenGL", "libGL.so.1", "desktop GL fallback"},
        {"libdbus", "libdbus-1.so.3", "GameMode fallback, power profiles, idle inhibit, rtkit"},
        {"GameMode", "libgamemode.so.0", "Feral GameMode"},
        {"PipeWire", "libpipewire-0.3.so.0", "audio (through SDL3)"},
        {"PulseAudio", "libpulse.so.0", "audio (through SDL3)"},
        {"ALSA", "libasound.so.2", "audio (through SDL3)"},
        {"libudev", "libudev.so.1", "controller hot-plug (through SDL3)"},
        {"libdecor", "libdecor-0.so.0", "Wayland window decorations (through SDL3)"},
        {"Wayland client", "libwayland-client.so.0", "Wayland video (through SDL3)"},
        {"X11", "libX11.so.6", "X11 video (through SDL3)"},
        {"XInput2", "libXi.so.6", "X11 raw mouse (through SDL3)"},
    };
    for (auto& l : c.libraries) {
        DynLib d;
        l.present = d.open(l.soname);
    }
    {
        DBusConn s;
        c.dbus_session = s.open(BusType::Session);
        if (c.dbus_session) c.screensaver_service = s.name_has_owner("org.freedesktop.ScreenSaver");
        if (c.dbus_session && !c.screensaver_service) c.notes.push_back("no org.freedesktop.ScreenSaver owner; SDL inhibition (portal / X11 / Wayland protocol) still works");
    }
    {
        DBusConn s;
        c.dbus_system = s.open(BusType::System);
    }
    GameModeClient gm;
    for (auto& l : c.libraries)
        if (std::string(l.soname) == "libgamemode.so.0") c.gamemode_lib = l.present;
    c.gamemode_daemon = gm.available();
    PowerProfileHold ppd;
    c.power_profiles = ppd.available();
    RtKit rk;
    c.rtkit = rk.available();
    {
        AsyncReader ar;
        AsyncReaderOptions o;
        o.prefer_io_uring = true;
        c.io_uring = ar.start(o) && std::string(ar.backend()) == "io_uring";
        ar.stop();
    }
    PriorityLimits lim = query_priority_limits();
    c.max_rt_priority = lim.max_rt_priority;
    c.min_nice = lim.min_nice;
    PeriodicTimer t;
    c.timerfd = t.start(1000000);
    t.close();
    struct timespec res;
    if (clock_getres(CLOCK_MONOTONIC, &res) == 0) c.monotonic_resolution_ns = res.tv_sec * 1000000000L + res.tv_nsec;
    c.core_limit = current_core_limit();
    return c;
}

void print_runtime_capabilities() {
    RuntimeCaps c = query_runtime_caps();
    auto yn = [](bool b) { return b ? "yes" : "no"; };
    std::printf("  optional libraries (all loaded with dlopen, none is required to start):\n");
    for (const auto& l : c.libraries) std::printf("    %-16s %-3s  %s\n", l.name, yn(l.present), l.used_for);
    std::printf("  D-Bus session / system     %s / %s\n", yn(c.dbus_session), yn(c.dbus_system));
    std::printf("  GameMode lib / daemon      %s / %s\n", yn(c.gamemode_lib), yn(c.gamemode_daemon));
    std::printf("  power-profiles-daemon      %s\n", yn(c.power_profiles));
    std::printf("  rtkit                      %s\n", yn(c.rtkit));
    std::printf("  ScreenSaver D-Bus service  %s\n", yn(c.screensaver_service));
    std::printf("  io_uring                   %s\n", yn(c.io_uring));
    std::printf("  max RT priority / min nice %d / %d\n", c.max_rt_priority, c.min_nice);
    std::printf("  timerfd                    %s (CLOCK_MONOTONIC resolution %ld ns)\n", yn(c.timerfd), c.monotonic_resolution_ns);
    std::printf("  RLIMIT_CORE soft           %s\n", c.core_limit == ~0ull ? "unlimited" : std::to_string(c.core_limit).c_str());
    for (const auto& n : c.notes) std::printf("  note: %s\n", n.c_str());

    StdFs fs;
    CoreDumpInfo cd = describe_core_dumps(fs, c.core_limit);
    for (const auto& h : cd.hints) std::printf("  core dumps: %s\n", h.c_str());
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd)) {
        std::string text;
        fs.read_file("/proc/self/mountinfo", text);
        auto mounts = parse_mountinfo(text);
        if (const MountEntry* m = mount_for_path(mounts, cwd)) {
            std::printf("  working dir file system    %s on %s%s\n", m->fs_type.c_str(), m->mount_point.c_str(), m->read_only ? " (read-only)" : "");
            std::string w = fs_type_warning(m->fs_type);
            if (!w.empty()) std::printf("  warning: %s\n", w.c_str());
        }
    }
}

}  // namespace nue::lx
