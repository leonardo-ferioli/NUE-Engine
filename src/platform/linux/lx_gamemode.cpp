#include "lx_gamemode.hpp"

#include <unistd.h>

namespace nue::lx {

// ---------------------------------------------------------------------------- GameMode

namespace {
using gm_int_fn = int (*)();
using gm_str_fn = const char* (*)();
}  // namespace

bool GameModeClient::available() {
    if (lib_.is_open()) return true;
    if (lib_.open("libgamemode.so.0", "libgamemode.so")) return true;
    if (!bus_.is_open() && !bus_.open(BusType::Session)) return false;
    return bus_.name_has_owner("com.feralinteractive.GameMode");
}

bool GameModeClient::start() {
    if (active_) return true;
    error_.clear();
    if (lib_.is_open() || lib_.open("libgamemode.so.0", "libgamemode.so")) {
        auto start_fn = lib_.fn<gm_int_fn>("gamemode_request_start");
        auto err_fn = lib_.fn<gm_str_fn>("gamemode_error_string");
        if (start_fn) {
            if (start_fn() == 0) {
                active_ = true;
                backend_ = "libgamemode";
                return true;
            }
            error_ = err_fn ? err_fn() : "gamemode_request_start failed";
        }
    }
    // Direct D-Bus fallback: com.feralinteractive.GameMode.RegisterGame(pid)
    if (bus_.is_open() || bus_.open(BusType::Session)) {
        std::vector<DBusValue> r;
        if (bus_.call("com.feralinteractive.GameMode", "/com/feralinteractive/GameMode", "com.feralinteractive.GameMode", "RegisterGame",
                      {DBusArg::i32(static_cast<int32_t>(getpid()))}, &r, 800) && !r.empty() && r[0].i == 0) {
            active_ = true;
            backend_ = "dbus";
            error_.clear();
            return true;
        }
        if (error_.empty()) error_ = bus_.last_error();
    } else if (error_.empty()) {
        error_ = "neither libgamemode nor the session bus is available";
    }
    return false;
}

void GameModeClient::stop() {
    if (!active_) return;
    if (backend_ == "libgamemode") {
        if (auto end_fn = lib_.fn<gm_int_fn>("gamemode_request_end")) end_fn();
    } else if (backend_ == "dbus") {
        std::vector<DBusValue> r;
        bus_.call("com.feralinteractive.GameMode", "/com/feralinteractive/GameMode", "com.feralinteractive.GameMode", "UnregisterGame",
                  {DBusArg::i32(static_cast<int32_t>(getpid()))}, &r, 800);
    }
    active_ = false;
}

int GameModeClient::status() {
    if (backend_ == "libgamemode")
        if (auto q = lib_.fn<gm_int_fn>("gamemode_query_status")) return q();
    if (backend_ == "dbus" || (backend_.empty() && bus_.is_open())) {
        std::vector<DBusValue> r;
        if (bus_.call("com.feralinteractive.GameMode", "/com/feralinteractive/GameMode", "com.feralinteractive.GameMode", "QueryStatus",
                      {DBusArg::i32(static_cast<int32_t>(getpid()))}, &r, 800) && !r.empty())
            return static_cast<int>(r[0].i);
    }
    return -1;
}

// ---------------------------------------------------------------------------- power profiles

const char* profile_name(PowerProfile p) {
    switch (p) {
        case PowerProfile::PowerSaver: return "power-saver";
        case PowerProfile::Performance: return "performance";
        default: return "balanced";
    }
}

bool PowerProfileHold::available() {
    if (!dest_.empty()) return true;
    if (!bus_.is_open() && !bus_.open(BusType::System)) {
        error_ = bus_.last_error();
        return false;
    }
    // power-profiles-daemon >= 0.20 exports both names; prefer the new one.
    if (bus_.name_has_owner("org.freedesktop.UPower.PowerProfiles")) {
        dest_ = "org.freedesktop.UPower.PowerProfiles";
        path_ = "/org/freedesktop/UPower/PowerProfiles";
        iface_ = "org.freedesktop.UPower.PowerProfiles";
        return true;
    }
    if (bus_.name_has_owner("net.hadess.PowerProfiles")) {
        dest_ = "net.hadess.PowerProfiles";
        path_ = "/net/hadess/PowerProfiles";
        iface_ = "net.hadess.PowerProfiles";
        return true;
    }
    error_ = "power-profiles-daemon is not running";
    return false;
}

bool PowerProfileHold::call(const std::string& method, const std::vector<DBusArg>& args, std::vector<DBusValue>* r) {
    if (!bus_.call(dest_, path_, iface_, method, args, r, 1200)) {
        error_ = bus_.last_error();
        return false;
    }
    return true;
}

bool PowerProfileHold::hold(PowerProfile p, const std::string& reason, const std::string& app_id) {
    if (cookie_) return true;
    if (!available()) return false;
    std::vector<DBusValue> r;
    if (!call("HoldProfile", {DBusArg::str(profile_name(p)), DBusArg::str(reason), DBusArg::str(app_id)}, &r) || r.empty() || r[0].type != 'u') return false;
    cookie_ = static_cast<uint32_t>(r[0].u);
    return cookie_ != 0;
}

void PowerProfileHold::release() {
    if (!cookie_) return;
    std::vector<DBusValue> r;
    call("ReleaseProfile", {DBusArg::u32(cookie_)}, &r);
    cookie_ = 0;
}

std::string PowerProfileHold::active_profile() {
    if (!available()) return std::string();
    DBusValue v;
    if (!bus_.get_property(dest_, path_, iface_, "ActiveProfile", v) || v.type != 's') return std::string();
    return v.s;
}

// ---------------------------------------------------------------------------- idle inhibition

bool IdleInhibitor::acquire(const std::string& app, const std::string& reason) {
    if (cookie_) return true;
    if (!bus_.is_open() && !bus_.open(BusType::Session)) {
        error_ = bus_.last_error();
        return false;
    }
    std::vector<DBusValue> r;
    if (!bus_.call("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "Inhibit",
                   {DBusArg::str(app), DBusArg::str(reason)}, &r, 1000) || r.empty() || r[0].type != 'u') {
        error_ = bus_.last_error();
        return false;
    }
    cookie_ = static_cast<uint32_t>(r[0].u);
    return true;
}

void IdleInhibitor::release() {
    if (!cookie_) return;
    std::vector<DBusValue> r;
    bus_.call("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "UnInhibit",
              {DBusArg::u32(cookie_)}, &r, 1000);
    cookie_ = 0;
}

// ---------------------------------------------------------------------------- rtkit

bool RtKit::available() {
    if (!bus_.is_open() && !bus_.open(BusType::System)) {
        error_ = bus_.last_error();
        return false;
    }
    return bus_.name_has_owner("org.freedesktop.RealtimeKit1");
}

bool RtKit::make_realtime(uint64_t tid, uint32_t priority) {
    if (!available()) return false;
    DBusValue max;
    if (bus_.get_property("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", "MaxRealtimePriority", max) && max.type == 'i' && max.i > 0 &&
        priority > static_cast<uint32_t>(max.i))
        priority = static_cast<uint32_t>(max.i);
    std::vector<DBusValue> r;
    if (!bus_.call("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", "MakeThreadRealtime",
                   {DBusArg::u64(tid), DBusArg::u32(priority)}, &r)) {
        error_ = bus_.last_error();
        return false;
    }
    return true;
}

bool RtKit::make_high_priority(uint64_t tid, int nice) {
    if (!available()) return false;
    DBusValue min;
    if (bus_.get_property("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", "MinNiceLevel", min) && min.type == 'i' && nice < min.i)
        nice = static_cast<int>(min.i);
    std::vector<DBusValue> r;
    if (!bus_.call("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", "MakeThreadHighPriority",
                   {DBusArg::u64(tid), DBusArg::i32(nice)}, &r)) {
        error_ = bus_.last_error();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------- session

GameModeState GameModeSession::enter(const GameModeOptions& o) {
    if (entered_) return state_;
    entered_ = true;
    state_ = GameModeState();
    if (o.use_gamemode) {
        if (gm_.start()) {
            state_.gamemode = true;
            state_.messages.push_back("GameMode active via " + gm_.backend());
        } else {
            state_.messages.push_back("GameMode unavailable: " + gm_.error());
        }
    }
    if (o.use_power_profile) {
        PowerProfile p = o.battery_saver ? PowerProfile::PowerSaver : PowerProfile::Performance;
        if (ppd_.hold(p, o.reason, o.app_id)) {
            state_.power_profile = true;
            state_.messages.push_back(std::string("power profile held: ") + profile_name(p));
        } else {
            state_.messages.push_back("power-profiles-daemon: " + ppd_.error());
        }
    }
    if (o.inhibit_idle) {
        if (idle_.acquire(o.app_id, o.reason)) {
            state_.idle_inhibited = true;
        } else {
            state_.messages.push_back("org.freedesktop.ScreenSaver inhibit failed: " + idle_.error() + " (SDL's own inhibition still applies)");
        }
    }
    return state_;
}

void GameModeSession::leave() {
    if (!entered_) return;
    idle_.release();
    ppd_.release();
    gm_.stop();
    state_ = GameModeState();
    entered_ = false;
}

}  // namespace nue::lx
