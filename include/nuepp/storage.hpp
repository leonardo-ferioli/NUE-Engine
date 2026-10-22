// NUE C++ wrapper: virtual file system.
#pragma once

#include "nuepp/core.hpp"

namespace nuepp {

// File handle (32-bit generation-checked id). Closed by the destructor.
class File {
public:
    File() = default;
    File(const char* path, nue_open_flags flags = NUE_OPEN_READ) { result_ = Result(nue_file_open(path, flags, &f_)); }
    File(File&& o) noexcept : f_(o.f_), result_(o.result_) { o.f_ = 0; }
    File& operator=(File&& o) noexcept { if (this != &o) { close(); f_ = o.f_; result_ = o.result_; o.f_ = 0; } return *this; }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { close(); }
    void close() { if (f_) { nue_file_close(f_); f_ = 0; } }

    bool ok() const { return f_ != 0; }
    const Result& result() const { return result_; }
    nue_file handle() const { return f_; }
    uint64_t size() const { uint64_t s = 0; nue_file_size(f_, &s); return s; }
    // Reads up to n bytes; returns the number read (0 at end of file).
    size_t read(void* dst, size_t n) { size_t got = 0; nue_file_read(f_, dst, n, &got); return got; }
    size_t read_at(uint64_t offset, void* dst, size_t n) { size_t got = 0; nue_file_read_at(f_, offset, dst, n, &got); return got; }
    size_t write(const void* src, size_t n) { size_t put = 0; nue_file_write(f_, src, n, &put); return put; }
    Result seek(int64_t offset, nue_seek_origin origin = NUE_SEEK_SET, uint64_t* pos = nullptr) { return nue_file_seek(f_, offset, origin, pos); }
    uint64_t tell() const { uint64_t p = 0; nue_file_tell(f_, &p); return p; }
    std::vector<uint8_t> read_all() {
        std::vector<uint8_t> v(static_cast<size_t>(size()));
        size_t got = v.empty() ? 0 : read_at(0, v.data(), v.size());
        v.resize(got);
        return v;
    }
private:
    nue_file f_ = 0;
    Result result_;
};

namespace vfs {
inline bool exists(const char* path) { return nue_vfs_exists(path) != 0; }
inline Expected<std::vector<uint8_t>> load(const char* path) {
    void* data = nullptr;
    size_t size = 0;
    Result r(nue_file_load(path, &data, &size));
    if (!r) return r;
    std::vector<uint8_t> v(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    nue_free(data);
    return v;
}
inline Result save(const char* path, const void* data, size_t size) { return nue_file_save(path, data, size); }
inline std::string resolve(const char* path, bool for_write = false) {
    size_t need = 0;
    if (NUE_FAILED(nue_vfs_resolve(path, for_write ? NUE_TRUE : NUE_FALSE, nullptr, 0, &need))) return std::string();
    std::string s(need, '\0');
    nue_vfs_resolve(path, for_write ? NUE_TRUE : NUE_FALSE, &s[0], s.size(), &need);
    s.resize(need ? need - 1 : 0);
    return s;
}
// Lists the entry names of a directory (merged over all mounts of its root).
inline std::vector<std::string> list(const char* path) {
    std::vector<std::string> names;
    nue_vfs_list(path, [](void* u, const nue_dir_entry* e) { static_cast<std::vector<std::string>*>(u)->push_back(e->name); return NUE_TRUE; }, &names);
    return names;
}
}  // namespace vfs

}  // namespace nuepp
