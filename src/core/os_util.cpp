// OS file system helpers. Windows: Win32 wide-character API. POSIX: open/pread/pwrite.
#include "internal/os_util.h"
#include "nue/vm.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/resource.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <TargetConditionals.h>
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

nue_result os_file_map(OsHandle h, uint64_t offset, size_t length, const void** out_data, OsMapping* out_mapping) {
    if (length == 0) return NUE_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uint64_t granularity = si.dwAllocationGranularity ? si.dwAllocationGranularity : 65536;
    const uint64_t aligned = offset - offset % granularity;
    const size_t delta = size_t(offset - aligned);
    HANDLE mapping = CreateFileMappingW(reinterpret_cast<HANDLE>(h), nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) return os_last_error();
    void* base = MapViewOfFile(mapping, FILE_MAP_READ, DWORD(aligned >> 32), DWORD(aligned & 0xFFFFFFFFu), delta + length);
    nue_result err = base ? NUE_OK : os_last_error();
    CloseHandle(mapping);   // the view keeps the mapping object alive
    if (!base) return err;
#else
    static const uint64_t page = uint64_t(::sysconf(_SC_PAGESIZE) > 0 ? ::sysconf(_SC_PAGESIZE) : 4096);
    const uint64_t aligned = offset - offset % page;
    const size_t delta = size_t(offset - aligned);
    void* base = ::mmap(nullptr, delta + length, PROT_READ, MAP_PRIVATE, int(h), off_t(aligned));
    if (base == MAP_FAILED) return os_last_error();
#endif
    out_mapping->base = base;
    out_mapping->length = delta + length;
    *out_data = static_cast<const uint8_t*>(base) + delta;
    return NUE_OK;
}

void os_file_unmap(const OsMapping& mapping) {
    if (!mapping.base) return;
#if defined(_WIN32)
    UnmapViewOfFile(mapping.base);
#else
    ::munmap(mapping.base, mapping.length);
#endif
}

// ---- virtual address space (nue/vm.h) --------------------------------------------------------------------------------------------------

#if defined(_WIN32)
namespace {
DWORD vm_page_protection(uint32_t p) {
    const bool x = (p & NUE_VM_EXEC) != 0, w = (p & NUE_VM_WRITE) != 0, r = (p & NUE_VM_READ) != 0;
    if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (w) return PAGE_READWRITE;
    return r ? PAGE_READONLY : PAGE_NOACCESS;
}
}  // namespace
#else
namespace {
int vm_prot(uint32_t p) {
    return ((p & NUE_VM_READ) ? PROT_READ : 0) | ((p & NUE_VM_WRITE) ? PROT_WRITE : 0) | ((p & NUE_VM_EXEC) ? PROT_EXEC : 0);
}
#  if defined(MAP_NORESERVE)
constexpr int kMapNoReserve = MAP_NORESERVE;
#  else
constexpr int kMapNoReserve = 0;
#  endif
bool vm_exec_allowed() {
#  if defined(__APPLE__) && TARGET_OS_IPHONE
    return false;   // iOS gives an app no executable memory it can write
#  else
    return true;
#  endif
}
}  // namespace
#endif

OsVmInfo os_vm_info() {
    OsVmInfo i;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    i.page_size = si.dwPageSize ? si.dwPageSize : 4096;
    i.granularity = si.dwAllocationGranularity ? si.dwAllocationGranularity : 65536;
    uint64_t top = uint64_t(reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress));
    uint32_t bits = 0;
    while (top) { ++bits; top >>= 1; }
    i.address_bits = bits ? bits : (sizeof(void*) == 8 ? 47 : 31);
#else
    long ps = ::sysconf(_SC_PAGESIZE);
    i.page_size = ps > 0 ? uint32_t(ps) : 4096;
    i.granularity = i.page_size;
    i.address_bits = sizeof(void*) == 8 ? 47 : 32;   // "at least": 48 bit user space on x86-64 / arm64 Linux with 4-level tables is 47 bits of positive addresses
    struct rlimit rl;
    if (::getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) i.address_limit = uint64_t(rl.rlim_cur);
    i.can_exec = vm_exec_allowed();
#endif
    return i;
}

nue_result os_vm_reserve(size_t size, size_t alignment, const void* hint, bool exact, void** out_base) {
    const OsVmInfo info = os_vm_info();
    if (alignment < info.granularity) alignment = info.granularity;
    if (hint && (reinterpret_cast<uintptr_t>(hint) % alignment) != 0) {
        if (exact) return NUE_ERR_INVALID_ARGUMENT;
        hint = nullptr;
    }
#if defined(_WIN32)
    auto reserve_at = [&](const void* where) -> void* {
        return VirtualAlloc(const_cast<void*>(where), size, MEM_RESERVE, PAGE_NOACCESS);
    };
    void* base = nullptr;
    if (hint) {
        base = reserve_at(hint);
        if (!base && exact) return NUE_ERR_OUT_OF_MEMORY;
    }
    if (!base && alignment <= info.granularity) base = reserve_at(nullptr);
    if (!base) {
        // VirtualAlloc2 (Windows 10 1803+) places an aligned reservation atomically
        using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
        static VirtualAlloc2Fn va2 = []() -> VirtualAlloc2Fn {
            HMODULE k = GetModuleHandleW(L"kernelbase.dll");
            return k ? reinterpret_cast<VirtualAlloc2Fn>(reinterpret_cast<void*>(GetProcAddress(k, "VirtualAlloc2"))) : nullptr;
        }();
        if (va2) {
            MEM_ADDRESS_REQUIREMENTS req = {};
            req.Alignment = alignment;
            MEM_EXTENDED_PARAMETER ep = {};
            ep.Type = MemExtendedParameterAddressRequirements;
            ep.Pointer = &req;
            base = va2(GetCurrentProcess(), nullptr, size, MEM_RESERVE, PAGE_NOACCESS, &ep, 1);
        }
    }
    if (!base) {
        // portable fallback: reserve a bigger range, release it and reserve the aligned part (another thread can take the hole in between: retry)
        for (int attempt = 0; attempt < 8 && !base; ++attempt) {
            if (size > SIZE_MAX - alignment) break;
            void* wide = VirtualAlloc(nullptr, size + alignment, MEM_RESERVE, PAGE_NOACCESS);
            if (!wide) break;
            uintptr_t aligned = (reinterpret_cast<uintptr_t>(wide) + alignment - 1) & ~(uintptr_t(alignment) - 1);
            VirtualFree(wide, 0, MEM_RELEASE);
            base = reserve_at(reinterpret_cast<void*>(aligned));
        }
    }
    if (!base) return NUE_ERR_OUT_OF_MEMORY;
    if ((reinterpret_cast<uintptr_t>(base) % alignment) != 0) { VirtualFree(base, 0, MEM_RELEASE); return NUE_ERR_OUT_OF_MEMORY; }
    *out_base = base;
    return NUE_OK;
#else
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS | kMapNoReserve;
    if (hint) {
        void* p = ::mmap(const_cast<void*>(hint), size, PROT_NONE, flags, -1, 0);
        if (p != MAP_FAILED && p == hint) { *out_base = p; return NUE_OK; }
        if (p != MAP_FAILED) ::munmap(p, size);
        if (exact) return NUE_ERR_OUT_OF_MEMORY;
    }
    const size_t wide = alignment > info.page_size ? size + alignment : size;
    if (wide < size) return NUE_ERR_OUT_OF_MEMORY;
    void* p = ::mmap(nullptr, wide, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) return NUE_ERR_OUT_OF_MEMORY;
    uintptr_t start = reinterpret_cast<uintptr_t>(p);
    uintptr_t aligned = (start + alignment - 1) & ~(uintptr_t(alignment) - 1);
    if (aligned > start) ::munmap(p, size_t(aligned - start));
    uintptr_t tail_begin = aligned + size, end = start + wide;
    if (end > tail_begin) ::munmap(reinterpret_cast<void*>(tail_begin), size_t(end - tail_begin));
    *out_base = reinterpret_cast<void*>(aligned);
    return NUE_OK;
#endif
}

nue_result os_vm_commit(void* address, size_t size, uint32_t protection) {
#if defined(_WIN32)
    if (!VirtualAlloc(address, size, MEM_COMMIT, vm_page_protection(protection))) return NUE_ERR_OUT_OF_MEMORY;
    return NUE_OK;
#else
    if ((protection & NUE_VM_EXEC) && !vm_exec_allowed()) return NUE_ERR_UNSUPPORTED;
    if (::mprotect(address, size, vm_prot(protection)) != 0) return errno == ENOMEM ? NUE_ERR_OUT_OF_MEMORY : NUE_ERR_IO;
    return NUE_OK;
#endif
}

nue_result os_vm_protect(void* address, size_t size, uint32_t protection) {
#if defined(_WIN32)
    DWORD old = 0;
    if (!VirtualProtect(address, size, vm_page_protection(protection), &old)) return NUE_ERR_ACCESS_DENIED;
    return NUE_OK;
#else
    if ((protection & NUE_VM_EXEC) && !vm_exec_allowed()) return NUE_ERR_UNSUPPORTED;
    if (::mprotect(address, size, vm_prot(protection)) != 0) return NUE_ERR_ACCESS_DENIED;
    return NUE_OK;
#endif
}

nue_result os_vm_decommit(void* address, size_t size) {
#if defined(_WIN32)
    if (!VirtualFree(address, size, MEM_DECOMMIT)) return NUE_ERR_IO;
    return NUE_OK;
#else
    // a fresh PROT_NONE mapping over the range drops the contents on every POSIX system (madvise semantics differ between Linux and macOS)
    void* p = ::mmap(address, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | kMapNoReserve, -1, 0);
    return p == MAP_FAILED ? NUE_ERR_IO : NUE_OK;
#endif
}

void os_vm_release(void* base, size_t size) {
#if defined(_WIN32)
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    ::munmap(base, size);
#endif
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

uint32_t os_process_id() {
#if defined(_WIN32)
    return uint32_t(GetCurrentProcessId());
#else
    return uint32_t(::getpid());
#endif
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
