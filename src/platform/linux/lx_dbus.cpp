#include "lx_dbus.hpp"

#include <unistd.h>

#include <cstring>

namespace nue::lx {

namespace {

// libdbus ABI (stable since 1.0). Structures that the library fills are opaque buffers that are
// larger than any known layout; only the first two pointers of DBusError are read.
struct DBusErrorBuf {
    alignas(8) unsigned char raw[64];
    const char* name() const { return *reinterpret_cast<const char* const*>(raw); }
    const char* message() const { return *reinterpret_cast<const char* const*>(raw + sizeof(void*)); }
};
struct IterBuf {
    alignas(8) unsigned char raw[128];
};

using dbus_bool_t = uint32_t;
using Conn = void;
using Msg = void;

}  // namespace

struct DBusConn::Api {
    void (*error_init)(void*) = nullptr;
    dbus_bool_t (*error_is_set)(const void*) = nullptr;
    void (*error_free)(void*) = nullptr;
    Conn* (*bus_get_private)(int, void*) = nullptr;
    void (*set_exit_on_disconnect)(Conn*, dbus_bool_t) = nullptr;
    void (*conn_close)(Conn*) = nullptr;
    void (*conn_unref)(Conn*) = nullptr;
    Msg* (*new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    Msg* (*send_block)(Conn*, Msg*, int, void*) = nullptr;
    void (*msg_unref)(Msg*) = nullptr;
    dbus_bool_t (*iter_init)(Msg*, void*) = nullptr;
    void (*iter_init_append)(Msg*, void*) = nullptr;
    dbus_bool_t (*iter_append_basic)(void*, int, const void*) = nullptr;
    int (*iter_arg_type)(void*) = nullptr;
    void (*iter_get_basic)(void*, void*) = nullptr;
    dbus_bool_t (*iter_next)(void*) = nullptr;
    void (*iter_recurse)(void*, void*) = nullptr;
    dbus_bool_t (*add_match)(Conn*, const char*, void*) = nullptr;
    dbus_bool_t (*read_write)(Conn*, int) = nullptr;
    Msg* (*pop_message)(Conn*) = nullptr;
    int (*msg_type)(Msg*) = nullptr;
    const char* (*msg_interface)(Msg*) = nullptr;
    const char* (*msg_member)(Msg*) = nullptr;
};

bool DBusConn::open(BusType bus) {
    close();
    if (!lib_.open("libdbus-1.so.3", "libdbus-1.so")) {
        error_ = "libdbus-1 not found";
        return false;
    }
    api_ = new Api;
    Api& a = *api_;
    a.error_init = lib_.fn<decltype(a.error_init)>("dbus_error_init");
    a.error_is_set = lib_.fn<decltype(a.error_is_set)>("dbus_error_is_set");
    a.error_free = lib_.fn<decltype(a.error_free)>("dbus_error_free");
    a.bus_get_private = lib_.fn<decltype(a.bus_get_private)>("dbus_bus_get_private");
    a.set_exit_on_disconnect = lib_.fn<decltype(a.set_exit_on_disconnect)>("dbus_connection_set_exit_on_disconnect");
    a.conn_close = lib_.fn<decltype(a.conn_close)>("dbus_connection_close");
    a.conn_unref = lib_.fn<decltype(a.conn_unref)>("dbus_connection_unref");
    a.new_method_call = lib_.fn<decltype(a.new_method_call)>("dbus_message_new_method_call");
    a.send_block = lib_.fn<decltype(a.send_block)>("dbus_connection_send_with_reply_and_block");
    a.msg_unref = lib_.fn<decltype(a.msg_unref)>("dbus_message_unref");
    a.iter_init = lib_.fn<decltype(a.iter_init)>("dbus_message_iter_init");
    a.iter_init_append = lib_.fn<decltype(a.iter_init_append)>("dbus_message_iter_init_append");
    a.iter_append_basic = lib_.fn<decltype(a.iter_append_basic)>("dbus_message_iter_append_basic");
    a.iter_arg_type = lib_.fn<decltype(a.iter_arg_type)>("dbus_message_iter_get_arg_type");
    a.iter_get_basic = lib_.fn<decltype(a.iter_get_basic)>("dbus_message_iter_get_basic");
    a.iter_next = lib_.fn<decltype(a.iter_next)>("dbus_message_iter_next");
    a.iter_recurse = lib_.fn<decltype(a.iter_recurse)>("dbus_message_iter_recurse");
    a.add_match = lib_.fn<decltype(a.add_match)>("dbus_bus_add_match");
    a.read_write = lib_.fn<decltype(a.read_write)>("dbus_connection_read_write");
    a.pop_message = lib_.fn<decltype(a.pop_message)>("dbus_connection_pop_message");
    a.msg_type = lib_.fn<decltype(a.msg_type)>("dbus_message_get_type");
    a.msg_interface = lib_.fn<decltype(a.msg_interface)>("dbus_message_get_interface");
    a.msg_member = lib_.fn<decltype(a.msg_member)>("dbus_message_get_member");
    if (!a.error_init || !a.error_is_set || !a.error_free || !a.bus_get_private || !a.set_exit_on_disconnect || !a.conn_close ||
        !a.conn_unref || !a.new_method_call || !a.send_block || !a.msg_unref || !a.iter_init || !a.iter_init_append ||
        !a.iter_append_basic || !a.iter_arg_type || !a.iter_get_basic || !a.iter_next || !a.iter_recurse || !a.add_match || !a.read_write || !a.pop_message || !a.msg_type || !a.msg_interface || !a.msg_member) {
        error_ = "libdbus-1 is missing required symbols";
        close();
        return false;
    }
    DBusErrorBuf err;
    std::memset(err.raw, 0, sizeof err.raw);
    a.error_init(err.raw);
    conn_ = a.bus_get_private(static_cast<int>(bus), err.raw);
    if (!conn_) {
        error_ = a.error_is_set(err.raw) && err.message() ? err.message() : "cannot connect to the bus";
        a.error_free(err.raw);
        close();
        return false;
    }
    a.error_free(err.raw);
    a.set_exit_on_disconnect(conn_, 0);  // never let libdbus call exit() in the game process
    return true;
}

void DBusConn::close() {
    if (conn_ && api_) {
        api_->conn_close(conn_);
        api_->conn_unref(conn_);
    }
    conn_ = nullptr;
    delete api_;
    api_ = nullptr;
    lib_.close();
}

void DBusConn::read_args(void* msg, std::vector<DBusValue>& out) {
    Api& a = *api_;
    out.clear();
    IterBuf it;
    std::memset(it.raw, 0, sizeof it.raw);
    if (a.iter_init(msg, it.raw)) {
            do {
                void* cur = it.raw;
                IterBuf sub;
                int t = a.iter_arg_type(cur);
                IterBuf nest;
                for (int depth = 0; t == 'v' && depth < 4; ++depth) {  // unwrap variants (portals nest them)
                    IterBuf& dst = (cur == sub.raw) ? nest : sub;
                    std::memset(dst.raw, 0, sizeof dst.raw);
                    a.iter_recurse(cur, dst.raw);
                    cur = dst.raw;
                    t = a.iter_arg_type(cur);
                }
                DBusValue v;
                v.type = static_cast<char>(t);
                switch (t) {
                    case 's':
                    case 'o':
                    case 'g': {
                        const char* p = nullptr;
                        a.iter_get_basic(cur, &p);
                        v.s = p ? p : "";
                        break;
                    }
                    case 'u': { uint32_t x = 0; a.iter_get_basic(cur, &x); v.u = x; break; }
                    case 'b': { dbus_bool_t x = 0; a.iter_get_basic(cur, &x); v.u = x; break; }
                    case 'i': { int32_t x = 0; a.iter_get_basic(cur, &x); v.i = x; break; }
                    case 'x': { int64_t x = 0; a.iter_get_basic(cur, &x); v.i = x; break; }
                    case 't': { uint64_t x = 0; a.iter_get_basic(cur, &x); v.u = x; break; }
                    case 'd': { double x = 0; a.iter_get_basic(cur, &x); v.d = x; break; }
                    case 'h': { int x = -1; a.iter_get_basic(cur, &x); v.i = x >= 0 ? ::dup(x) : -1; break; }  // we own a duplicate; the message keeps its own
                    default: break;  // containers are reported by type only
                }
                out.push_back(std::move(v));
            } while (a.iter_next(it.raw));
    }
}

bool DBusConn::call(const std::string& dest, const std::string& path, const std::string& iface, const std::string& method,
                    const std::vector<DBusArg>& args, std::vector<DBusValue>* reply, int timeout_ms) {
    if (!conn_) {
        error_ = "not connected";
        return false;
    }
    Api& a = *api_;
    Msg* m = a.new_method_call(dest.c_str(), path.c_str(), iface.c_str(), method.c_str());
    if (!m) {
        error_ = "out of memory";
        return false;
    }
    if (!args.empty()) {
        IterBuf it;
        std::memset(it.raw, 0, sizeof it.raw);
        a.iter_init_append(m, it.raw);
        for (const DBusArg& arg : args) {
            bool ok = false;
            switch (arg.type) {
                case 's':
                case 'o': {
                    const char* p = arg.s.c_str();
                    ok = a.iter_append_basic(it.raw, arg.type, &p);
                    break;
                }
                case 'u': { uint32_t v = static_cast<uint32_t>(arg.u); ok = a.iter_append_basic(it.raw, 'u', &v); break; }
                case 'b': { dbus_bool_t v = arg.u ? 1 : 0; ok = a.iter_append_basic(it.raw, 'b', &v); break; }
                case 'i': { int32_t v = static_cast<int32_t>(arg.i); ok = a.iter_append_basic(it.raw, 'i', &v); break; }
                case 'x': { int64_t v = arg.i; ok = a.iter_append_basic(it.raw, 'x', &v); break; }
                case 't': { uint64_t v = arg.u; ok = a.iter_append_basic(it.raw, 't', &v); break; }
                default: break;
            }
            if (!ok) {
                error_ = "cannot append argument";
                a.msg_unref(m);
                return false;
            }
        }
    }
    DBusErrorBuf err;
    std::memset(err.raw, 0, sizeof err.raw);
    a.error_init(err.raw);
    Msg* r = a.send_block(conn_, m, timeout_ms, err.raw);
    a.msg_unref(m);
    if (!r) {
        error_ = a.error_is_set(err.raw) ? std::string(err.name() ? err.name() : "error") + ": " + (err.message() ? err.message() : "") : "no reply";
        a.error_free(err.raw);
        return false;
    }
    a.error_free(err.raw);
    if (reply) read_args(r, *reply);
    a.msg_unref(r);
    return true;
}

bool DBusConn::get_property(const std::string& dest, const std::string& path, const std::string& iface, const std::string& prop,
                            DBusValue& out, int timeout_ms) {
    std::vector<DBusValue> r;
    if (!call(dest, path, "org.freedesktop.DBus.Properties", "Get", {DBusArg::str(iface), DBusArg::str(prop)}, &r, timeout_ms)) return false;
    if (r.empty()) {
        error_ = "empty reply";
        return false;
    }
    out = r[0];
    return true;
}

bool DBusConn::add_match(const std::string& rule) {
    if (!conn_) {
        error_ = "not connected";
        return false;
    }
    DBusErrorBuf err;
    std::memset(err.raw, 0, sizeof err.raw);
    api_->error_init(err.raw);
    api_->add_match(conn_, rule.c_str(), err.raw);
    const bool failed = api_->error_is_set(err.raw);
    if (failed) error_ = err.message() ? err.message() : "add_match failed";
    api_->error_free(err.raw);
    return !failed;
}

int DBusConn::wait_signal(int timeout_ms, DBusSignal& out) {
    if (!conn_) return -1;
    Api& a = *api_;
    for (int attempt = 0; attempt < 2; ++attempt) {
        while (Msg* m = a.pop_message(conn_)) {
            if (a.msg_type(m) == 4 /* DBUS_MESSAGE_TYPE_SIGNAL */) {
                const char* i = a.msg_interface(m);
                const char* n = a.msg_member(m);
                out.interface = i ? i : "";
                out.member = n ? n : "";
                read_args(m, out.args);
                a.msg_unref(m);
                return 1;
            }
            a.msg_unref(m);
        }
        if (attempt == 1) break;
        if (!a.read_write(conn_, timeout_ms)) return -1;  // disconnected
    }
    return 0;
}

bool DBusConn::name_has_owner(const std::string& name) {
    std::vector<DBusValue> r;
    if (!call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", {DBusArg::str(name)}, &r, 1000)) return false;
    return !r.empty() && r[0].type == 'b' && r[0].u != 0;
}

}  // namespace nue::lx
