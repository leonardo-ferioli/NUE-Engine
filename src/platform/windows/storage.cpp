#include "nue_win/storage.hpp"

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif

namespace nue::win {

namespace {
const KNOWNFOLDERID& folder_id(KnownFolder f) {
    switch (f) {
        case KnownFolder::SavedGames: return FOLDERID_SavedGames;
        case KnownFolder::LocalAppData: return FOLDERID_LocalAppData;
        case KnownFolder::RoamingAppData: return FOLDERID_RoamingAppData;
        case KnownFolder::Documents: return FOLDERID_Documents;
        case KnownFolder::ProgramData: return FOLDERID_ProgramData;
        case KnownFolder::Desktop: return FOLDERID_Desktop;
        case KnownFolder::Downloads: return FOLDERID_Downloads;
        default: return FOLDERID_LocalAppData;
    }
}

std::wstring env(const wchar_t* name) {
    std::wstring v(512, L'\0');
    DWORD n = GetEnvironmentVariableW(name, v.data(), DWORD(v.size()));
    if (n >= v.size()) { v.resize(n + 1); n = GetEnvironmentVariableW(name, v.data(), DWORD(v.size())); }
    v.resize(n);
    return v;
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
}  // namespace

Status known_folder_path(KnownFolder f, std::wstring& out) {
    if (f == KnownFolder::Temp) {
        std::wstring t(MAX_PATH + 1, L'\0');
        DWORD n = GetTempPath2W(DWORD(t.size()), t.data());
        if (n == 0) return Status::last_error("GetTempPath2");
        if (n >= t.size()) { t.resize(n + 1); n = GetTempPath2W(DWORD(t.size()), t.data()); }
        t.resize(n);
        while (t.size() > 3 && (t.back() == L'\\' || t.back() == L'/')) t.pop_back();
        out = t;
        return Status::success();
    }
    PWSTR p = nullptr;
    HRESULT hr = SHGetKnownFolderPath(folder_id(f), KF_FLAG_CREATE | KF_FLAG_DONT_UNEXPAND, nullptr, &p);
    if (FAILED(hr) || !p) { if (p) CoTaskMemFree(p); return Status::hresult("SHGetKnownFolderPath", hr); }
    out = p;
    CoTaskMemFree(p);
    return Status::success();
}

bool path_is_under_onedrive(const std::wstring& path) {
    for (const wchar_t* var : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
        std::wstring root = env(var);
        if (root.empty()) continue;
        std::wstring p = strip_extended_prefix(path);
        if (p.size() >= root.size() && CompareStringOrdinal(p.c_str(), int(root.size()), root.c_str(), int(root.size()), TRUE) == CSTR_EQUAL &&
            (p.size() == root.size() || p[root.size()] == L'\\' || p[root.size()] == L'/'))
            return true;
    }
    return false;
}

Status resolve_app_dirs(const std::string& vendor, const std::string& app, const AppDirOptions& o, AppDirs& out) {
    out = AppDirs{};
    const std::wstring rel = join(path_from_utf8(sanitize_file_name(vendor)), path_from_utf8(sanitize_file_name(app)));
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof exe / sizeof exe[0]));
    if (n && n < sizeof exe / sizeof exe[0]) { std::wstring e(exe, n); size_t k = e.find_last_of(L"\\/"); out.install = k == std::wstring::npos ? e : e.substr(0, k); }

    if (!o.root_override.empty()) {
        out.saves = join(o.root_override, L"saves");
        out.config = join(o.root_override, L"config");
        out.cache = join(o.root_override, L"cache");
        out.logs = join(o.root_override, L"logs");
        out.crash_dumps = join(o.root_override, L"crashdumps");
    } else {
        std::wstring saved, roaming, local;
        if (Status s = known_folder_path(KnownFolder::SavedGames, saved); !s.ok) {
            // Saved Games can be missing on stripped profiles; Documents is the documented fallback.
            if (Status d = known_folder_path(KnownFolder::Documents, saved); !d.ok) return s;
            saved = join(saved, L"My Games");
        }
        if (Status s = known_folder_path(KnownFolder::RoamingAppData, roaming); !s.ok) return s;
        if (Status s = known_folder_path(KnownFolder::LocalAppData, local); !s.ok) return s;
        out.saves = join(saved, rel);
        out.config = join(roaming, rel);
        const std::wstring local_app = join(local, rel);
        out.cache = join(local_app, L"cache");
        out.logs = join(local_app, L"logs");
        out.crash_dumps = join(local_app, L"crashdumps");
    }
    out.saves_on_onedrive = path_is_under_onedrive(out.saves);
    if (o.create) {
        for (const std::wstring* d : {&out.saves, &out.config, &out.cache, &out.logs, &out.crash_dumps})
            if (Status s = create_directories(*d); !s.ok) return s;
    }
    return Status::success();
}

// ---- Paths -------------------------------------------------------------------------------------------------------------------------------------------

std::wstring strip_extended_prefix(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0 && p.size() > 6 && p[5] == L':') return p.substr(4);
    return p;
}

std::wstring to_extended_path(const std::wstring& path) {
    if (path.empty()) return path;
    std::wstring p = path;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.rfind(L"\\\\?\\", 0) == 0 || p.rfind(L"\\\\.\\", 0) == 0) return p;   // already an extended/device path: GetFullPathName must not touch it
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n == 0) return p;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
    full.resize(n);
    if (full.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

bool long_paths_enabled() {
    using Fn = BOOLEAN(NTAPI*)();
    if (auto fn = load_proc<Fn>(L"ntdll.dll", "RtlAreLongPathsEnabled")) return fn() != FALSE;
    return false;
}

std::wstring path_from_utf8(const std::string& s) {
    std::wstring w = to_wide(s);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}
std::string path_to_utf8(const std::wstring& w) { return to_utf8(w); }

bool is_reserved_file_name(const std::string& name) {
    std::string base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    std::string up;
    for (char c : base) up += char(std::toupper((unsigned char)c));
    if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL") return true;
    if (up.size() == 4 && (up.rfind("COM", 0) == 0 || up.rfind("LPT", 0) == 0) && up[3] >= '1' && up[3] <= '9') return true;
    return false;
}

std::string sanitize_file_name(const std::string& name) {
    std::string out;
    for (char c : name) out += ((unsigned char)c < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) ? '_' : c;
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "_";
    if (is_reserved_file_name(out)) out.insert(out.begin(), '_');
    return out;
}

// ---- Case semantics ---------------------------------------------------------------------------------------------------------------------------------

CaseMode directory_case_mode(const std::wstring& dir) {
    struct IoStatus { LONG_PTR status; ULONG_PTR information; };
    struct CaseInfo { ULONG flags; };
    using Fn = LONG(NTAPI*)(HANDLE, IoStatus*, void*, ULONG, int);
    auto query = load_proc<Fn>(L"ntdll.dll", "NtQueryInformationFile");
    if (!query) return CaseMode::Unknown;
    HANDLE h = CreateFileW(to_extended_path(dir).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return CaseMode::Unknown;
    IoStatus iosb{};
    CaseInfo info{};
    const LONG st = query(h, &iosb, &info, sizeof info, 71 /*FileCaseSensitiveInformation*/);
    CloseHandle(h);
    if (st < 0) return CaseMode::Unknown;   // file systems without the information class (FAT, network): treat as the default, i.e. unknown
    return (info.flags & 1 /*FILE_CS_FLAG_CASE_SENSITIVE_DIR*/) ? CaseMode::Sensitive : CaseMode::Insensitive;
}

bool paths_equal_ignore_case(const std::wstring& a, const std::wstring& b) {
    std::wstring x = a, y = b;
    std::replace(x.begin(), x.end(), L'/', L'\\');
    std::replace(y.begin(), y.end(), L'/', L'\\');
    return x.size() == y.size() && CompareStringOrdinal(x.c_str(), int(x.size()), y.c_str(), int(y.size()), TRUE) == CSTR_EQUAL;
}

bool resolve_case_insensitive(const std::wstring& base, const std::wstring& relative, std::wstring& real) {
    std::wstring current = base;
    size_t pos = 0;
    while (pos < relative.size()) {
        size_t next = relative.find_first_of(L"\\/", pos);
        std::wstring part = relative.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        pos = next == std::wstring::npos ? relative.size() : next + 1;
        if (part.empty() || part == L".") continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(to_extended_path(join(current, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        do {
            if (paths_equal_ignore_case(fd.cFileName, part)) { current = join(current, fd.cFileName); found = true; break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!found) return false;
    }
    real = current;
    return true;
}

// ---- Files ---------------------------------------------------------------------------------------------------------------------------------------------

Status create_directories(const std::wstring& path) {
    const std::wstring ext = to_extended_path(path);
    const DWORD a = GetFileAttributesW(ext.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) ? Status::success() : Status::fail("path exists and is a file", ERROR_ALREADY_EXISTS);
    size_t k = ext.find_last_of(L'\\');
    if (k != std::wstring::npos && k > 6) {
        if (Status s = create_directories(ext.substr(0, k)); !s.ok) return s;
    }
    if (!CreateDirectoryW(ext.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Status::last_error("CreateDirectory");
    return Status::success();
}

Status read_file(const std::wstring& path, std::vector<uint8_t>& out, uint64_t max_size) {
    HANDLE h = CreateFileW(to_extended_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status::last_error("CreateFile(read)");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || uint64_t(size.QuadPart) > max_size) { CloseHandle(h); return Status::fail("file too large", ERROR_FILE_TOO_LARGE); }
    out.resize(size_t(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, DWORD(std::min<size_t>(out.size() - done, 1u << 30)), &got, nullptr) || got == 0) { CloseHandle(h); return Status::last_error("ReadFile"); }
        done += got;
    }
    CloseHandle(h);
    return Status::success();
}

Status write_file_atomic(const std::wstring& path, const void* data, size_t size) {
    const std::wstring target = to_extended_path(path);
    const std::wstring temp = target + L".nue-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status::last_error("CreateFile(temp)");
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        DWORD put = 0;
        if (!WriteFile(h, p + done, DWORD(std::min<size_t>(size - done, 1u << 30)), &put, nullptr)) { Status s = Status::last_error("WriteFile"); CloseHandle(h); DeleteFileW(temp.c_str()); return s; }
        done += put;
    }
    if (!FlushFileBuffers(h)) { Status s = Status::last_error("FlushFileBuffers"); CloseHandle(h); DeleteFileW(temp.c_str()); return s; }
    CloseHandle(h);
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Status s = Status::last_error("MoveFileEx(replace)");
        DeleteFileW(temp.c_str());
        return s;
    }
    return Status::success();
}

Status remove_tree(const std::wstring& path) {
    const std::wstring ext = to_extended_path(path);
    const DWORD a = GetFileAttributesW(ext.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? Status::success() : Status::last_error("GetFileAttributes");
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesW(ext.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (a & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(ext.c_str()) ? Status::success() : Status::last_error("RemoveDirectory(link)");   // never follow links out of the tree
        return DeleteFileW(ext.c_str()) ? Status::success() : Status::last_error("DeleteFile");
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ext + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (Status s = remove_tree(ext + L"\\" + fd.cFileName); !s.ok) { FindClose(h); return s; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(ext.c_str()) ? Status::success() : Status::last_error("RemoveDirectory");
}

Status disk_space(const std::wstring& path, DiskSpace& out) {
    ULARGE_INTEGER free_user{}, total{}, total_free{};
    std::wstring dir = to_extended_path(path);
    if (!GetDiskFreeSpaceExW(dir.c_str(), &free_user, &total, &total_free)) return Status::last_error("GetDiskFreeSpaceEx");
    out.free_for_user = free_user.QuadPart;
    out.total = total.QuadPart;
    return Status::success();
}

bool file_is_cloud_placeholder(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(to_extended_path(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE));
}

}  // namespace nue::win
