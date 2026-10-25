// Tiny RAII wrapper around dlopen/dlsym. Every optional system library of the Linux backend
// (libvulkan, libgamemode, libdbus-1, libpipewire, libudev, ...) is reached through it, so the
// engine has no hard link-time dependency on any of them.
#pragma once
#include <string>

namespace nue::lx {

class DynLib {
public:
    DynLib() = default;
    ~DynLib() { close(); }
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;
    DynLib(DynLib&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    DynLib& operator=(DynLib&& o) noexcept {
        if (this != &o) {
            close();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }

    // Tries each name in order (e.g. "libvulkan.so.1", "libvulkan.so"); RTLD_NOW | RTLD_LOCAL.
    bool open(const char* name1, const char* name2 = nullptr, const char* name3 = nullptr);
    void close();
    bool is_open() const { return h_ != nullptr; }
    void* symbol(const char* name) const;
    template <class Fn>
    Fn fn(const char* name) const { return reinterpret_cast<Fn>(symbol(name)); }
    // dlerror() text of the last failed open() (thread-unsafe, diagnostic only).
    const std::string& error() const { return error_; }

private:
    void* h_ = nullptr;
    std::string error_;
};

}  // namespace nue::lx
