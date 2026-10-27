#include "nue_legacy/win32/host/windows_registry.hpp"
#include "nue_legacy/win32/handles.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace nue::legacy::win32::host {
namespace {

struct RealKey : Object {
    explicit RealKey(HKEY k) : Object(ObjectKind::registry_key), key(k) {}
    ~RealKey() override { if (key) RegCloseKey(key); }
    HKEY key;
};

class WindowsRegistry final : public Registry {
public:
    uint32_t open_key(Handle parent, std::string_view sub_key, Handle& out) override {
        HKEY p;
        if (!real(parent, p)) return err::invalid_handle;
        HKEY k = nullptr;
        const LSTATUS s = RegOpenKeyExA(p, std::string(sub_key).c_str(), 0, KEY_READ | KEY_WRITE, &k);
        if (s != ERROR_SUCCESS) {
            const LSTATUS s2 = RegOpenKeyExA(p, std::string(sub_key).c_str(), 0, KEY_READ, &k);   // a key the user may read but not write
            if (s2 != ERROR_SUCCESS) return uint32_t(s2);
        }
        return wrap(k, out);
    }
    uint32_t create_key(Handle parent, std::string_view sub_key, Handle& out, uint32_t* disposition) override {
        HKEY p;
        if (!real(parent, p)) return err::invalid_handle;
        HKEY k = nullptr;
        DWORD d = 0;
        const LSTATUS s = RegCreateKeyExA(p, std::string(sub_key).c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, &d);
        if (s != ERROR_SUCCESS) return uint32_t(s);
        if (disposition) *disposition = d;
        return wrap(k, out);
    }
    uint32_t close_key(Handle key) override {
        if (is_predefined(key)) return err::success;
        return table_.close(key) ? err::success : err::invalid_handle;
    }
    uint32_t query_value(Handle key, std::string_view name, uint32_t* type, uint8_t* data, uint32_t* size) override {
        HKEY k;
        if (!real(key, k)) return err::invalid_handle;
        DWORD t = 0, n = size ? *size : 0;
        const LSTATUS s = RegQueryValueExA(k, std::string(name).c_str(), nullptr, &t, data, size ? &n : nullptr);
        if (type) *type = t;
        if (size) *size = n;
        return uint32_t(s);
    }
    uint32_t set_value(Handle key, std::string_view name, uint32_t type, const uint8_t* data, uint32_t size) override {
        HKEY k;
        if (!real(key, k)) return err::invalid_handle;
        return uint32_t(RegSetValueExA(k, std::string(name).c_str(), 0, type, data, size));
    }
    uint32_t delete_value(Handle key, std::string_view name) override {
        HKEY k;
        if (!real(key, k)) return err::invalid_handle;
        return uint32_t(RegDeleteValueA(k, std::string(name).c_str()));
    }
    uint32_t delete_key(Handle parent, std::string_view sub_key) override {
        HKEY p;
        if (!real(parent, p)) return err::invalid_handle;
        return uint32_t(RegDeleteKeyA(p, std::string(sub_key).c_str()));
    }
    uint32_t enum_key(Handle key, uint32_t index, std::string& name) override {
        HKEY k;
        if (!real(key, k)) return err::invalid_handle;
        char buffer[512];
        DWORD n = sizeof buffer;
        const LSTATUS s = RegEnumKeyExA(k, index, buffer, &n, nullptr, nullptr, nullptr, nullptr);
        if (s == ERROR_SUCCESS) name.assign(buffer, n);
        return uint32_t(s);
    }
    uint32_t enum_value(Handle key, uint32_t index, std::string& name, uint32_t* type) override {
        HKEY k;
        if (!real(key, k)) return err::invalid_handle;
        char buffer[512];
        DWORD n = sizeof buffer, t = 0;
        const LSTATUS s = RegEnumValueA(k, index, buffer, &n, nullptr, &t, nullptr, nullptr);
        if (s == ERROR_SUCCESS) { name.assign(buffer, n); if (type) *type = t; }
        return uint32_t(s);
    }

private:
    static bool is_predefined(Handle h) { return h == hkey::classes_root || h == hkey::current_user || h == hkey::local_machine || h == hkey::users; }
    bool real(Handle h, HKEY& out) {
        if (is_predefined(h)) { out = reinterpret_cast<HKEY>(uintptr_t(int32_t(h))); return true; }   // sign-extended, as on a 64-bit host
        auto object = table_.get_as<RealKey>(h, ObjectKind::registry_key);
        if (!object) return false;
        out = object->key;
        return true;
    }
    uint32_t wrap(HKEY k, Handle& out) {
        out = table_.add(std::make_shared<RealKey>(k));
        if (!out) { RegCloseKey(k); return err::not_enough_memory; }
        return err::success;
    }
    HandleTable table_;
};
}  // namespace

std::unique_ptr<Registry> make_windows_registry() { return std::make_unique<WindowsRegistry>(); }
}  // namespace nue::legacy::win32::host
