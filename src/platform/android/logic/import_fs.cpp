// std::filesystem + stdio implementation of the import FileSystem, and the directory ImportSource.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "import_engine.hpp"

#if defined(_WIN32)
#include <io.h>
#define NUE_FSYNC(fd) _commit(fd)
#define NUE_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define NUE_FSYNC(fd) fsync(fd)
#define NUE_FILENO(f) fileno(f)
#endif

namespace nue::android {
namespace fs = std::filesystem;

namespace {

fs::path to_path(const std::string& s) { return fs::u8path(s); }

class StdWritable final : public WritableFile {
public:
    explicit StdWritable(FILE* f) : f_(f) {}
    ~StdWritable() override {
        if (f_) std::fclose(f_);
    }
    WriteStatus write(const void* data, size_t size) override {
        if (std::fwrite(data, 1, size, f_) == size) return WriteStatus::Ok;
        return errno == ENOSPC ? WriteStatus::NoSpace : WriteStatus::Error;
    }
    bool close(bool sync) override {
        bool ok = std::fflush(f_) == 0;
        if (ok && sync) ok = NUE_FSYNC(NUE_FILENO(f_)) == 0;
        ok = (std::fclose(f_) == 0) && ok;
        f_ = nullptr;
        return ok;
    }

private:
    FILE* f_;
};

class StdFileSystem final : public FileSystem {
public:
    bool make_dirs(const std::string& path) override {
        std::error_code ec;
        fs::create_directories(to_path(path), ec);
        return !ec;
    }
    std::unique_ptr<WritableFile> create(const std::string& path) override {
        FILE* f = std::fopen(to_path(path).string().c_str(), "wb");
        if (!f) return nullptr;
        return std::make_unique<StdWritable>(f);
    }
    bool rename(const std::string& from, const std::string& to) override {
        std::error_code ec;
        fs::rename(to_path(from), to_path(to), ec);
        if (ec) { // some platforms refuse to replace: remove and retry
            fs::remove(to_path(to), ec);
            ec.clear();
            fs::rename(to_path(from), to_path(to), ec);
        }
        return !ec;
    }
    bool remove(const std::string& path) override {
        std::error_code ec;
        fs::remove(to_path(path), ec);
        return !ec;
    }
    bool file_size(const std::string& path, uint64_t* size) override {
        std::error_code ec;
        if (!fs::is_regular_file(to_path(path), ec) || ec) return false;
        uint64_t s = fs::file_size(to_path(path), ec);
        if (ec) return false;
        *size = s;
        return true;
    }
    bool free_space(const std::string& path, uint64_t* bytes) override {
        std::error_code ec;
        fs::space_info si = fs::space(to_path(path), ec);
        if (ec) return false;
        *bytes = si.available;
        return true;
    }
    bool read_text(const std::string& path, std::string* out) override {
        std::ifstream in(to_path(path), std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        *out = ss.str();
        return true;
    }
    bool write_text_atomic(const std::string& path, const std::string& text) override {
        std::string tmp = path + ".tmp";
        std::unique_ptr<WritableFile> f = create(tmp);
        if (!f) return false;
        if (f->write(text.data(), text.size()) != WriteStatus::Ok) {
            f->close(false);
            remove(tmp);
            return false;
        }
        if (!f->close(true)) {
            remove(tmp);
            return false;
        }
        return rename(tmp, path);
    }
    bool append_text(const std::string& path, const std::string& text, bool sync) override {
        FILE* f = std::fopen(to_path(path).string().c_str(), "ab");
        if (!f) return false;
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = (std::fflush(f) == 0) && ok;
        if (ok && sync) ok = NUE_FSYNC(NUE_FILENO(f)) == 0;
        ok = (std::fclose(f) == 0) && ok;
        return ok;
    }
};

class FileByteSource final : public ByteSource {
public:
    explicit FileByteSource(FILE* f) : f_(f) {}
    ~FileByteSource() override {
        if (f_) std::fclose(f_);
    }
    int64_t read(void* dst, size_t size) override {
        size_t n = std::fread(dst, 1, size, f_);
        if (n == 0 && std::ferror(f_)) return -1;
        return static_cast<int64_t>(n);
    }

private:
    FILE* f_;
};

class DirectorySource final : public ImportSource {
public:
    explicit DirectorySource(const std::string& root) : root_(to_path(root)) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || e2) continue;
            SourceFile f;
            f.path = fs::relative(it->path(), root_, e2).generic_string();
            f.size = it->file_size(e2);
            if (e2) continue;
            files_.push_back(std::move(f));
        }
        std::sort(files_.begin(), files_.end(), [](const SourceFile& a, const SourceFile& b) { return a.path < b.path; });
    }
    const std::vector<SourceFile>& files() const override { return files_; }
    std::unique_ptr<ByteSource> open(size_t index) override {
        if (index >= files_.size()) return nullptr;
        FILE* f = std::fopen((root_ / fs::u8path(files_[index].path)).string().c_str(), "rb");
        if (!f) return nullptr;
        return std::make_unique<FileByteSource>(f);
    }

private:
    fs::path root_;
    std::vector<SourceFile> files_;
};

} // namespace

std::unique_ptr<FileSystem> make_std_file_system() { return std::make_unique<StdFileSystem>(); }
std::unique_ptr<ImportSource> make_directory_source(const std::string& root) {
    return std::make_unique<DirectorySource>(root);
}

} // namespace nue::android
