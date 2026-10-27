#include "nue_legacy/platform/std_platform.hpp"
#include "nue_legacy/platform/storage_path.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nue::legacy::platform::stdlib {
namespace fs = std::filesystem;
namespace {

int seek64(std::FILE* f, int64_t offset, int origin) {
#if defined(_WIN32)
    return _fseeki64(f, offset, origin);
#else
    return fseeko(f, off_t(offset), origin);
#endif
}
int64_t tell64(std::FILE* f) {
#if defined(_WIN32)
    return _ftelli64(f);
#else
    return int64_t(ftello(f));
#endif
}

int64_t to_unix_ms(fs::file_time_type t) {
    // file_clock's epoch is not portable: measure the offset against the system clock now (the error is the time between the two reads).
    const auto file_now = fs::file_time_type::clock::now();
    const auto sys_now = std::chrono::system_clock::now();
    const auto delta = t - file_now;
    return std::chrono::duration_cast<std::chrono::milliseconds>((sys_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta)).time_since_epoch()).count();
}

struct OpenFile {
    std::FILE* file = nullptr;
    fs::path native;
    bool append = false, can_write = false;
    std::mutex m;
    ~OpenFile() { if (file) std::fclose(file); }
};

class StdStorage final : public Storage {
public:
    Status mount(const MountDesc& desc) override {
        const fs::path native = fs::path(desc.native_path);
        std::error_code ec;
        if (desc.create_directory) fs::create_directories(native, ec);
        if (!fs::is_directory(native, ec)) return Status::not_found;
        Mount m{path::fold(desc.root), native, desc.read_only};
        std::lock_guard lock(m_);
        if (desc.append) mounts_.push_back(std::move(m));
        else mounts_.insert(mounts_.begin(), std::move(m));
        return Status::ok;
    }
    bool has_root(std::string_view root) override {
        const std::string r = path::fold(root);
        std::lock_guard lock(m_);
        for (const auto& m : mounts_) if (m.root == r) return true;
        return false;
    }
    bool normalize(std::string_view p, std::string& out) override {
        const auto split = path::split_root(p);
        std::vector<std::string> segments;
        if (!path::normalize_relative(split.rest, segments)) return false;
        out = split.native ? path::join(segments) : split.root + ":" + path::join(segments);
        return true;
    }
    Result<std::string> resolve(std::string_view p, bool for_write) override {
        fs::path out;
        const Status s = resolve_path(p, for_write, out);
        return {s, s == Status::ok ? out.string() : std::string()};
    }

    Result<FileHandle> open(std::string_view p, uint32_t flags) override {
        const bool want_write = flags & (open_flags::write | open_flags::append);
        fs::path native;
        Status s = resolve_path(p, want_write || (flags & open_flags::create), native);
        std::error_code ec;
        const bool exists = fs::is_regular_file(native, ec);
        if (s == Status::not_found && !(flags & open_flags::create)) return {Status::not_found, {}};
        if (s != Status::ok && s != Status::not_found) return {s, {}};
        if (!exists) {
            if (!(flags & open_flags::create)) return {Status::not_found, {}};
        } else if ((flags & open_flags::create) && (flags & open_flags::exclusive)) {
            return {Status::already_exists, {}};
        }
        const char* mode;
        if (!want_write) mode = "rb";
        else if (!exists || (flags & open_flags::truncate)) mode = "w+b";
        else mode = "r+b";
        if (want_write && !writable_path(p)) return {Status::invalid_argument, {}};
        auto file = std::make_shared<OpenFile>();
#if defined(_WIN32)
        file->file = _wfsopen(native.c_str(), fs::path(mode).c_str(), _SH_DENYNO);   // share with every other opener: sharing rules are the Win32 layer's job
#else
        file->file = std::fopen(native.c_str(), mode);
#endif
        if (!file->file) return {exists ? Status::io_error : Status::not_found, {}};
        file->native = native;
        file->append = flags & open_flags::append;
        file->can_write = want_write;
        if (file->append) seek64(file->file, 0, SEEK_END);
        std::lock_guard lock(m_);
        const uint32_t id = next_++;
        files_[id] = file;
        return {Status::ok, FileHandle{id}};
    }
    Status close(FileHandle h) override {
        std::lock_guard lock(m_);
        return files_.erase(h.value) ? Status::ok : Status::invalid_argument;
    }
    Status read(FileHandle h, MutableBytes out, size_t& read) override {
        auto f = file_of(h);
        read = 0;
        if (!f) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        read = std::fread(out.data(), 1, out.size(), f->file);
        return Status::ok;
    }
    Status write(FileHandle h, Bytes data, size_t& written) override {
        auto f = file_of(h);
        written = 0;
        if (!f || !f->can_write) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        if (f->append) seek64(f->file, 0, SEEK_END);
        written = std::fwrite(data.data(), 1, data.size(), f->file);
        return written == data.size() ? Status::ok : Status::io_error;
    }
    Status read_at(FileHandle h, uint64_t offset, MutableBytes out, size_t& read) override {
        auto f = file_of(h);
        read = 0;
        if (!f) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        const int64_t saved = tell64(f->file);
        seek64(f->file, int64_t(offset), SEEK_SET);
        read = std::fread(out.data(), 1, out.size(), f->file);
        seek64(f->file, saved, SEEK_SET);
        return Status::ok;
    }
    Status write_at(FileHandle h, uint64_t offset, Bytes data, size_t& written) override {
        auto f = file_of(h);
        written = 0;
        if (!f || !f->can_write) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        const int64_t saved = tell64(f->file);
        seek64(f->file, int64_t(offset), SEEK_SET);
        written = std::fwrite(data.data(), 1, data.size(), f->file);
        seek64(f->file, saved, SEEK_SET);
        return written == data.size() ? Status::ok : Status::io_error;
    }
    Status seek(FileHandle h, int64_t offset, SeekOrigin origin, uint64_t& position) override {
        auto f = file_of(h);
        if (!f) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        if (seek64(f->file, offset, origin == SeekOrigin::set ? SEEK_SET : origin == SeekOrigin::current ? SEEK_CUR : SEEK_END) != 0) return Status::invalid_argument;
        position = uint64_t(tell64(f->file));
        return Status::ok;
    }
    Status size(FileHandle h, uint64_t& out) override {
        auto f = file_of(h);
        if (!f) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        std::fflush(f->file);
        std::error_code ec;
        out = fs::file_size(f->native, ec);
        return ec ? Status::io_error : Status::ok;
    }
    Status set_size(FileHandle h, uint64_t size) override {
        auto f = file_of(h);
        if (!f || !f->can_write) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        std::fflush(f->file);
#if defined(_WIN32)
        return _chsize_s(_fileno(f->file), int64_t(size)) == 0 ? Status::ok : Status::io_error;   // through the open descriptor: resizing by name fails on a file that is open
#else
        return ftruncate(fileno(f->file), off_t(size)) == 0 ? Status::ok : Status::io_error;
#endif
    }
    Status flush(FileHandle h) override {
        auto f = file_of(h);
        if (!f) return Status::invalid_argument;
        std::lock_guard lock(f->m);
        return std::fflush(f->file) == 0 ? Status::ok : Status::io_error;
    }

    Status stat(std::string_view p, StatInfo& out) override {
        fs::path native;
        const Status s = resolve_path(p, false, native);
        if (s != Status::ok) return s;
        return stat_native(native, out);
    }
    Status create_directory(std::string_view p) override {
        if (!writable_path(p)) return Status::invalid_argument;
        fs::path native;
        const Status s = resolve_path(p, true, native);
        if (s != Status::ok && s != Status::not_found) return s;
        std::error_code ec;
        fs::create_directories(native, ec);
        return ec ? Status::io_error : Status::ok;
    }
    Status remove(std::string_view p) override {
        if (!writable_path(p)) return Status::invalid_argument;
        fs::path native;
        const Status s = resolve_path(p, false, native);
        if (s != Status::ok) return s;
        std::error_code ec;
        if (fs::is_directory(native, ec) && !fs::is_empty(native, ec)) return Status::invalid_argument;
        return fs::remove(native, ec) ? Status::ok : Status::io_error;
    }
    Status rename(std::string_view from, std::string_view to) override {
        if (!writable_path(from) || !writable_path(to)) return Status::invalid_argument;
        fs::path a, b;
        Status s = resolve_path(from, false, a);
        if (s != Status::ok) return s;
        s = resolve_path(to, true, b);
        if (s != Status::ok && s != Status::not_found) return s;
        std::error_code ec;
        fs::rename(a, b, ec);
        return ec ? Status::io_error : Status::ok;
    }
    Status list(std::string_view p, std::vector<DirEntry>& out) override {
        out.clear();
        const auto split = path::split_root(p);
        std::vector<std::string> segments;
        if (!path::normalize_relative(split.rest, segments)) return Status::invalid_argument;
        std::vector<fs::path> directories;
        if (split.native) directories.push_back(fs::path(std::string(p)));
        else {
            std::lock_guard lock(m_);
            for (const auto& m : mounts_) if (m.root == split.root) directories.push_back(m.path);
            if (directories.empty()) return Status::not_found;
        }
        bool found = false;
        std::map<std::string, size_t> seen;
        for (auto base : directories) {
            if (!split.native) {
                fs::path walk = base;
                bool ok = true;
                for (const auto& seg : segments) { ok = step(walk, seg); if (!ok) break; }
                if (!ok) continue;
                base = walk;
            }
            std::error_code ec;
            if (!fs::is_directory(base, ec)) continue;
            found = true;
            for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
                DirEntry entry;
                entry.name = it->path().filename().string();
                stat_native(it->path(), entry.info);
                const std::string key = path::fold(entry.name);
                if (seen.count(key)) continue;
                seen[key] = out.size();
                out.push_back(std::move(entry));
            }
        }
        return found ? Status::ok : Status::not_found;
    }

private:
    struct Mount { std::string root; fs::path path; bool read_only; };

    // Moves `walk` to its child `name`, matching the existing entry ignoring case; the exact name when none matches (a path to be created).
    static bool step(fs::path& walk, const std::string& name) {
        std::error_code ec;
        fs::path exact = walk / fs::path(std::u8string(name.begin(), name.end()));
        if (fs::exists(exact, ec)) { walk = exact; return true; }
        for (fs::directory_iterator it(walk, ec), end; !ec && it != end; it.increment(ec)) {
            if (path::equal_folded(it->path().filename().string(), name)) { walk = it->path(); return true; }
        }
        walk = exact;
        return false;
    }

    Status resolve_path(std::string_view p, bool for_write, fs::path& out) {
        const auto split = path::split_root(p);
        std::vector<std::string> segments;
        if (!path::normalize_relative(split.rest, segments)) return Status::invalid_argument;
        if (split.native) {
            out = fs::path(std::string(p));
            std::error_code ec;
            return fs::exists(out, ec) ? Status::ok : Status::not_found;
        }
        std::lock_guard lock(m_);
        bool any_root = false;
        for (const auto& m : mounts_) {
            if (m.root != split.root) continue;
            any_root = true;
            fs::path walk = m.path;
            bool exists = true;
            for (const auto& seg : segments) if (!(exists = step(walk, seg))) break;
            if (exists) { out = walk; return Status::ok; }
        }
        if (!any_root) return Status::not_found;
        if (for_write) {   // a new entry goes to the first writable mount, below its parent as it exists there (ignoring case)
            for (const auto& m : mounts_) {
                if (m.root != split.root || m.read_only) continue;
                fs::path walk = m.path;
                for (const auto& seg : segments) step(walk, seg);
                out = walk;
                return Status::not_found;
            }
            return Status::invalid_argument;
        }
        return Status::not_found;
    }
    bool writable_path(std::string_view p) {
        const auto split = path::split_root(p);
        if (split.native) return true;
        std::lock_guard lock(m_);
        for (const auto& m : mounts_) if (m.root == split.root && !m.read_only) return true;
        return false;
    }
    static Status stat_native(const fs::path& native, StatInfo& out) {
        std::error_code ec;
        const auto status = fs::status(native, ec);
        if (ec || !fs::exists(status)) return Status::not_found;
        out = StatInfo{};
        out.kind = fs::is_directory(status) ? EntryKind::directory : fs::is_regular_file(status) ? EntryKind::file : EntryKind::other;
        if (out.kind == EntryKind::file) out.size = fs::file_size(native, ec);
        out.modified_unix_ms = out.created_unix_ms = out.accessed_unix_ms = to_unix_ms(fs::last_write_time(native, ec));
#if defined(_WIN32)
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) { out.read_only = attributes & FILE_ATTRIBUTE_READONLY; out.hidden = attributes & FILE_ATTRIBUTE_HIDDEN; }
#else
        out.read_only = (status.permissions() & fs::perms::owner_write) == fs::perms::none;
#endif
        return Status::ok;
    }
    std::shared_ptr<OpenFile> file_of(FileHandle h) {
        std::lock_guard lock(m_);
        auto it = files_.find(h.value);
        return it == files_.end() ? nullptr : it->second;
    }

    std::mutex m_;
    std::vector<Mount> mounts_;
    std::map<uint32_t, std::shared_ptr<OpenFile>> files_;
    uint32_t next_ = 1;
};
}  // namespace

std::unique_ptr<Storage> make_storage() { return std::make_unique<StdStorage>(); }
}  // namespace nue::legacy::platform::stdlib
