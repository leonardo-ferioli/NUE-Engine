// nue_adec_open_file: a decoder source over the engine VFS.
#include "internal.hpp"
#include "nue/storage.h"

#include <cstdint>

namespace {
nue_result NUE_CALL file_read(void* ud, uint64_t offset, void* dst, size_t size, size_t* out_read) {
    return nue_file_read_at(nue_file(uintptr_t(ud)), offset, dst, size, out_read);
}
void NUE_CALL file_close(void* ud) { nue_file_close(nue_file(uintptr_t(ud))); }
}  // namespace

extern "C" NUE_API nue_result NUE_CALL nue_adec_open_file(const char* path, nue_adec* out) {
    if (!path || !out) return NUE_ERR_INVALID_ARGUMENT;
    nue_file f = 0;
    nue_result r = nue_file_open(path, NUE_OPEN_READ, &f);
    if (NUE_FAILED(r)) return r;
    uint64_t size = 0;
    r = nue_file_size(f, &size);
    if (NUE_FAILED(r)) { nue_file_close(f); return r; }
    nue_adec_source src;
    NUE_STRUCT_INIT(&src);
    src.read = file_read; src.close = file_close; src.userdata = reinterpret_cast<void*>(uintptr_t(f)); src.size = size;
    r = nue_adec_open_source(&src, out);
    if (NUE_FAILED(r)) nue_file_close(f);
    return r;
}
