#include "lx_dl.hpp"

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace nue::lx {

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)

bool DynLib::open(const char* a, const char* b, const char* c) {
    close();
    error_.clear();
    for (const char* n : {a, b, c}) {
        if (!n) continue;
        h_ = dlopen(n, RTLD_NOW | RTLD_LOCAL);
        if (h_) return true;
        const char* e = dlerror();
        if (e) error_ = e;
    }
    return false;
}

void DynLib::close() {
    if (h_) dlclose(h_);
    h_ = nullptr;
}

void* DynLib::symbol(const char* name) const { return h_ ? dlsym(h_, name) : nullptr; }

#else  // non-POSIX build (portable unit tests on the Windows host): nothing can be loaded

bool DynLib::open(const char*, const char*, const char*) {
    error_ = "dynamic loading is not available on this platform";
    return false;
}
void DynLib::close() { h_ = nullptr; }
void* DynLib::symbol(const char*) const { return nullptr; }

#endif

}  // namespace nue::lx
