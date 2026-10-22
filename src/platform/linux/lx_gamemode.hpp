// "Game mode" on Linux: everything that asks the system to favour the running game.
//   - Feral GameMode (libgamemode.so.0 via dlopen, D-Bus fallback to gamemoded)
//   - power-profiles-daemon: hold the "performance" profile (net.hadess.PowerProfiles /
//     org.freedesktop.UPower.PowerProfiles), "power-saver" while on battery if requested
//   - screen saver / idle inhibition outside SDL: org.freedesktop.ScreenSaver
//   - RealtimeKit (rtkit) for unprivileged real-time / high-priority threads
// Every service is optional and independent; a missing one reports why and the rest still work.
// Calls block on D-Bus for at most ~1.5 s each: start()/stop() belong on a worker thread, not on
// the render thread.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lx_dbus.hpp"
#include "lx_dl.hpp"

namespace nue::lx {

class GameModeClient {
public:
    ~GameModeClient() { stop(); }
    // True if libgamemode is loadable or gamemoded owns its bus name.
    bool available();
    bool start();   // registers this process; idempotent
    void stop();    // unregisters; idempotent
    bool active() const { return active_; }
    // 0 inactive, 1 active (other clients), 2 active and this process registered, <0 error.
    int status();
    const std::string& backend() const { return backend_; }  // "libgamemode", "dbus" or ""
    const std::string& error() const { return error_; }

private:
    DynLib lib_;
    DBusConn bus_;
    bool active_ = false;
    std::string backend_, error_;
};

enum class PowerProfile { PowerSaver, Balanced, Performance };
const char* profile_name(PowerProfile p);

class PowerProfileHold {
public:
    ~PowerProfileHold() { release(); }
    bool available();
    bool hold(PowerProfile p, const std::string& reason, const std::string& app_id);
    void release();
    bool held() const { return cookie_ != 0; }
    // Active profile as reported by the daemon, empty if unavailable.
    std::string active_profile();
    const std::string& error() const { return error_; }

private:
    bool call(const std::string& method, const std::vector<DBusArg>& args, std::vector<DBusValue>* r);
    DBusConn bus_;
    std::string dest_, path_, iface_, error_;
    uint32_t cookie_ = 0;
};

// Inhibit(app, reason) -> cookie / UnInhibit(cookie) services with the same shape:
//   screen saver  org.freedesktop.ScreenSaver at /org/freedesktop/ScreenSaver   (display / idle)
//   power manager org.freedesktop.PowerManagement.Inhibit at /org/freedesktop/PowerManagement/Inhibit  (system sleep)
class IdleInhibitor {
public:
    enum class Kind { ScreenSaver, PowerManagement };
    explicit IdleInhibitor(Kind k = Kind::ScreenSaver) : kind_(k) {}
    ~IdleInhibitor() { release(); }
    bool acquire(const std::string& app, const std::string& reason);
    void release();
    bool held() const { return cookie_ != 0; }
    const std::string& error() const { return error_; }

private:
    Kind kind_;
    DBusConn bus_;
    uint32_t cookie_ = 0;
    std::string error_;
};

class RtKit {
public:
    // Asks rtkit to make `tid` real-time (SCHED_RR) at `priority` (1..max); clamps to the daemon's maximum.
    bool make_realtime(uint64_t tid, uint32_t priority);
    bool make_high_priority(uint64_t tid, int nice);  // negative nice, clamped to MinNiceLevel
    bool available();
    const std::string& error() const { return error_; }

private:
    DBusConn bus_;
    std::string error_;
};

// Enters/leaves "game mode" as one switch: GameMode + performance profile + idle inhibition.
struct GameModeOptions {
    bool use_gamemode = true;
    bool use_power_profile = true;
    bool inhibit_idle = true;
    bool battery_saver = false;  // hold power-saver instead of performance
    std::string app_id = "nue";
    std::string reason = "Running a game";
};
struct GameModeState {
    bool gamemode = false, power_profile = false, idle_inhibited = false;
    std::vector<std::string> messages;  // why something was not enabled
};

class GameModeSession {
public:
    ~GameModeSession() { leave(); }
    GameModeState enter(const GameModeOptions& o);
    void leave();
    const GameModeState& state() const { return state_; }

private:
    GameModeClient gm_;
    PowerProfileHold ppd_;
    IdleInhibitor idle_;
    GameModeState state_;
    bool entered_ = false;
};

}  // namespace nue::lx
