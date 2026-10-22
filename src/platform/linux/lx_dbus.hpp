// Minimal D-Bus client on top of libdbus-1 loaded at run time (no headers, no link dependency).
// Enough for blocking method calls with basic-type arguments and replies: GameMode, power-profiles-
// daemon, org.freedesktop.ScreenSaver, RealtimeKit and property reads.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lx_dl.hpp"

namespace nue::lx {

enum class BusType { Session = 0, System = 1 };

struct DBusArg {
    char type = 's';  // 's' 'o' string/object path, 'u' uint32, 'i' int32, 'x' int64, 't' uint64, 'b' bool
    std::string s;
    uint64_t u = 0;  // used for u, t, b (0/1)
    int64_t i = 0;   // used for i, x
    static DBusArg str(std::string v) { DBusArg a; a.type = 's'; a.s = std::move(v); return a; }
    static DBusArg u32(uint32_t v) { DBusArg a; a.type = 'u'; a.u = v; return a; }
    static DBusArg i32(int32_t v) { DBusArg a; a.type = 'i'; a.i = v; return a; }
    static DBusArg u64(uint64_t v) { DBusArg a; a.type = 't'; a.u = v; return a; }
};

struct DBusValue {
    char type = 0;  // basic type code, or the container code ('a', 'r', 'e') when skipped
    std::string s;
    uint64_t u = 0;
    int64_t i = 0;
    double d = 0;
};

class DBusConn {
public:
    DBusConn() = default;
    ~DBusConn() { close(); }
    DBusConn(const DBusConn&) = delete;
    DBusConn& operator=(const DBusConn&) = delete;

    // Opens a private connection (does not share state with the host application's libdbus use).
    bool open(BusType bus);
    void close();
    bool is_open() const { return conn_ != nullptr; }

    // Blocking call. Variants in the reply are unwrapped (up to 4 levels); arrays and structs are
    // reported as a single value with type 'a' / 'r' and skipped.
    bool call(const std::string& dest, const std::string& path, const std::string& iface, const std::string& method,
              const std::vector<DBusArg>& args, std::vector<DBusValue>* reply, int timeout_ms = 1500);
    // org.freedesktop.DBus.Properties.Get
    bool get_property(const std::string& dest, const std::string& path, const std::string& iface, const std::string& prop,
                      DBusValue& out, int timeout_ms = 1500);
    bool name_has_owner(const std::string& name);
    const std::string& last_error() const { return error_; }

private:
    DynLib lib_;
    void* conn_ = nullptr;
    std::string error_;
    struct Api;
    Api* api_ = nullptr;
};

}  // namespace nue::lx
