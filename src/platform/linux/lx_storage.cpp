#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "lx_storage.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#if defined(__has_include)
#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#define NUE_HAVE_IO_URING 1
#endif
#endif

namespace nue::lx {

// ---------------------------------------------------------------------------- GameFiles

int GameFiles::open_read(std::string_view win_path) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        ResolveResult r = r_.resolve(win_path);
        if (!r.found) return -ENOENT;
        int fd = ::open(r.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) return fd;
        if (errno != ENOENT) return -errno;
        r_.invalidate_all();  // stale index: the entry vanished behind our back
    }
    return -ENOENT;
}

int GameFiles::open_write(std::string_view win_path, bool truncate) {
    ResolveResult r = r_.resolve(win_path);
    if (!r.found && !r.parent_found) return -ENOENT;
    int fd = ::open(r.path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | (truncate ? O_TRUNC : 0), 0644);
    if (fd < 0) return -errno;
    if (!r.found) r_.note_created(r.path, false);
    return fd;
}

bool GameFiles::exists(std::string_view p) { return r_.resolve(p).found; }

bool GameFiles::file_size(std::string_view p, uint64_t& size) {
    ResolveResult r = r_.resolve(p);
    struct stat st;
    if (!r.found || ::stat(r.path.c_str(), &st) != 0) return false;
    size = static_cast<uint64_t>(st.st_size);
    return true;
}

bool GameFiles::remove(std::string_view p) {
    ResolveResult r = r_.resolve(p);
    if (!r.found) return false;
    bool ok = ::unlink(r.path.c_str()) == 0;
    if (ok) r_.note_removed(r.path);
    return ok;
}

bool GameFiles::make_dirs(std::string_view p) {
    ResolveResult r = r_.resolve(p);
    if (r.found) return true;
    if (!make_dirs_posix(r.path)) return false;
    for (size_t pos = r.path.find('/', 1); pos != std::string::npos; pos = r.path.find('/', pos + 1)) r_.invalidate_dir(r.path.substr(0, pos));
    r_.invalidate_dir(r.path);
    return true;
}

// ---------------------------------------------------------------------------- helpers

bool pread_full(int fd, void* buf, size_t len, uint64_t offset) {
    char* p = static_cast<char*>(buf);
    while (len) {
        ssize_t n = ::pread(fd, p, len, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
        offset += static_cast<uint64_t>(n);
    }
    return true;
}

bool write_full(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len) {
        ssize_t n = ::write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool make_dirs_posix(const std::string& path, unsigned mode) {
    if (path.empty()) return false;
    for (size_t pos = 1; pos <= path.size(); ++pos) {
        if (pos != path.size() && path[pos] != '/') continue;
        std::string part = path.substr(0, pos);
        if (::mkdir(part.c_str(), mode) != 0 && errno != EEXIST) return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool write_file_atomic(const std::string& path, const void* data, size_t len) {
    std::string dir = ".";
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) dir = slash == 0 ? "/" : path.substr(0, slash);
    std::string tmp = path + ".tmp" + std::to_string(::getpid());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = write_full(fd, data, len) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool read_whole_file(const std::string& path, std::string& out, size_t max_bytes) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    out.clear();
    bool ok = ::fstat(fd, &st) == 0 && static_cast<uint64_t>(st.st_size) <= max_bytes;
    if (ok) {
        out.resize(static_cast<size_t>(st.st_size));
        ok = pread_full(fd, out.data(), out.size(), 0);
    }
    ::close(fd);
    return ok;
}

bool free_space_bytes(const std::string& path, uint64_t& avail) {
    struct statvfs sv;
    if (::statvfs(path.c_str(), &sv) != 0) return false;
    avail = static_cast<uint64_t>(sv.f_bavail) * static_cast<uint64_t>(sv.f_frsize);
    return true;
}

// ---------------------------------------------------------------------------- FileMap

bool FileMap::open_readonly(const std::string& path, bool populate) {
    close();
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return st.st_size == 0;  // an empty file maps to nothing but is not an error
    }
    int flags = MAP_PRIVATE | (populate ? MAP_POPULATE : 0);
    void* p = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, flags, fd, 0);
    ::close(fd);  // the mapping keeps the file alive
    if (p == MAP_FAILED) return false;
    data_ = p;
    size_ = static_cast<size_t>(st.st_size);
    return true;
}

void FileMap::close() {
    if (data_) ::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
}
void FileMap::advise_sequential() { if (data_) ::madvise(data_, size_, MADV_SEQUENTIAL); }
void FileMap::advise_random() { if (data_) ::madvise(data_, size_, MADV_RANDOM); }
void FileMap::advise_dont_need() { if (data_) ::madvise(data_, size_, MADV_DONTNEED); }

// ---------------------------------------------------------------------------- InstanceLock

bool InstanceLock::acquire(const std::string& lock_path) {
    release();
    fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) return false;
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void InstanceLock::release() {
    if (fd_ >= 0) ::close(fd_);  // closing drops the flock
    fd_ = -1;
}

// ---------------------------------------------------------------------------- io_uring

#if defined(NUE_HAVE_IO_URING) && defined(__NR_io_uring_setup)

namespace {

struct Pending {
    int fd;
    char* buf;
    size_t len;
    uint64_t off;
    size_t done = 0;
    ReadCallback cb;
};

class Uring {
public:
    ~Uring() { stop(); }

    bool init(unsigned entries) {
        struct io_uring_params p;
        std::memset(&p, 0, sizeof p);
        ring_fd_ = static_cast<int>(::syscall(__NR_io_uring_setup, entries, &p));
        if (ring_fd_ < 0) return false;
        sq_entries_ = p.sq_entries;
        cq_entries_ = p.cq_entries;
        size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
        const bool single = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
        if (single && cq_sz > sq_sz) sq_sz = cq_sz;
        sq_ptr_ = ::mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
        if (sq_ptr_ == MAP_FAILED) { sq_ptr_ = nullptr; teardown(); return false; }
        sq_map_ = sq_sz;
        if (single) {
            cq_ptr_ = sq_ptr_;
        } else {
            cq_ptr_ = ::mmap(nullptr, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
            if (cq_ptr_ == MAP_FAILED) { cq_ptr_ = nullptr; teardown(); return false; }
            cq_map_ = cq_sz;
        }
        sqes_map_ = p.sq_entries * sizeof(struct io_uring_sqe);
        sqes_ = static_cast<struct io_uring_sqe*>(::mmap(nullptr, sqes_map_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES));
        if (sqes_ == MAP_FAILED) { sqes_ = nullptr; teardown(); return false; }
        char* sq = static_cast<char*>(sq_ptr_);
        char* cq = static_cast<char*>(cq_ptr_);
        sq_head_ = reinterpret_cast<unsigned*>(sq + p.sq_off.head);
        sq_tail_ = reinterpret_cast<unsigned*>(sq + p.sq_off.tail);
        sq_mask_ = *reinterpret_cast<unsigned*>(sq + p.sq_off.ring_mask);
        sq_array_ = reinterpret_cast<unsigned*>(sq + p.sq_off.array);
        cq_head_ = reinterpret_cast<unsigned*>(cq + p.cq_off.head);
        cq_tail_ = reinterpret_cast<unsigned*>(cq + p.cq_off.tail);
        cq_mask_ = *reinterpret_cast<unsigned*>(cq + p.cq_off.ring_mask);
        cqes_ = reinterpret_cast<struct io_uring_cqe*>(cq + p.cq_off.cqes);
        running_ = true;
        reaper_ = std::thread([this] { reap_loop(); });
        return true;
    }

    bool submit(int fd, void* buf, size_t len, uint64_t off, ReadCallback cb) {
        std::lock_guard<std::mutex> g(m_);
        if (!running_) return false;
        uint64_t id = ++next_id_;
        pending_[id] = Pending{fd, static_cast<char*>(buf), len, off, 0, std::move(cb)};
        if (!queue_locked(id)) {
            pending_.erase(id);
            return false;
        }
        return true;
    }

    void stop() {
        {
            std::unique_lock<std::mutex> l(m_);
            if (!running_ && ring_fd_ < 0) return;
            drain_cv_.wait_for(l, std::chrono::seconds(30), [this] { return pending_.empty(); });
            running_ = false;
            if (sqes_) queue_nop_locked();  // wake the reaper
        }
        if (reaper_.joinable()) reaper_.join();
        teardown();
    }

    uint64_t completed() const { return completed_.load(); }

private:
    static constexpr uint64_t kWake = ~0ull;

    bool queue_locked(uint64_t id) {
        Pending& p = pending_[id];
        unsigned tail = *sq_tail_;
        unsigned head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        if (tail - head >= sq_entries_) return false;
        unsigned idx = tail & sq_mask_;
        struct io_uring_sqe* sqe = &sqes_[idx];
        std::memset(sqe, 0, sizeof *sqe);
        sqe->opcode = IORING_OP_READ;
        sqe->fd = p.fd;
        sqe->off = p.off + p.done;
        sqe->addr = reinterpret_cast<unsigned long>(p.buf + p.done);
        sqe->len = static_cast<unsigned>(p.len - p.done);
        sqe->user_data = id;
        sq_array_[idx] = idx;
        __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);
        return ::syscall(__NR_io_uring_enter, ring_fd_, 1, 0, 0, nullptr, 0) >= 0;
    }

    bool queue_nop_locked() {
        unsigned tail = *sq_tail_;
        unsigned head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        if (tail - head >= sq_entries_) return false;
        unsigned idx = tail & sq_mask_;
        struct io_uring_sqe* sqe = &sqes_[idx];
        std::memset(sqe, 0, sizeof *sqe);
        sqe->opcode = IORING_OP_NOP;
        sqe->user_data = kWake;
        sq_array_[idx] = idx;
        __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);
        return ::syscall(__NR_io_uring_enter, ring_fd_, 1, 0, 0, nullptr, 0) >= 0;
    }

    void reap_loop() {
        for (;;) {
            unsigned head = *cq_head_;
            unsigned tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
            if (head == tail) {
                {
                    std::lock_guard<std::mutex> g(m_);
                    if (!running_ && pending_.empty()) return;
                }
                ::syscall(__NR_io_uring_enter, ring_fd_, 0, 1, IORING_ENTER_GETEVENTS, nullptr, 0);
                continue;
            }
            while (head != tail) {
                struct io_uring_cqe cqe = cqes_[head & cq_mask_];
                ++head;
                __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);
                if (cqe.user_data == kWake) continue;
                complete(cqe.user_data, cqe.res);
            }
        }
    }

    void complete(uint64_t id, int res) {
        ReadCallback cb;
        int64_t result = 0;
        {
            std::lock_guard<std::mutex> g(m_);
            auto it = pending_.find(id);
            if (it == pending_.end()) return;
            Pending& p = it->second;
            if (res > 0) p.done += static_cast<size_t>(res);
            const bool finished = res <= 0 || p.done >= p.len;
            if (!finished) {
                if (queue_locked(id)) return;  // short read: continue with the remainder
            }
            result = res < 0 ? res : static_cast<int64_t>(p.done);
            cb = std::move(p.cb);
            pending_.erase(it);
            if (pending_.empty()) drain_cv_.notify_all();
        }
        completed_.fetch_add(1);
        if (cb) cb(result);
    }

    void teardown() {
        if (sqes_) ::munmap(sqes_, sqes_map_);
        if (cq_ptr_ && cq_ptr_ != sq_ptr_) ::munmap(cq_ptr_, cq_map_);
        if (sq_ptr_) ::munmap(sq_ptr_, sq_map_);
        if (ring_fd_ >= 0) ::close(ring_fd_);
        sqes_ = nullptr; cq_ptr_ = nullptr; sq_ptr_ = nullptr; ring_fd_ = -1;
    }

    int ring_fd_ = -1;
    void* sq_ptr_ = nullptr;
    void* cq_ptr_ = nullptr;
    size_t sq_map_ = 0, cq_map_ = 0, sqes_map_ = 0;
    struct io_uring_sqe* sqes_ = nullptr;
    struct io_uring_cqe* cqes_ = nullptr;
    unsigned *sq_head_ = nullptr, *sq_tail_ = nullptr, *sq_array_ = nullptr, *cq_head_ = nullptr, *cq_tail_ = nullptr;
    unsigned sq_mask_ = 0, cq_mask_ = 0, sq_entries_ = 0, cq_entries_ = 0;
    std::mutex m_;
    std::condition_variable drain_cv_;
    std::map<uint64_t, Pending> pending_;
    uint64_t next_id_ = 0;
    bool running_ = false;
    std::thread reaper_;
    std::atomic<uint64_t> completed_{0};
};

}  // namespace

#define NUE_URING_COMPILED 1

#endif  // io_uring

// ---------------------------------------------------------------------------- AsyncReader

struct AsyncReader::Impl {
    std::unique_ptr<TaskQueue> pool;
    std::string backend = "thread-pool";
    std::atomic<uint64_t> done{0};
#if defined(NUE_URING_COMPILED)
    std::unique_ptr<Uring> ring;
#endif
};

AsyncReader::AsyncReader() : impl_(new Impl) {}
AsyncReader::~AsyncReader() { stop(); }

bool AsyncReader::start(const AsyncReaderOptions& o) {
    stop();
#if defined(NUE_URING_COMPILED)
    if (o.prefer_io_uring) {
        auto r = std::make_unique<Uring>();
        if (r->init(o.ring_entries)) {
            impl_->ring = std::move(r);
            impl_->backend = "io_uring";
            return true;
        }
    }
#endif
    impl_->pool = std::make_unique<TaskQueue>(o.pool_threads, [](size_t) {});
    impl_->backend = "thread-pool";
    return true;
}

void AsyncReader::stop() {
#if defined(NUE_URING_COMPILED)
    if (impl_->ring) {
        impl_->done += impl_->ring->completed();
        impl_->ring->stop();
        impl_->ring.reset();
    }
#endif
    if (impl_->pool) {
        impl_->pool->shutdown();
        impl_->pool.reset();
    }
}

bool AsyncReader::read(int fd, void* buf, size_t len, uint64_t offset, ReadCallback cb) {
#if defined(NUE_URING_COMPILED)
    if (impl_->ring) {
        if (impl_->ring->submit(fd, buf, len, offset, cb)) return true;
        // ring full or shut down: fall through to a one-off thread so the request is never lost
        std::thread([=] {
            ssize_t n = ::pread(fd, buf, len, static_cast<off_t>(offset));
            if (cb) cb(n < 0 ? -errno : n);
        }).detach();
        return true;
    }
#endif
    if (!impl_->pool) return false;
    Impl* im = impl_.get();
    return impl_->pool->post([=] {
        char* p = static_cast<char*>(buf);
        size_t got = 0;
        int64_t result = 0;
        while (got < len) {
            ssize_t n = ::pread(fd, p + got, len - got, static_cast<off_t>(offset + got));
            if (n < 0) {
                if (errno == EINTR) continue;
                result = -errno;
                break;
            }
            if (n == 0) break;
            got += static_cast<size_t>(n);
        }
        if (result == 0) result = static_cast<int64_t>(got);
        im->done.fetch_add(1);
        if (cb) cb(result);
    });
}

const char* AsyncReader::backend() const { return impl_->backend.c_str(); }
uint64_t AsyncReader::completed() const {
    uint64_t n = impl_->done.load();
#if defined(NUE_URING_COMPILED)
    if (impl_->ring) n += impl_->ring->completed();
#endif
    return n;
}

}  // namespace nue::lx
