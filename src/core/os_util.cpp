// OS file system helpers. Windows: Win32 wide-character API. POSIX: open/pread/pwrite.
#include "internal/os_util.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#endif

namespace fs = std::filesystem;

namespace nuei {

namespace {

fs::path to_path(const std::string& utf8) {
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), &w[0], n);
    return fs::path(w);
#else
    return fs::path(utf8);
#endif
}

std::string from_path(const fs::path& p) {
#if defined(_WIN32)
    const std::wstring& w = p.native();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

int64_t file_time_to_unix_ms(fs::file_time_type t) {
    // C++17 has no portable conversion: anchor both clocks at "now".
    auto sys_now = std::chrono::system_clock::now();
    auto file_now = fs::file_time_type::clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(t - file_now);
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys_now.time_since_epoch()).count() + delta.count();
}

}  // namespace

FILE* fopen_utf8(const std::string& path, const char* mode) {
#if defined(_WIN32)
    wchar_t wmode[8] = {};
    for (int i = 0; mode[i] && i < 7; ++i) wmode[i] = wchar_t(mode[i]);
    return _wfopen(to_path(path).c_str(), wmode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool os_remove_file(const std::string& path) {
    std::error_code ec;
    return fs::remove(to_path(path), ec) && !ec;
}

bool os_remove_dir(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(to_path(path), ec) && fs::remove(to_path(path), ec) && !ec;
}

bool os_remove_dir_tree(const std::string& path) {
    std::error_code ec;
    fs::remove_all(to_path(path), ec);
    return !ec;
}

bool os_rename_file(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    return MoveFileExW(to_path(from).c_str(), to_path(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool os_make_dirs(const std::string& path) {
    std::error_code ec;
    fs::path p = to_path(path);
    if (fs::is_directory(p, ec)) return true;
    fs::create_directories(p, ec);
    return fs::is_directory(p, ec);
}

bool os_make_parent_dirs(const std::string& path) {
    fs::path parent = to_path(path).parent_path();
    if (parent.empty()) return true;
    return os_make_dirs(from_path(parent));
}

void os_localtime(std::time_t t, std::tm* out) {
#if defined(_WIN32)
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

nue_result os_last_error() {
#if defined(_WIN32)
    switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: return NUE_ERR_NOT_FOUND;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_WRITE_PROTECT: return NUE_ERR_ACCESS_DENIED;
        case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return NUE_ERR_ALREADY_EXISTS;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return NUE_ERR_OUT_OF_MEMORY;
        case ERROR_HANDLE_EOF: return NUE_ERR_END_OF_DATA;
        default: return NUE_ERR_IO;
    }
#else
    switch (errno) {
        case ENOENT: case ENOTDIR: return NUE_ERR_NOT_FOUND;
        case EACCES: case EPERM: case EROFS: return NUE_ERR_ACCESS_DENIED;
        case EEXIST: return NUE_ERR_ALREADY_EXISTS;
        case ENOMEM: return NUE_ERR_OUT_OF_MEMORY;
        default: return NUE_ERR_IO;
    }
#endif
}

bool os_stat_path(const std::string& path, nue_stat* out) {
    std::error_code ec;
    fs::path p = to_path(path);
    fs::file_status st = fs::status(p, ec);
    if (ec || st.type() == fs::file_type::not_found) return false;
    NUE_STRUCT_INIT(out);
    out->kind = st.type() == fs::file_type::directory ? NUE_ENTRY_DIRECTORY : st.type() == fs::file_type::regular ? NUE_ENTRY_FILE : NUE_ENTRY_OTHER;
    if (out->kind == NUE_ENTRY_FILE) { auto sz = fs::file_size(p, ec); out->size = ec ? 0 : uint64_t(sz); }
    auto mt = fs::last_write_time(p, ec);
    out->modified_unix_ms = ec ? 0 : file_time_to_unix_ms(mt);
    out->created_unix_ms = out->modified_unix_ms;
    auto perms = st.permissions();
    out->read_only = (perms & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) == fs::perms::none;
    return true;
}

bool os_list_dir(const std::string& path, std::vector<OsDirEntry>* out) {
    std::error_code ec;
    fs::path p = to_path(path);
    if (!fs::is_directory(p, ec)) return false;
    out->clear();
    for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        OsDirEntry e;
        e.name = from_path(it->path().filename());
        std::error_code e2;
        fs::file_status st = it->status(e2);
        e.kind = st.type() == fs::file_type::directory ? NUE_ENTRY_DIRECTORY : st.type() == fs::file_type::regular ? NUE_ENTRY_FILE : NUE_ENTRY_OTHER;
        if (e.kind == NUE_ENTRY_FILE) { auto sz = it->file_size(e2); e.size = e2 ? 0 : uint64_t(sz); }
        auto mt = it->last_write_time(e2);
        e.modified_unix_ms = e2 ? 0 : file_time_to_unix_ms(mt);
        out->push_back(std::move(e));
    }
    return true;
}

// ---- raw file handles -----------------------------------------------------------------------------------------------------------

nue_result os_file_open(const std::string& path, uint32_t f, OsHandle* out) {
    *out = kBadOsHandle;
#if defined(_WIN32)
    DWORD access = 0;
    if (f & NUE_OPEN_READ) access |= GENERIC_READ;
    if (f & (NUE_OPEN_WRITE | NUE_OPEN_APPEND)) access |= GENERIC_WRITE;
    DWORD disp = OPEN_EXISTING;
    const bool writing = (f & (NUE_OPEN_WRITE | NUE_OPEN_APPEND)) != 0;
    if (writing && (f & NUE_OPEN_CREATE)) {
        if (f & NUE_OPEN_EXCLUSIVE) disp = CREATE_NEW;
        else if (f & NUE_OPEN_TRUNCATE) disp = CREATE_ALWAYS;
        else disp = OPEN_ALWAYS;
    } else if (writing && (f & NUE_OPEN_TRUNCATE)) {
        disp = TRUNCATE_EXISTING;
    }
    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    if (f & NUE_OPEN_SEQUENTIAL) attrs |= FILE_FLAG_SEQUENTIAL_SCAN;
    if (f & NUE_OPEN_RANDOM) attrs |= FILE_FLAG_RANDOM_ACCESS;
    HANDLE h = CreateFileW(to_path(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp, attrs, nullptr);
    if (h == INVALID_HANDLE_VALUE) return os_last_error();
    *out = reinterpret_cast<OsHandle>(h);
    return NUE_OK;
#else
    int flags = 0;
    const bool r = (f & NUE_OPEN_READ) != 0, w = (f & (NUE_OPEN_WRITE | NUE_OPEN_APPEND)) != 0;
    flags = (r && w) ? O_RDWR : w ? O_WRONLY : O_RDONLY;
    if (w && (f & NUE_OPEN_CREATE)) flags |= O_CREAT;
    if (w && (f & NUE_OPEN_EXCLUSIVE)) flags |= O_EXCL;
    if (w && (f & NUE_OPEN_TRUNCATE)) flags |= O_TRUNC;
#  ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#  endif
    int fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) return os_last_error();
    *out = fd;
    return NUE_OK;
#endif
}

void os_file_close(OsHandle h) {
    if (h == kBadOsHandle) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(h));
#else
    ::close(int(h));
#endif
}

nue_result os_file_read_at(OsHandle h, uint64_t offset, void* buffer, size_t size, size_t* out_read) {
    *out_read = 0;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    while (size > 0) {
#if defined(_WIN32)
        OVERLAPPED ov = {};
        ov.Offset = DWORD(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30)), got = 0;
        if (!ReadFile(reinterpret_cast<HANDLE>(h), dst, chunk, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return os_last_error();
        }
        if (got == 0) break;
#else
        ssize_t got = ::pread(int(h), dst, size, off_t(offset));
        if (got < 0) { if (errno == EINTR) continue; return os_last_error(); }
        if (got == 0) break;
#endif
        dst += got; offset += uint64_t(got); size -= size_t(got); *out_read += size_t(got);
    }
    return NUE_OK;
}

nue_result os_file_write_at(OsHandle h, uint64_t offset, const void* data, size_t size, size_t* out_written) {
    *out_written = 0;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    while (size > 0) {
#if defined(_WIN32)
        OVERLAPPED ov = {};
        ov.Offset = DWORD(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30)), put = 0;
        if (!WriteFile(reinterpret_cast<HANDLE>(h), src, chunk, &put, &ov)) return os_last_error();
#else
        ssize_t put = ::pwrite(int(h), src, size, off_t(offset));
        if (put < 0) { if (errno == EINTR) continue; return os_last_error(); }
#endif
        src += put; offset += uint64_t(put); size -= size_t(put); *out_written += size_t(put);
    }
    return NUE_OK;
}

nue_result os_file_size(OsHandle h, uint64_t* out) {
#if defined(_WIN32)
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(reinterpret_cast<HANDLE>(h), &sz)) return os_last_error();
    *out = uint64_t(sz.QuadPart);
#else
    struct stat st;
    if (::fstat(int(h), &st) != 0) return os_last_error();
    *out = uint64_t(st.st_size);
#endif
    return NUE_OK;
}

nue_result os_file_set_size(OsHandle h, uint64_t size) {
#if defined(_WIN32)
    LARGE_INTEGER pos; pos.QuadPart = LONGLONG(size);
    if (!SetFilePointerEx(reinterpret_cast<HANDLE>(h), pos, nullptr, FILE_BEGIN) || !SetEndOfFile(reinterpret_cast<HANDLE>(h))) return os_last_error();
#else
    if (::ftruncate(int(h), off_t(size)) != 0) return os_last_error();
#endif
    return NUE_OK;
}

nue_result os_file_sync(OsHandle h) {
#if defined(_WIN32)
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(h))) return os_last_error();
#else
    if (::fsync(int(h)) != 0) return os_last_error();
#endif
    return NUE_OK;
}

std::string os_executable_dir() {
    std::string dir;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    if (n) dir = from_path(fs::path(std::wstring(buf, n)).parent_path());
#elif defined(__linux__) || defined(__ANDROID__)
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; dir = fs::path(buf).parent_path().string(); }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) dir = fs::path(buf).parent_path().string();
#endif
    std::replace(dir.begin(), dir.end(), '\\', '/');
    return dir;
}

std::string os_temp_dir() {
    std::error_code ec;
    std::string t = from_path(fs::temp_directory_path(ec));
    std::replace(t.begin(), t.end(), '\\', '/');
    while (t.size() > 1 && t.back() == '/') t.pop_back();
    return t;
}

std::string os_getenv(const char* name) {
#if defined(_WIN32)
    wchar_t wname[128] = {};
    for (int i = 0; name[i] && i < 127; ++i) wname[i] = wchar_t(name[i]);
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(wname, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::string();
    return from_path(fs::path(std::wstring(buf, n)));
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

// Simple case folding on UTF-8: ASCII, Latin-1 supplement, Latin Extended-A (pairs), Greek and Cyrillic basic blocks.
std::string fold_case_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += char(c >= 'A' && c <= 'Z' ? c + 32 : c); ++i; continue; }
        uint32_t cp = 0; size_t len = 0;
        if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu); len = 3; }
        else { out += char(c); ++i; continue; }
        uint32_t f = cp;
        if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)) f = cp + 32;
        else if (cp >= 0x100 && cp <= 0x17F && cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149 && cp != 0x17F) {
            bool upper = (cp < 0x138) ? (cp % 2 == 0) : (cp > 0x138 && cp < 0x149 ? (cp % 2 == 1) : (cp > 0x149 && cp < 0x178 ? (cp % 2 == 0) : (cp >= 0x179 && cp < 0x17F ? (cp % 2 == 1) : false)));
            if (upper) f = cp + 1;
        }
        else if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) f = cp + 32;
        else if (cp >= 0x410 && cp <= 0x42F) f = cp + 32;
        else if (cp >= 0x400 && cp <= 0x40F) f = cp + 80;
        if (f < 0x80) { out += char(f); }
        else if (f < 0x800) { out += char(0xC0 | (f >> 6)); out += char(0x80 | (f & 0x3F)); }
        else { out += char(0xE0 | (f >> 12)); out += char(0x80 | ((f >> 6) & 0x3F)); out += char(0x80 | (f & 0x3F)); }
        i += len;
    }
    return out;
}

}  // namespace nuei
