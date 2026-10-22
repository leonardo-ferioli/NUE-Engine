// Storage services (Linux only): game file access through the case-insensitive resolver,
// memory-mapped files, atomic writes, single-instance lock, free space, and asynchronous reads
// (io_uring when allowed, thread pool otherwise).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "lx_pathcase.hpp"
#include "lx_taskqueue.hpp"

namespace nue::lx {

// ---- game files ----------------------------------------------------------------------
class GameFiles {
public:
    // `resolver` must outlive this object.
    explicit GameFiles(PathResolver& resolver) : r_(resolver) {}

    // open(2) with a Windows-style path. Retries once after dropping the cached directory index when
    // the cached name disappeared (file renamed/deleted behind our back). Returns fd or -errno.
    int open_read(std::string_view win_path);
    // O_CREAT|O_RDWR; the parent directory must exist (resolved case-insensitively). Returns fd or -errno.
    int open_write(std::string_view win_path, bool truncate);
    // stat-like helpers on resolved paths
    bool exists(std::string_view win_path);
    bool file_size(std::string_view win_path, uint64_t& size);
    bool remove(std::string_view win_path);
    bool make_dirs(std::string_view win_path);  // mkdir -p, existing components matched case-insensitively
    PathResolver& resolver() { return r_; }

private:
    PathResolver& r_;
};

// ---- helpers on file descriptors / paths ---------------------------------------------
bool pread_full(int fd, void* buf, size_t len, uint64_t offset);  // false on error or EOF before len
bool write_full(int fd, const void* buf, size_t len);
// Writes `data` to path atomically: temp file in the same directory, fsync, rename, fsync of the directory.
bool write_file_atomic(const std::string& path, const void* data, size_t len);
bool read_whole_file(const std::string& path, std::string& out, size_t max_bytes = 1u << 30);
bool free_space_bytes(const std::string& path, uint64_t& avail);
bool make_dirs_posix(const std::string& path, unsigned mode = 0755);  // mkdir -p on an exact path

// ---- memory mapping ------------------------------------------------------------------
class FileMap {
public:
    FileMap() = default;
    ~FileMap() { close(); }
    FileMap(const FileMap&) = delete;
    FileMap& operator=(const FileMap&) = delete;
    // Maps the whole file read-only (MAP_PRIVATE). `populate` pre-faults pages (MAP_POPULATE).
    bool open_readonly(const std::string& path, bool populate = false);
    void close();
    const void* data() const { return data_; }
    size_t size() const { return size_; }
    // madvise helpers: sequential streaming vs random access vs "drop pages we are done with"
    void advise_sequential();
    void advise_random();
    void advise_dont_need();

private:
    void* data_ = nullptr;
    size_t size_ = 0;
};

// ---- single instance -----------------------------------------------------------------
class InstanceLock {
public:
    ~InstanceLock() { release(); }
    // flock(LOCK_EX|LOCK_NB) on a file; false if another process holds it. The lock dies with the process.
    bool acquire(const std::string& lock_path);
    void release();

private:
    int fd_ = -1;
};

// ---- asynchronous reads --------------------------------------------------------------
struct AsyncReaderOptions {
    bool prefer_io_uring = false;  // opt-in: some sandboxes block io_uring (seccomp), it is probed anyway
    size_t pool_threads = 2;
    unsigned ring_entries = 128;
};
using ReadCallback = std::function<void(int64_t result)>;  // bytes read (may be short at EOF) or -errno

class AsyncReader {
public:
    AsyncReader();
    ~AsyncReader();
    bool start(const AsyncReaderOptions& o);
    void stop();  // waits for in-flight requests
    // Never blocks the caller for I/O. The callback runs on an I/O thread. `buf` must stay valid until then.
    bool read(int fd, void* buf, size_t len, uint64_t offset, ReadCallback cb);
    const char* backend() const;  // "io_uring" or "thread-pool"
    uint64_t completed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nue::lx
