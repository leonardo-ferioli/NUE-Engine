#include "nue_legacy/win32/files.hpp"
#include "nue_legacy/platform/storage_path.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

namespace nue::legacy::win32 {
namespace {

constexpr uint32_t kErrorNegativeSeek = 131, kErrorDriveLocked = 108, kFlagBackupSemantics = 0x02000000u;
constexpr uint64_t kUnixToFileTimeMs = 11644473600000ull;

uint64_t file_time_of(int64_t unix_ms) { return unix_ms <= 0 ? 0 : (uint64_t(unix_ms) + kUnixToFileTimeMs) * 10000ull; }
char upper_ascii(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

bool wants_read(uint32_t a) { return a & (access::generic_read | access::generic_all | access::file_read_data | access::generic_execute); }
bool wants_write(uint32_t a) { return a & (access::generic_write | access::generic_all | access::file_write_data | access::file_append_data); }

// A canonical Win32 path: a drive letter and the components below its root, in the case the caller wrote them.
struct Canonical {
    char drive = 'C';
    std::vector<std::string> parts;
    bool trailing_separator = false;
    bool has_wildcards = false;
    std::string text() const {
        std::string s(1, drive);
        s += ":\\";
        for (size_t i = 0; i < parts.size(); ++i) { if (i) s += '\\'; s += parts[i]; }
        return s;
    }
    std::string leaf() const { return parts.empty() ? std::string() : parts.back(); }
};

}  // namespace

struct FileSystem::Impl {
    struct OpenInfo { uint32_t access = 0, share = 0; const void* owner = nullptr; };
    std::mutex m;
    std::map<std::string, std::vector<OpenInfo>> open_files;     // folded Win32 path -> opens (share-mode checks)
    std::map<uint32_t, Handle> std_handles;
    std::string current_directory;
};

namespace {

struct FileObject : Object {
    FileObject(platform::Storage& s, platform::FileHandle h, uint32_t a, std::string key, std::weak_ptr<FileSystem::Impl> impl) : Object(ObjectKind::file), storage(s), file(h), desired_access(a), folded_path(std::move(key)), owner(std::move(impl)) {}
    ~FileObject() override;
    platform::Storage& storage;
    platform::FileHandle file;
    uint32_t desired_access;
    std::string folded_path;
    std::weak_ptr<FileSystem::Impl> owner;
    bool character_device = false;
    std::mutex m;
};

struct FindObject : Object {
    FindObject() : Object(ObjectKind::find) {}
    std::vector<FindData> entries;
    size_t next = 0;
    std::mutex m;
};

struct MappingObject : Object {
    MappingObject() : Object(ObjectKind::mapping) {}
    std::shared_ptr<FileObject> file;
    std::vector<std::byte> bytes;
    uint32_t protect = 0;
};

// A mapped view: where it points and what to write back.
struct View { std::shared_ptr<MappingObject> mapping; uint64_t offset = 0; size_t size = 0; bool write = false; std::vector<std::byte> data; };

}  // namespace

FileObject::~FileObject() {
    if (file) storage.close(file);
    if (auto impl = owner.lock()) {
        std::lock_guard lock(impl->m);
        auto it = impl->open_files.find(folded_path);
        if (it != impl->open_files.end()) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [this](const FileSystem::Impl::OpenInfo& o) { return o.owner == this; }), v.end());
            if (v.empty()) impl->open_files.erase(it);
        }
    }
}

namespace {
std::mutex g_view_mutex;
std::map<const std::byte*, View>& views() { static std::map<const std::byte*, View> v; return v; }
}  // namespace

FileSystem::FileSystem(platform::Storage& storage, HandleTable& handles, FileSystemConfig config) : impl_(std::make_shared<Impl>()), config_(std::move(config)), storage_(storage), handles_(handles) {
    impl_->current_directory = config_.current_directory;
    // Standard handles: character devices (the console), never tied to a file of the storage.
    for (uint32_t which : {std_handle::input, std_handle::output, std_handle::error}) {
        auto o = std::make_shared<FileObject>(storage_, platform::FileHandle{}, which == std_handle::input ? access::generic_read : access::generic_write, std::string(), impl_);
        o->character_device = true;
        impl_->std_handles[which] = handles_.add(o);
    }
}

// ------------------------------------------------------------------------------------------------------------------------------ paths
namespace {
// Canonicalises a Win32 path against `cwd` (itself canonical text). `allow_wildcards` lets '*' and '?' through in the last component.
Ret<Canonical> canonicalize(std::string_view input, const std::string& cwd, bool allow_wildcards) {
    std::string p(input);
    for (auto& c : p) if (c == '/') c = '\\';
    if (p.rfind("\\\\?\\", 0) == 0) p.erase(0, 4);
    else if (p.rfind("\\\\", 0) == 0) return fail<Canonical>(err::path_not_found);        // UNC paths: no network
    Canonical out;
    char cwd_drive = cwd.size() >= 2 && cwd[1] == ':' ? upper_ascii(cwd[0]) : 'C';
    std::vector<std::string> base;
    auto split = [](std::string_view s, std::vector<std::string>& v) {
        size_t i = 0;
        while (i <= s.size()) { size_t j = i; while (j < s.size() && s[j] != '\\') ++j; if (j > i) v.emplace_back(s.substr(i, j - i)); i = j + 1; }
    };
    std::string rest = p;
    bool absolute = false;
    if (p.size() >= 2 && p[1] == ':' && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))) {
        out.drive = upper_ascii(p[0]);
        rest = p.substr(2);
        absolute = !rest.empty() && rest[0] == '\\';
        if (!absolute && out.drive == cwd_drive) split(cwd.size() > 3 ? std::string_view(cwd).substr(3) : std::string_view(), base);   // "C:file" is relative to that drive's directory
    } else if (!p.empty() && p[0] == '\\') {
        out.drive = cwd_drive;
        absolute = true;
    } else {
        out.drive = cwd_drive;
        split(cwd.size() > 3 ? std::string_view(cwd).substr(3) : std::string_view(), base);
    }
    std::vector<std::string> parts;
    split(rest, parts);
    out.trailing_separator = !rest.empty() && rest.back() == '\\';
    std::vector<std::string> result = absolute ? std::vector<std::string>{} : base;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string c = parts[i];
        if (c == ".") continue;
        if (c == "..") { if (!result.empty()) result.pop_back(); continue; }
        while (!c.empty() && (c.back() == '.' || c.back() == ' ')) c.pop_back();   // trailing dots and spaces are not part of a Windows name
        if (c.empty()) { if (parts[i].find_first_not_of('.') == std::string::npos) continue; return fail<Canonical>(err::invalid_name); }
        for (char ch : c) {
            if (static_cast<unsigned char>(ch) < 32 || ch == '<' || ch == '>' || ch == '"' || ch == '|') return fail<Canonical>(err::invalid_name);
            if (ch == '*' || ch == '?') { if (!allow_wildcards || i + 1 != parts.size()) return fail<Canonical>(err::invalid_name); out.has_wildcards = true; }
            if (ch == ':') return fail<Canonical>(err::invalid_name);
        }
        result.push_back(std::move(c));
    }
    out.parts = std::move(result);
    return ok(out);
}
}  // namespace

Ret<std::string> FileSystem::to_storage_path(std::string_view win32_path, bool* trailing) {
    auto c = canonicalize(win32_path, impl_->current_directory, false);
    if (!c) return fail<std::string>(c.error);
    if (trailing) *trailing = c.value.trailing_separator;
    auto drive = config_.drives.find(c.value.drive);
    if (drive == config_.drives.end()) return ok(c.value.text());                // a native path, backslashes and all
    std::string out = drive->second;
    for (size_t i = 0; i < c.value.parts.size(); ++i) { if (i || (!out.empty() && out.back() != ':' && out.back() != '/')) out += '/'; out += c.value.parts[i]; }
    return ok(out);
}

Ret<std::string> FileSystem::get_full_path_name(std::string_view path) {
    if (path.empty()) return fail<std::string>(err::path_not_found);
    auto c = canonicalize(path, impl_->current_directory, true);
    if (!c) return fail<std::string>(c.error);
    std::string text = c.value.text();
    if (c.value.trailing_separator && !c.value.parts.empty()) text += '\\';
    return ok(text);
}

Ret<std::string> FileSystem::get_long_path_name(std::string_view path) {
    auto c = canonicalize(path, impl_->current_directory, false);
    if (!c) return fail<std::string>(c.error);
    // Walk the path in the storage so each component comes back in the case it has on disk.
    auto mapped = to_storage_path(path);
    platform::StatInfo info;
    if (storage_.stat(mapped.value, info) != platform::Status::ok) return fail<std::string>(err::file_not_found);
    std::string result(1, c.value.drive);
    result += ":";
    std::string walk;
    auto drive = config_.drives.find(c.value.drive);
    walk = drive == config_.drives.end() ? std::string(1, c.value.drive) + ":\\" : drive->second;
    for (const auto& part : c.value.parts) {
        std::vector<platform::DirEntry> entries;
        std::string actual = part;
        if (storage_.list(walk, entries) == platform::Status::ok)
            for (const auto& e : entries) if (platform::path::equal_folded(e.name, part)) { actual = e.name; break; }
        result += "\\" + actual;
        if (drive == config_.drives.end()) walk += (walk.back() == '\\' ? "" : "\\") + actual; else walk += (walk.back() == ':' || walk.back() == '/' ? "" : "/") + actual;
    }
    if (c.value.parts.empty()) result += "\\";
    return ok(result);
}

Ret<bool> FileSystem::set_current_directory(std::string_view path) {
    auto c = canonicalize(path, impl_->current_directory, false);
    if (!c) return fail<bool>(c.error);
    auto mapped = to_storage_path(path);
    platform::StatInfo info;
    if (storage_.stat(mapped.value, info) != platform::Status::ok || info.kind != platform::EntryKind::directory) return fail<bool>(err::path_not_found);
    impl_->current_directory = c.value.text();
    config_.current_directory = impl_->current_directory;
    return ok(true);
}

uint32_t FileSystem::get_drive_type(std::string_view root) {
    auto c = canonicalize(root, impl_->current_directory, false);
    if (!c || !c.value.parts.empty()) return 1;     // DRIVE_NO_ROOT_DIR
    return (config_.logical_drives_mask >> (c.value.drive - 'A')) & 1 ? 3 : 1;   // DRIVE_FIXED
}

Ret<FileSystem::DiskSpace> FileSystem::get_disk_free_space(std::string_view root) {
    if (get_drive_type(root) != 3) return fail<DiskSpace>(err::path_not_found);
    DiskSpace s;
    s.total = 512ull << 30; s.total_free = s.free_to_caller = 256ull << 30;
    return ok(s);
}

// ------------------------------------------------------------------------------------------------------------------------------ files
namespace {
platform::Status open_in_storage(platform::Storage& storage, const std::string& path, uint32_t flags, platform::FileHandle& out) {
    auto r = storage.open(path, flags);
    out = r.value;
    return r.status;
}
uint32_t error_of(platform::Status s, uint32_t fallback) {
    switch (s) {
    case platform::Status::ok: return 0;
    case platform::Status::not_found: return err::file_not_found;
    case platform::Status::already_exists: return err::file_exists;
    case platform::Status::invalid_argument: return err::access_denied;
    case platform::Status::out_of_memory: return err::not_enough_memory;
    default: return fallback;
    }
}
}  // namespace

Ret<Handle> FileSystem::create_file(std::string_view path, uint32_t desired_access, uint32_t share_mode, uint32_t disp, uint32_t flags_and_attributes) {
    if (path.empty()) return fail<Handle>(err::path_not_found, kInvalidHandle);
    auto canon = canonicalize(path, impl_->current_directory, false);
    if (!canon) return fail<Handle>(canon.error, kInvalidHandle);
    if (disp < disposition::create_new || disp > disposition::truncate_existing) return fail<Handle>(err::invalid_parameter, kInvalidHandle);
    auto mapped = to_storage_path(path);
    if (!mapped) return fail<Handle>(mapped.error, kInvalidHandle);
    if (canon.value.parts.empty()) return fail<Handle>(err::access_denied, kInvalidHandle);   // a drive root is not a file
    platform::StatInfo info;
    const bool exists = storage_.stat(mapped.value, info) == platform::Status::ok;
    if (exists && info.kind == platform::EntryKind::directory) {
        if (disp == disposition::create_new || disp == disposition::create_always) return fail<Handle>(disp == disposition::create_new ? err::file_exists : err::access_denied, kInvalidHandle);
        return fail<Handle>(err::access_denied, kInvalidHandle);                          // a directory opens only with FILE_FLAG_BACKUP_SEMANTICS (not supported)
    }
    // The parent has to exist: that is what separates "file not found" from "path not found".
    if (!exists) {
        Canonical parent = canon.value;
        parent.parts.pop_back();
        std::string parent_text = parent.text();
        auto parent_mapped = to_storage_path(parent_text);
        platform::StatInfo pinfo;
        if (!parent.parts.empty() && (storage_.stat(parent_mapped.value, pinfo) != platform::Status::ok || pinfo.kind != platform::EntryKind::directory)) return fail<Handle>(err::path_not_found, kInvalidHandle);
        if (parent.parts.empty() && config_.drives.count(canon.value.drive) == 0 && !((config_.logical_drives_mask >> (canon.value.drive - 'A')) & 1)) return fail<Handle>(err::path_not_found, kInvalidHandle);
    }
    // Disposition rules.
    uint32_t last = 0;                      // the error left behind by a successful call (ERROR_ALREADY_EXISTS)
    uint32_t flags = 0;
    switch (disp) {
    case disposition::create_new:
        if (exists) return fail<Handle>(err::file_exists, kInvalidHandle);
        flags = platform::open_flags::write | platform::open_flags::create | platform::open_flags::exclusive;
        break;
    case disposition::create_always:
        if (exists) { last = err::already_exists; if (!wants_write(desired_access) && false) return fail<Handle>(err::access_denied, kInvalidHandle); }
        flags = platform::open_flags::write | platform::open_flags::create | platform::open_flags::truncate;
        break;
    case disposition::open_existing:
        if (!exists) return fail<Handle>(err::file_not_found, kInvalidHandle);
        flags = wants_write(desired_access) ? platform::open_flags::write : platform::open_flags::read;
        break;
    case disposition::open_always:
        if (exists) last = err::already_exists;
        flags = exists ? (wants_write(desired_access) ? platform::open_flags::write : platform::open_flags::read) : (platform::open_flags::write | platform::open_flags::create);
        break;
    case disposition::truncate_existing:
        if (!exists) return fail<Handle>(err::file_not_found, kInvalidHandle);
        if (!wants_write(desired_access)) return fail<Handle>(err::invalid_parameter, kInvalidHandle);
        flags = platform::open_flags::write | platform::open_flags::truncate;
        break;
    }
    if (exists && (flags & platform::open_flags::write) && info.read_only) return fail<Handle>(err::access_denied, kInvalidHandle);
    // Share modes: the new open and the existing ones must allow each other.
    const std::string key = platform::path::fold(canon.value.text());
    {
        std::lock_guard lock(impl_->m);
        auto it = impl_->open_files.find(key);
        if (it != impl_->open_files.end()) {
            for (const auto& o : it->second) {
                const bool new_read = wants_read(desired_access), new_write = wants_write(desired_access);
                if ((new_read && !(o.share & share::read)) || (new_write && !(o.share & share::write)) || (wants_read(o.access) && !(share_mode & share::read)) || (wants_write(o.access) && !(share_mode & share::write)))
                    return fail<Handle>(err::sharing_violation, kInvalidHandle);
            }
        }
    }
    platform::FileHandle file;
    const platform::Status s = open_in_storage(storage_, mapped.value, flags, file);
    if (s != platform::Status::ok) return fail<Handle>(error_of(s, err::access_denied), kInvalidHandle);
    auto object = std::make_shared<FileObject>(storage_, file, desired_access, key, impl_);
    {
        std::lock_guard lock(impl_->m);
        impl_->open_files[key].push_back({desired_access, share_mode, object.get()});
    }
    const Handle h = handles_.add(object);
    if (!h) return fail<Handle>(err::too_many_open_files, kInvalidHandle);
    return Ret<Handle>{h, last};
}

bool FileSystem::is_file_handle(Handle h) const { return handles_.get_as<FileObject>(h, ObjectKind::file) != nullptr; }

Ret<uint32_t> FileSystem::read_file(Handle h, MutableBytes out) {
    auto f = handles_.get_as<FileObject>(h, ObjectKind::file);
    if (!f) return fail<uint32_t>(err::invalid_handle);
    if (!wants_read(f->desired_access)) return fail<uint32_t>(err::access_denied);
    if (f->character_device) return ok(0u);
    size_t n = 0;
    std::lock_guard lock(f->m);
    const platform::Status s = f->storage.read(f->file, out, n);
    if (s != platform::Status::ok) return fail<uint32_t>(err::invalid_handle);
    return ok(uint32_t(n));
}

Ret<uint32_t> FileSystem::write_file(Handle h, Bytes data) {
    auto f = handles_.get_as<FileObject>(h, ObjectKind::file);
    if (!f) return fail<uint32_t>(err::invalid_handle);
    if (!wants_write(f->desired_access)) return fail<uint32_t>(err::access_denied);
    if (f->character_device) return ok(uint32_t(data.size()));            // console output goes nowhere here
    size_t n = 0;
    std::lock_guard lock(f->m);
    const platform::Status s = f->storage.write(f->file, data, n);
    if (s != platform::Status::ok) return Ret<uint32_t>{uint32_t(n), err::access_denied};
    return ok(uint32_t(n));
}

Ret<uint64_t> FileSystem::set_file_pointer(Handle h, int64_t distance, uint32_t method) {
    auto f = handles_.get_as<FileObject>(h, ObjectKind::file);
    if (!f) return fail<uint64_t>(err::invalid_handle, kInvalidFilePointer);
    if (method > 2) return fail<uint64_t>(err::invalid_parameter, kInvalidFilePointer);
    if (f->character_device) return fail<uint64_t>(err::invalid_handle, kInvalidFilePointer);
    uint64_t position = 0;
    std::lock_guard lock(f->m);
    // Seeking before the start is an error; the storage would clamp or refuse, so check against the current position first.
    uint64_t current = 0, size = 0;
    f->storage.seek(f->file, 0, platform::SeekOrigin::current, current);
    f->storage.size(f->file, size);
    const int64_t base = method == seek_method::begin ? 0 : method == seek_method::current ? int64_t(current) : int64_t(size);
    if (base + distance < 0) return fail<uint64_t>(kErrorNegativeSeek, kInvalidFilePointer);
    const platform::Status s = f->storage.seek(f->file, base + distance, platform::SeekOrigin::set, position);
    if (s != platform::Status::ok) return fail<uint64_t>(err::invalid_parameter, kInvalidFilePointer);
    return ok(position);
}

Ret<uint64_t> FileSystem::get_file_size(Handle h) {
    auto f = handles_.get_as<FileObject>(h, ObjectKind::file);
    if (!f) return fail<uint64_t>(err::invalid_handle, kInvalidFilePointer);
    uint64_t size = 0;
    std::lock_guard lock(f->m);
    if (f->character_device) return ok(0ull);
    f->storage.size(f->file, size);
    return ok(size);
}

Ret<bool> FileSystem::set_end_of_file(Handle h) {
    auto f = handles_.get_as<FileObject>(h, ObjectKind::file);
    if (!f) return fail<bool>(err::invalid_handle);
    if (!wants_write(f->desired_access)) return fail<bool>(err::access_denied);
    uint64_t current = 0;
    std::lock_guard lock(f->m);
    f->storage.seek(f->file, 0, platform::SeekOrigin::current, current);
    return f->storage.set_size(f->file, current) == platform::Status::ok ? ok(true) : fail<bool>(err::access_denied);
}

Ret<bool> FileSystem::flush_file_buffers(Handle h) {
    auto f = handles_.get_as<FileObject>(h, ObjectKind::file);
    if (!f) return fail<bool>(err::invalid_handle);
    if (!wants_write(f->desired_access)) return fail<bool>(err::access_denied);
    std::lock_guard lock(f->m);
    if (!f->character_device) f->storage.flush(f->file);
    return ok(true);
}

uint32_t FileSystem::get_file_type(Handle h) {
    auto f = handles_.get_as<FileObject>(h, ObjectKind::file);
    if (!f) return file_type::unknown;
    return f->character_device ? file_type::character : file_type::disk;
}

Ret<bool> FileSystem::delete_file(std::string_view path) {
    auto mapped = to_storage_path(path);
    if (!mapped) return fail<bool>(mapped.error);
    platform::StatInfo info;
    if (storage_.stat(mapped.value, info) != platform::Status::ok) {
        auto canon = canonicalize(path, impl_->current_directory, false);
        Canonical parent = canon.value;
        if (!parent.parts.empty()) parent.parts.pop_back();
        platform::StatInfo pinfo;
        const bool parent_ok = parent.parts.empty() || storage_.stat(to_storage_path(parent.text()).value, pinfo) == platform::Status::ok;
        return fail<bool>(parent_ok ? err::file_not_found : err::path_not_found);
    }
    if (info.kind == platform::EntryKind::directory) return fail<bool>(err::access_denied);
    if (info.read_only) return fail<bool>(err::access_denied);
    {
        auto canon = canonicalize(path, impl_->current_directory, false);
        std::lock_guard lock(impl_->m);
        if (impl_->open_files.count(platform::path::fold(canon.value.text()))) return fail<bool>(err::sharing_violation);
    }
    return storage_.remove(mapped.value) == platform::Status::ok ? ok(true) : fail<bool>(err::access_denied);
}

Ret<bool> FileSystem::move_file(std::string_view from, std::string_view to) {
    auto a = to_storage_path(from), b = to_storage_path(to);
    if (!a) return fail<bool>(a.error);
    if (!b) return fail<bool>(b.error);
    platform::StatInfo info;
    if (storage_.stat(a.value, info) != platform::Status::ok) return fail<bool>(err::file_not_found);
    if (storage_.stat(b.value, info) == platform::Status::ok) return fail<bool>(err::already_exists);   // MoveFile does not replace
    return storage_.rename(a.value, b.value) == platform::Status::ok ? ok(true) : fail<bool>(err::access_denied);
}

Ret<bool> FileSystem::create_directory(std::string_view path) {
    auto canon = canonicalize(path, impl_->current_directory, false);
    if (!canon) return fail<bool>(canon.error);
    auto mapped = to_storage_path(path);
    platform::StatInfo info;
    if (canon.value.parts.empty() || storage_.stat(mapped.value, info) == platform::Status::ok) return fail<bool>(err::already_exists);
    Canonical parent = canon.value;
    parent.parts.pop_back();
    platform::StatInfo pinfo;
    if (!parent.parts.empty() && (storage_.stat(to_storage_path(parent.text()).value, pinfo) != platform::Status::ok || pinfo.kind != platform::EntryKind::directory)) return fail<bool>(err::path_not_found);
    return storage_.create_directory(mapped.value) == platform::Status::ok ? ok(true) : fail<bool>(err::access_denied);
}

Ret<bool> FileSystem::remove_directory(std::string_view path) {
    auto mapped = to_storage_path(path);
    if (!mapped) return fail<bool>(mapped.error);
    platform::StatInfo info;
    if (storage_.stat(mapped.value, info) != platform::Status::ok) return fail<bool>(err::file_not_found);   // Windows reports path-not-found for a missing leaf too; see the tests
    if (info.kind != platform::EntryKind::directory) return fail<bool>(267);                                // ERROR_DIRECTORY
    std::vector<platform::DirEntry> entries;
    storage_.list(mapped.value, entries);
    if (!entries.empty()) return fail<bool>(err::dir_not_empty);
    return storage_.remove(mapped.value) == platform::Status::ok ? ok(true) : fail<bool>(err::access_denied);
}

uint32_t FileSystem::get_file_attributes(std::string_view path) {
    last_error = 0;
    auto canon = canonicalize(path, impl_->current_directory, false);
    if (!canon) { last_error = canon.error; return attribute::invalid; }
    if (canon.value.parts.empty()) return attribute::directory;
    auto mapped = to_storage_path(path);
    platform::StatInfo info;
    if (storage_.stat(mapped.value, info) != platform::Status::ok) {
        Canonical parent = canon.value;
        parent.parts.pop_back();
        platform::StatInfo pinfo;
        const bool parent_exists = parent.parts.empty() || storage_.stat(to_storage_path(parent.text()).value, pinfo) == platform::Status::ok;
        last_error = parent_exists ? err::file_not_found : err::path_not_found;   // a missing directory on the way is a different error from a missing leaf
        return attribute::invalid;
    }
    uint32_t a = info.kind == platform::EntryKind::directory ? attribute::directory : attribute::archive;
    if (info.read_only) a |= attribute::readonly;
    if (info.hidden) a |= attribute::hidden;
    return a;
}

// ------------------------------------------------------------------------------------------------------------------------------ find
bool wildcard_match(std::string_view pattern, std::string_view name) {
    if (pattern == "*.*" || pattern == "*") return true;
    const std::string p = platform::path::fold(pattern), n = platform::path::fold(name);
    // Classic iterative wildcard matcher with backtracking on '*'.
    size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) { ++pi; ++ni; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

Ret<Handle> FileSystem::find_first_file(std::string_view pattern, FindData& out) {
    if (pattern.empty()) return fail<Handle>(err::path_not_found, kInvalidHandle);
    auto canon = canonicalize(pattern, impl_->current_directory, true);
    if (!canon) return fail<Handle>(canon.error, kInvalidHandle);
    if (canon.value.parts.empty()) return fail<Handle>(err::file_not_found, kInvalidHandle);
    Canonical dir = canon.value;
    const std::string leaf = dir.parts.back();
    dir.parts.pop_back();
    auto dir_path = to_storage_path(dir.text());
    std::vector<platform::DirEntry> entries;
    const platform::Status s = storage_.list(dir_path.value, entries);
    if (s != platform::Status::ok) return fail<Handle>(dir.parts.empty() ? err::path_not_found : err::path_not_found, kInvalidHandle);
    auto object = std::make_shared<FindObject>();
    auto make = [&](const platform::DirEntry& e) {
        FindData d;
        d.name = e.name;
        d.attributes = e.info.kind == platform::EntryKind::directory ? attribute::directory : attribute::archive;
        if (e.info.read_only) d.attributes |= attribute::readonly;
        if (e.info.hidden) d.attributes |= attribute::hidden;
        d.size = e.info.kind == platform::EntryKind::directory ? 0 : e.info.size;
        d.creation_time = file_time_of(e.info.created_unix_ms);
        d.access_time = file_time_of(e.info.accessed_unix_ms);
        d.write_time = file_time_of(e.info.modified_unix_ms);
        return d;
    };
    std::vector<platform::DirEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const platform::DirEntry& a, const platform::DirEntry& b) { return platform::path::fold(a.name) < platform::path::fold(b.name); });
    if (!dir.parts.empty()) {   // a directory other than a drive's root lists "." and ".." first
        platform::DirEntry dot;
        dot.info.kind = platform::EntryKind::directory;
        dot.name = ".";
        if (wildcard_match(leaf, ".")) object->entries.push_back(make(dot));
        dot.name = "..";
        if (wildcard_match(leaf, "..")) object->entries.push_back(make(dot));
    }
    for (const auto& e : sorted) if (wildcard_match(leaf, e.name)) object->entries.push_back(make(e));
    if (object->entries.empty()) return fail<Handle>(err::file_not_found, kInvalidHandle);
    out = object->entries[0];
    object->next = 1;
    const Handle h = handles_.add(object);
    if (!h) return fail<Handle>(err::too_many_open_files, kInvalidHandle);
    return ok(h);
}

Ret<bool> FileSystem::find_next_file(Handle h, FindData& out) {
    auto f = handles_.get_as<FindObject>(h, ObjectKind::find);
    if (!f) return fail<bool>(err::invalid_handle);
    std::lock_guard lock(f->m);
    if (f->next >= f->entries.size()) return fail<bool>(err::no_more_files);
    out = f->entries[f->next++];
    return ok(true);
}

bool FileSystem::find_close(Handle h) {
    if (!handles_.get_as<FindObject>(h, ObjectKind::find)) return false;
    return handles_.close(h);
}

// ------------------------------------------------------------------------------------------------------------------------------ standard handles
Handle FileSystem::get_std_handle(uint32_t which) {
    if (which != std_handle::input && which != std_handle::output && which != std_handle::error) return kInvalidHandle;
    std::lock_guard lock(impl_->m);
    return impl_->std_handles[which];
}
bool FileSystem::set_std_handle(uint32_t which, Handle h) {
    if (which != std_handle::input && which != std_handle::output && which != std_handle::error) return false;
    std::lock_guard lock(impl_->m);
    impl_->std_handles[which] = h;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------------ mappings
Ret<Handle> FileSystem::create_file_mapping(Handle file, uint32_t protect, uint64_t maximum_size) {
    auto f = handles_.get_as<FileObject>(file, ObjectKind::file);
    if (!f) return fail<Handle>(err::invalid_handle);
    auto m = std::make_shared<MappingObject>();
    m->file = f;
    m->protect = protect;
    uint64_t size = 0;
    f->storage.size(f->file, size);
    const uint64_t total = maximum_size ? maximum_size : size;
    if (total == 0) return fail<Handle>(err::invalid_parameter);
    m->bytes.assign(size_t(total), std::byte{0});
    size_t n = 0;
    f->storage.read_at(f->file, 0, MutableBytes(m->bytes.data(), size_t(std::min<uint64_t>(size, total))), n);
    const Handle h = handles_.add(m);
    return h ? ok(h) : fail<Handle>(err::not_enough_memory);
}

Ret<MutableBytes> FileSystem::map_view_of_file(Handle mapping, bool write, uint64_t offset, size_t size) {
    auto m = handles_.get_as<MappingObject>(mapping, ObjectKind::mapping);
    if (!m) return fail<MutableBytes>(err::invalid_handle);
    if (offset > m->bytes.size()) return fail<MutableBytes>(err::invalid_parameter);
    const size_t length = size ? size : m->bytes.size() - size_t(offset);
    if (offset + length > m->bytes.size()) return fail<MutableBytes>(err::invalid_parameter);
    View view;
    view.mapping = m; view.offset = offset; view.size = length; view.write = write;
    view.data.assign(m->bytes.begin() + ptrdiff_t(offset), m->bytes.begin() + ptrdiff_t(offset + length));
    std::lock_guard lock(g_view_mutex);
    auto inserted = views().emplace(view.data.data(), std::move(view));
    return ok(MutableBytes(const_cast<std::byte*>(inserted.first->first), length));
}

Ret<bool> FileSystem::unmap_view_of_file(const std::byte* base) {
    View view;
    {
        std::lock_guard lock(g_view_mutex);
        auto it = views().find(base);
        if (it == views().end()) return fail<bool>(err::invalid_parameter);
        view = std::move(it->second);
        views().erase(it);
    }
    if (view.write) {
        std::memcpy(view.mapping->bytes.data() + view.offset, view.data.data(), view.size);
        size_t n = 0;
        auto& f = view.mapping->file;
        if (f && f->file) { std::lock_guard lock(f->m); f->storage.write_at(f->file, view.offset, Bytes(view.data.data(), view.size), n); }
    }
    return ok(true);
}

}  // namespace nue::legacy::win32
