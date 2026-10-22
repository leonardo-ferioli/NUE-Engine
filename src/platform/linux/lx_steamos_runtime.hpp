// SteamOS services that need the running system (Linux only): suspend / resume notification from systemd-logind,
// read-only steamos-manager queries, launching Steam's on-screen keyboard, and an optional Steamworks shim loaded with
// dlopen (never a link-time dependency).
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "lx_dbus.hpp"
#include "lx_dl.hpp"
#include "lx_steamos.hpp"

namespace nue::lx {

// Watches org.freedesktop.login1 PrepareForSleep. While running it holds a "delay" inhibitor lock so the system waits
// (up to logind's InhibitDelayMaxSec, 5 s by default) until the callback for `sleeping == true` returned; the lock is
// released then, and taken again after resume. The callback runs on the watcher thread.
class SuspendWatcher {
public:
    ~SuspendWatcher() { stop(); }
    bool start(std::function<void(bool sleeping)> callback);
    void stop();
    bool running() const { return running_.load(); }
    bool has_delay_lock() const { return lock_fd_.load() >= 0; }
    const std::string& error() const { return error_; }

private:
    void run();
    void take_lock(DBusConn& bus);
    void drop_lock();
    std::function<void(bool)> cb_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, running_{false};
    std::atomic<int> lock_fd_{-1};
    std::string error_;
};

struct SteamOsManagerInfo {
    bool available = false;
    std::vector<DBusInterfaceInfo> interfaces;
    std::map<std::string, std::string> values;  // "<interface>.<property>" -> text, only whitelisted properties
    std::string error;
};
// Introspects com.steampowered.SteamOSManager1 on the system bus and reads the whitelisted properties. Read only:
// the engine never writes TDP or performance settings (that is the player's choice in the Steam quick access menu).
SteamOsManagerInfo query_steamos_manager();

// fork/exec without a shell; the child is reaped in the background. False if argv is empty or the spawn failed.
bool spawn_detached(const std::vector<std::string>& argv);

// Minimal Steamworks access through libsteam_api.so when the game ships it. Everything is optional.
class SteamworksLite {
public:
    ~SteamworksLite() { shutdown(); }
    // Looks for the library in `extra_dirs` first, then the default search path.
    bool load(const std::vector<std::string>& extra_dirs);
    bool steam_running() const;
    bool init(std::string& error);   // SteamAPI_InitFlat / SteamAPI_Init; needs steam_appid.txt or a Steam launch
    void run_callbacks() const;
    void shutdown();
    bool loaded() const { return lib_.is_open(); }
    bool initialized() const { return initialized_; }

private:
    DynLib lib_;
    bool initialized_ = false;
};

}  // namespace nue::lx
