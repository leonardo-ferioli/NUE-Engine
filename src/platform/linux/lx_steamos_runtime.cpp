#include "lx_steamos_runtime.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

extern char** environ;

namespace nue::lx {

// ---------------------------------------------------------------------------- suspend / resume

bool SuspendWatcher::start(std::function<void(bool)> callback) {
    stop();
    cb_ = std::move(callback);
    error_.clear();
    stop_ = false;
    // Probe synchronously so the caller learns about a missing logind before a thread is created.
    {
        DBusConn probe;
        if (!probe.open(BusType::System)) {
            error_ = probe.last_error();
            return false;
        }
        if (!probe.name_has_owner("org.freedesktop.login1")) {
            error_ = "systemd-logind is not running";
            return false;
        }
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void SuspendWatcher::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    drop_lock();
}

void SuspendWatcher::take_lock(DBusConn& bus) {
    if (lock_fd_.load() >= 0) return;
    std::vector<DBusValue> r;
    if (bus.call("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
                 {DBusArg::str("sleep"), DBusArg::str("NUE"), DBusArg::str("Pause the game and silence audio before suspend"), DBusArg::str("delay")}, &r, 1500) &&
        !r.empty() && r[0].type == 'h' && r[0].i >= 0)
        lock_fd_ = static_cast<int>(r[0].i);
}

void SuspendWatcher::drop_lock() {
    int fd = lock_fd_.exchange(-1);
    if (fd >= 0) ::close(fd);  // closing the descriptor releases the inhibitor lock
}

void SuspendWatcher::run() {
    DBusConn bus;
    if (!bus.open(BusType::System) ||
        !bus.add_match("type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'")) {
        error_ = bus.last_error();
        running_ = false;
        return;
    }
    take_lock(bus);
    while (!stop_.load()) {
        DBusConn::DBusSignal sig;
        int r = bus.wait_signal(500, sig);
        if (r < 0) {
            error_ = "system bus connection lost";
            break;
        }
        if (r == 0 || sig.member != "PrepareForSleep" || sig.args.empty() || sig.args[0].type != 'b') continue;
        const bool sleeping = sig.args[0].u != 0;
        if (cb_) cb_(sleeping);
        if (sleeping) drop_lock();  // let the system continue to suspend now that the game is quiet
        else take_lock(bus);
    }
    running_ = false;
}

// ---------------------------------------------------------------------------- steamos-manager

SteamOsManagerInfo query_steamos_manager() {
    SteamOsManagerInfo info;
    DBusConn bus;
    if (!bus.open(BusType::System)) {
        info.error = bus.last_error();
        return info;
    }
    const std::string dest = "com.steampowered.SteamOSManager1", path = "/com/steampowered/SteamOSManager1";
    if (!bus.name_has_owner(dest)) {
        info.error = "steamos-manager is not running (not SteamOS, or an older release)";
        return info;
    }
    std::vector<DBusValue> r;
    if (!bus.call(dest, path, "org.freedesktop.DBus.Introspectable", "Introspect", {}, &r, 1500) || r.empty() || r[0].type != 's') {
        info.error = bus.last_error();
        return info;
    }
    info.available = true;
    info.interfaces = parse_introspection_xml(r[0].s);
    for (const auto& i : info.interfaces) {
        for (const auto& p : i.properties) {
            if (!is_steamos_manager_property_of_interest(p.first)) continue;
            DBusValue v;
            if (!bus.get_property(dest, path, i.name, p.first, v)) continue;
            std::string text;
            switch (v.type) {
                case 's': case 'o': text = v.s; break;
                case 'u': case 't': case 'b': text = std::to_string(v.u); break;
                case 'i': case 'x': text = std::to_string(v.i); break;
                case 'd': text = std::to_string(v.d); break;
                default: text = std::string("<") + v.type + ">"; break;
            }
            info.values[i.name + "." + p.first] = text;
        }
    }
    return info;
}

// ---------------------------------------------------------------------------- spawn

bool spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, argv[0].c_str(), nullptr, nullptr, args.data(), environ) != 0) return false;
    std::thread([pid] {
        int status = 0;
        ::waitpid(pid, &status, 0);  // reap the child so it never stays a zombie
    }).detach();
    return true;
}

// ---------------------------------------------------------------------------- Steamworks (optional)

namespace {
using bool_fn = bool (*)();
using void_fn = void (*)();
using init_flat_fn = int (*)(char*);  // ESteamAPIInitResult, k_ESteamAPIInitResult_OK == 0
}  // namespace

bool SteamworksLite::load(const std::vector<std::string>& extra_dirs) {
    if (lib_.is_open()) return true;
    for (const auto& d : extra_dirs) {
        std::string full = d + "/libsteam_api.so";
        if (lib_.open(full.c_str())) return true;
    }
    return lib_.open("libsteam_api.so");
}

bool SteamworksLite::steam_running() const {
    auto fn = lib_.fn<bool_fn>("SteamAPI_IsSteamRunning");
    return fn && fn();
}

bool SteamworksLite::init(std::string& error) {
    if (initialized_) return true;
    if (auto flat = lib_.fn<init_flat_fn>("SteamAPI_InitFlat")) {
        char msg[1024] = {};
        if (flat(msg) == 0) {
            initialized_ = true;
            return true;
        }
        error = msg[0] ? msg : "SteamAPI_InitFlat failed";
        return false;
    }
    if (auto legacy = lib_.fn<bool_fn>("SteamAPI_Init")) {
        initialized_ = legacy();
        if (!initialized_) error = "SteamAPI_Init failed";
        return initialized_;
    }
    error = "libsteam_api.so has no SteamAPI_InitFlat / SteamAPI_Init";
    return false;
}

void SteamworksLite::run_callbacks() const {
    if (!initialized_) return;
    if (auto fn = lib_.fn<void_fn>("SteamAPI_RunCallbacks")) fn();
}

void SteamworksLite::shutdown() {
    if (initialized_)
        if (auto fn = lib_.fn<void_fn>("SteamAPI_Shutdown")) fn();
    initialized_ = false;
    lib_.close();
}

}  // namespace nue::lx
