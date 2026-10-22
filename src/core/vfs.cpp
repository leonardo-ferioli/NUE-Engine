// Virtual file system: roots and mounts, path normalisation and case-insensitive resolution, file handle table, directories.
#include "internal/vfs_internal.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace nuei {

namespace {

struct Mount {
    std::string native;
    nue_mount_flags flags = 0;
};

struct DirCache {
    std::map<std::string, std::string> folded_to_actual;
    std::chrono::steady_clock::time_point built;   // real clock on purpose: nue_time_ns() may be a frozen virtual clock
};

struct VfsState {
    std::mutex mutex;                                     // mounts + directory cache
    std::map<std::string, std::vector<Mount>> roots;      // root name (lower case) -> mounts, highest priority first
    std::map<std::string, DirCache> dir_cache;

    std::mutex files_mutex;
    std::vector<std::shared_ptr<FileObj>> files;          // slot i -> id low half i+1
    std::vector<uint16_t> generations;
    std::vector<uint32_t> free_slots;

    std::string owned_temp;   // per-process scratch folder created by vfs_init (removed at shutdown); empty when the platform or portable mode provides it
};

VfsState& V() {
    static VfsState* v = new VfsState();
    return *v;
}

bool valid_root_name(const std::string& r) {
    if (r.empty()) return false;
    for (char c : r) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

std::string lower_ascii(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }

bool native_exists(const std::string& p) { nue_stat st; return os_stat_path(p, &st); }

bool native_is_dir(const std::string& p) { nue_stat st; return os_stat_path(p, &st) && st.kind == NUE_ENTRY_DIRECTORY; }

// Case-insensitive lookup of `name` inside `dir` through the directory cache. A miss triggers one fresh listing unless the cached one is younger than
// kTrustMs (repeated lookups of names that do not exist must not list a big directory every time). Everything the VFS itself creates, renames or removes
// invalidates the affected directories immediately, so only changes made behind the VFS's back can be missed, and only for kTrustMs.
bool match_in_dir(const std::string& dir, const std::string& name, std::string* actual) {
    constexpr int kTrustMs = 100;
    VfsState& v = V();
    const std::string key = fold_case_utf8(name);
    bool need_build = false;
    {
        std::lock_guard<std::mutex> lock(v.mutex);
        auto it = v.dir_cache.find(dir);
        if (it != v.dir_cache.end()) {
            auto f = it->second.folded_to_actual.find(key);
            if (f != it->second.folded_to_actual.end()) { *actual = f->second; return true; }
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - it->second.built).count();
            need_build = age >= kTrustMs;
            if (!need_build) return false;
        } else need_build = true;
    }
    std::vector<OsDirEntry> entries;
    if (!os_list_dir(dir, &entries)) return false;
    DirCache dc;
    for (const OsDirEntry& e : entries) dc.folded_to_actual.emplace(fold_case_utf8(e.name), e.name);
    dc.built = std::chrono::steady_clock::now();
    auto f = dc.folded_to_actual.find(key);
    const bool found = f != dc.folded_to_actual.end();
    if (found) *actual = f->second;
    std::lock_guard<std::mutex> lock(v.mutex);
    v.dir_cache[dir] = std::move(dc);
    return found;
}


// Drops the cached listings of the parent of `native` and of every ancestor above it (a parent directory may have been created by this very operation).
void invalidate_parent_of(const std::string& native) {
    VfsState& v = V();
    std::lock_guard<std::mutex> lock(v.mutex);
    for (auto it = v.dir_cache.begin(); it != v.dir_cache.end();) {
        const std::string& k = it->first;
        bool ancestor = native.size() > k.size() && native.compare(0, k.size(), k) == 0 && native[k.size()] == '/';
        if (ancestor) it = v.dir_cache.erase(it); else ++it;
    }
}

std::vector<std::string> split_segments(const std::string& rel) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        if (j > i) out.push_back(rel.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// Resolve `rel` below `base`. must_exist: fail if some segment cannot be found; otherwise unknown trailing segments are appended verbatim.
bool resolve_in_mount(const Mount& m, const std::string& rel, bool must_exist, std::string* out) {
    std::string cur = m.native;
    const std::vector<std::string> segs = split_segments(rel);
    for (size_t i = 0; i < segs.size(); ++i) {
        std::string cand = native_join(cur, segs[i]);
        if (native_exists(cand)) { cur = cand; continue; }
        std::string actual;
        if (!(m.flags & NUE_MOUNT_CASE_SENSITIVE) && match_in_dir(cur, segs[i], &actual)) { cur = native_join(cur, actual); continue; }
        if (must_exist) return false;
        for (size_t k = i; k < segs.size(); ++k) cur = native_join(cur, segs[k]);
        break;
    }
    *out = cur;
    return true;
}

const char* kWritableRoots[] = {"user", "cache", "temp", "settings"};

}  // namespace

std::string native_join(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (base.empty()) return rel;
    if (base.back() == '/' || base.back() == '\\') return base + rel;
    return base + "/" + rel;
}

bool normalize_relative(const std::string& rel, std::string* out) {
    std::vector<std::string> stack;
    std::string s = rel;
    std::replace(s.begin(), s.end(), '\\', '/');
    for (const std::string& seg : split_segments(s)) {
        if (seg == ".") continue;
        if (seg == "..") { if (stack.empty()) return false; stack.pop_back(); continue; }
        stack.push_back(seg);
    }
    std::string r;
    for (size_t i = 0; i < stack.size(); ++i) { if (i) r += '/'; r += stack[i]; }
    *out = r;
    return true;
}

bool split_root(const std::string& path, std::string* root, std::string* rel) {
    size_t colon = path.find(':');
    if (colon == std::string::npos || colon < 2) return false;   // "C:\x" has a one-letter prefix: native drive path
    std::string r = path.substr(0, colon);
    if (!valid_root_name(r)) return false;
    *root = lower_ascii(r);
    *rel = path.substr(colon + 1);
    return true;
}

nue_result resolve_path(const std::string& path, bool for_write, std::string* native_out) {
    std::string root, rel;
    if (!split_root(path, &root, &rel)) { *native_out = path; return NUE_OK; }   // native path: untouched
    std::string norm;
    if (!normalize_relative(rel, &norm)) return fail(NUE_ERR_ACCESS_DENIED, "path '%s' climbs above its root", path.c_str());
    std::vector<Mount> mounts;
    {
        VfsState& v = V();
        std::lock_guard<std::mutex> lock(v.mutex);
        auto it = v.roots.find(root);
        if (it == v.roots.end()) return fail(NUE_ERR_NOT_FOUND, "unknown storage root '%s' in path '%s'", root.c_str(), path.c_str());
        mounts = it->second;
    }
    if (for_write) {
        for (const Mount& m : mounts) {
            if (m.flags & NUE_MOUNT_READ_ONLY) continue;
            return resolve_in_mount(m, norm, false, native_out) ? NUE_OK : NUE_ERR_FAILED;
        }
        return fail(NUE_ERR_ACCESS_DENIED, "storage root '%s' is read-only", root.c_str());
    }
    for (const Mount& m : mounts) if (resolve_in_mount(m, norm, true, native_out)) return NUE_OK;
    return fail(NUE_ERR_NOT_FOUND, "'%s' was not found in root '%s'", norm.c_str(), root.c_str());
}

std::string resolve_native_path(const char* path, bool for_write) {
    if (!path) return std::string();
    std::string out;
    return NUE_SUCCEEDED(resolve_path(path, for_write, &out)) ? out : std::string();
}

std::shared_ptr<FileObj> file_from_id(nue_file id) {
    VfsState& v = V();
    std::lock_guard<std::mutex> lock(v.files_mutex);
    uint32_t idx = (id & 0xFFFFu);
    if (idx == 0 || idx > v.files.size()) return nullptr;
    if (v.generations[idx - 1] != (id >> 16)) return nullptr;
    return v.files[idx - 1];
}

// ---- roots at init ---------------------------------------------------------------------------------------------------------------

namespace {

void add_mount(const std::string& root, const std::string& native, nue_mount_flags flags, bool front) {
    std::string clean = native;
    std::replace(clean.begin(), clean.end(), '\\', '/');
    while (clean.size() > 1 && clean.back() == '/' && !(clean.size() == 3 && clean[1] == ':')) clean.pop_back();
    VfsState& v = V();
    std::lock_guard<std::mutex> lock(v.mutex);
    auto& list = v.roots[lower_ascii(root)];
    for (const Mount& m : list) if (m.native == clean) return;
    Mount m;
    m.native = clean;
    m.flags = flags;
    if (front) list.insert(list.begin(), m); else list.push_back(m);
}

std::string generic_user_dir(const InitInfo& info) {
#if defined(NUE_PLATFORM_WINDOWS)
    std::string base = os_getenv("LOCALAPPDATA");
    if (base.empty()) base = os_getenv("APPDATA");
#elif defined(NUE_PLATFORM_MACOS)
    std::string base = os_getenv("HOME");
    if (!base.empty()) base += "/Library/Application Support";
#else
    std::string base = os_getenv("XDG_DATA_HOME");
    if (base.empty() && !os_getenv("HOME").empty()) base = os_getenv("HOME") + "/.local/share";
#endif
    if (base.empty()) base = os_temp_dir();
    std::replace(base.begin(), base.end(), '\\', '/');
    return native_join(native_join(base, info.org_name), info.app_id);
}

std::string root_from_platform(nue_root_kind kind, const InitInfo& info) {
    const nue_platform_ops* p = platform();
    if (!p || !p->storage_root) return std::string();
    char buf[1024];
    size_t needed = 0;
    nue_init_desc d = info.desc;
    // the platform layer reads strings through the desc: provide the stored copies
    d.app_name = info.app_name.c_str(); d.app_id = info.app_id.c_str(); d.org_name = info.org_name.c_str();
    d.game_data_path = info.game_data_path.empty() ? nullptr : info.game_data_path.c_str();
    d.user_data_path = info.user_data_path.empty() ? nullptr : info.user_data_path.c_str();
    if (NUE_FAILED(p->storage_root(kind, &d, buf, sizeof(buf), &needed)) || needed == 0 || needed > sizeof(buf)) return std::string();
    return buf;
}

}  // namespace

void vfs_init(const InitInfo& info) {
    { VfsState& v = V(); std::lock_guard<std::mutex> lock(v.mutex); v.roots.clear(); v.dir_cache.clear(); v.owned_temp.clear(); }

    // data (read-only): explicit path > platform rule > folder of the executable
    std::string data = info.game_data_path;
    if (data.empty()) data = root_from_platform(NUE_ROOT_DATA, info);
    if (data.empty()) data = os_executable_dir();
    add_mount("data", data, NUE_MOUNT_READ_ONLY, false);

    std::string user = !info.user_data_path.empty() ? info.user_data_path : root_from_platform(NUE_ROOT_USER, info);
    if (user.empty()) user = generic_user_dir(info);
    std::string cache = root_from_platform(NUE_ROOT_CACHE, info);
    if (cache.empty()) cache = native_join(user, "cache");
    std::string settings = root_from_platform(NUE_ROOT_SETTINGS, info);
    if (settings.empty()) settings = native_join(user, "settings");
    std::string temp = root_from_platform(NUE_ROOT_TEMP, info);
    // Scratch space is cleared at start, so two processes must never share one: portable mode keeps it below the explicit user folder, otherwise
    // it is a per-process folder in the system temp directory, removed again at shutdown.
    if (temp.empty()) {
        if (!info.user_data_path.empty()) temp = native_join(user, "temp");
        else { temp = native_join(os_temp_dir(), "nue-" + info.org_name + "-" + info.app_id + "-" + std::to_string(os_process_id())); V().owned_temp = temp; }
    }

    add_mount("user", user, NUE_MOUNT_CREATE_DIRECTORY, false);
    add_mount("cache", cache, NUE_MOUNT_CREATE_DIRECTORY, false);
    add_mount("settings", settings, NUE_MOUNT_CREATE_DIRECTORY, false);
    os_remove_dir_tree(temp);   // scratch space starts empty (this folder is private to the app)
    add_mount("temp", temp, NUE_MOUNT_CREATE_DIRECTORY, false);
    for (const std::string& d : {user, cache, settings, temp}) os_make_dirs(d);
}

void vfs_shutdown() {
    VfsState& v = V();
    size_t open_files = 0;
    {
        std::lock_guard<std::mutex> lock(v.files_mutex);
        for (auto& f : v.files) if (f) { ++open_files; f.reset(); }
        v.files.clear(); v.generations.clear(); v.free_slots.clear();
    }
    if (open_files) NUE_LOGW("storage", "%zu file handle(s) were still open at shutdown and were closed", open_files);
    std::string owned;
    {
        std::lock_guard<std::mutex> lock(v.mutex);
        v.roots.clear();
        v.dir_cache.clear();
        owned.swap(v.owned_temp);
    }
    if (!owned.empty()) os_remove_dir_tree(owned);
}

}  // namespace nuei

using namespace nuei;

extern "C" {

NUE_API nue_result NUE_CALL nue_vfs_mount(const nue_mount_desc* desc) {
    if (!desc || !desc->root || !desc->native_path) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_vfs_mount: root and native_path are required");
    if (!valid_root_name(desc->root)) return fail(NUE_ERR_INVALID_ARGUMENT, "invalid root name '%s' (letters, digits, '_' and '-' only)", desc->root);
    if (desc->flags & NUE_MOUNT_CREATE_DIRECTORY) { if (!os_make_dirs(desc->native_path)) return fail(NUE_ERR_IO, "cannot create '%s'", desc->native_path); }
    add_mount(desc->root, desc->native_path, desc->flags, !(desc->flags & NUE_MOUNT_APPEND));
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vfs_unmount(const char* root, const char* native_path) {
    if (!root) return NUE_ERR_INVALID_ARGUMENT;
    VfsState& v = V();
    std::lock_guard<std::mutex> lock(v.mutex);
    auto it = v.roots.find(lower_ascii(root));
    if (it == v.roots.end()) return NUE_ERR_NOT_FOUND;
    if (!native_path) { v.roots.erase(it); return NUE_OK; }
    std::string clean = native_path;
    std::replace(clean.begin(), clean.end(), '\\', '/');
    auto& list = it->second;
    size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Mount& m) { return m.native == clean; }), list.end());
    if (list.empty()) v.roots.erase(it);
    return list.size() < before || v.roots.find(lower_ascii(root)) == v.roots.end() ? NUE_OK : NUE_ERR_NOT_FOUND;
}

NUE_API nue_bool NUE_CALL nue_vfs_has_root(const char* root) {
    if (!root) return NUE_FALSE;
    VfsState& v = V();
    std::lock_guard<std::mutex> lock(v.mutex);
    return v.roots.count(lower_ascii(root)) ? NUE_TRUE : NUE_FALSE;
}

NUE_API nue_result NUE_CALL nue_vfs_get_root_path(const char* root, char* buffer, size_t capacity, size_t* out_size) {
    if (!root) return NUE_ERR_INVALID_ARGUMENT;
    std::string p;
    {
        VfsState& v = V();
        std::lock_guard<std::mutex> lock(v.mutex);
        auto it = v.roots.find(lower_ascii(root));
        if (it == v.roots.end() || it->second.empty()) return fail(NUE_ERR_NOT_FOUND, "unknown storage root '%s'", root);
        p = it->second.front().native;
    }
    return copy_out_string(p, buffer, capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_path_normalize(const char* path, char* buffer, size_t capacity, size_t* out_size) {
    if (!path) return NUE_ERR_INVALID_ARGUMENT;
    std::string root, rel, out;
    std::string p = path;
    if (split_root(p, &root, &rel)) {
        std::string norm;
        if (!normalize_relative(rel, &norm)) return fail(NUE_ERR_ACCESS_DENIED, "path '%s' climbs above its root", path);
        out = root + ":" + norm;
    } else {
        // native path: separators and dot segments only; keep a leading drive/UNC/root prefix intact
        std::string s = p;
        std::replace(s.begin(), s.end(), '\\', '/');
        std::string prefix;
        if (s.size() >= 2 && s[1] == ':') { prefix = s.substr(0, 2); s = s.substr(2); }
        bool absolute = !s.empty() && s[0] == '/';
        std::string norm;
        std::vector<std::string> stack;
        for (const std::string& seg : split_segments(s)) {
            if (seg == ".") continue;
            if (seg == "..") { if (!stack.empty() && stack.back() != "..") stack.pop_back(); else if (!absolute) stack.push_back(".."); continue; }
            stack.push_back(seg);
        }
        for (size_t i = 0; i < stack.size(); ++i) { if (i) norm += '/'; norm += stack[i]; }
        out = prefix + (absolute ? "/" : "") + norm;
    }
    return copy_out_string(out, buffer, capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_vfs_resolve(const char* path, nue_bool for_write, char* buffer, size_t capacity, size_t* out_size) {
    if (!path) return NUE_ERR_INVALID_ARGUMENT;
    std::string out;
    nue_result r = resolve_path(path, for_write != 0, &out);
    if (NUE_FAILED(r)) return r;
    return copy_out_string(out, buffer, capacity, out_size);
}

NUE_API void NUE_CALL nue_vfs_clear_cache(void) { VfsState& v = V(); std::lock_guard<std::mutex> lock(v.mutex); v.dir_cache.clear(); }

// ---- files ----------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_file_open(const char* path, nue_open_flags flags, nue_file* out) {
    if (!path || !out) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_file_open: path and out are required");
    *out = 0;
    if (!(flags & (NUE_OPEN_READ | NUE_OPEN_WRITE | NUE_OPEN_APPEND))) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_file_open: no access flags");
    const bool writing = (flags & (NUE_OPEN_WRITE | NUE_OPEN_APPEND)) != 0;
    std::string native;
    nue_result r = (flags & NUE_OPEN_RAW) ? (native = path, NUE_OK) : resolve_path(path, writing, &native);
    if (NUE_FAILED(r)) return r;
    if (writing && (flags & NUE_OPEN_CREATE)) os_make_parent_dirs(native);
    std::shared_ptr<FileObj> f = std::make_shared<FileObj>();
    uint32_t oflags = flags;
    if (flags & NUE_OPEN_APPEND) oflags |= NUE_OPEN_WRITE | NUE_OPEN_CREATE;
    r = os_file_open(native, oflags, &f->handle);
    if (NUE_FAILED(r)) return fail(r, "cannot open '%s' (%s)", path, native.c_str());
    f->flags = flags;
    f->native_path = native;
    if (writing) invalidate_parent_of(native);
    VfsState& v = V();
    std::lock_guard<std::mutex> lock(v.files_mutex);
    uint32_t slot;
    if (!v.free_slots.empty()) { slot = v.free_slots.back(); v.free_slots.pop_back(); }
    else {
        if (v.files.size() >= 0xFFFE) return fail(NUE_ERR_OUT_OF_MEMORY, "too many open files (65534)");
        slot = uint32_t(v.files.size());
        v.files.emplace_back();
        v.generations.push_back(0);
    }
    uint16_t gen = uint16_t(v.generations[slot] + 1);
    if (gen == 0) gen = 1;
    v.generations[slot] = gen;
    v.files[slot] = f;
    *out = (uint32_t(gen) << 16) | (slot + 1);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_close(nue_file id) {
    VfsState& v = V();
    std::shared_ptr<FileObj> keep;   // closed outside the lock, when the last in-flight operation drops its reference
    {
        std::lock_guard<std::mutex> lock(v.files_mutex);
        uint32_t idx = id & 0xFFFFu;
        if (idx == 0 || idx > v.files.size() || v.generations[idx - 1] != (id >> 16) || !v.files[idx - 1]) return NUE_ERR_INVALID_HANDLE;
        keep = std::move(v.files[idx - 1]);
        v.files[idx - 1].reset();
        v.generations[idx - 1] = uint16_t(v.generations[idx - 1] + 1 == 0 ? 1 : v.generations[idx - 1] + 1);   // invalidate stale ids at once
        v.free_slots.push_back(idx - 1);
    }
    return NUE_OK;
}

#define GET_FILE(var, id) \
    std::shared_ptr<FileObj> var = file_from_id(id); \
    if (!var) return fail(NUE_ERR_INVALID_HANDLE, "invalid file handle %u", (unsigned)(id))

NUE_API nue_result NUE_CALL nue_file_read_at(nue_file id, uint64_t offset, void* buffer, size_t size, size_t* out_read) {
    GET_FILE(f, id);
    if (!(f->flags & NUE_OPEN_READ)) return fail(NUE_ERR_ACCESS_DENIED, "file was not opened for reading");
    size_t got = 0;
    nue_result r = os_file_read_at(f->handle, offset, buffer, size, &got);
    if (out_read) *out_read = got;
    if (NUE_FAILED(r)) return r;
    if (got == 0 && size > 0) return NUE_ERR_END_OF_DATA;
    return got < size ? NUE_OK_PARTIAL : NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_read(nue_file id, void* buffer, size_t size, size_t* out_read) {
    GET_FILE(f, id);
    std::lock_guard<std::mutex> lock(f->pos_mutex);
    size_t got = 0;
    if (!(f->flags & NUE_OPEN_READ)) return fail(NUE_ERR_ACCESS_DENIED, "file was not opened for reading");
    nue_result r = os_file_read_at(f->handle, f->pos, buffer, size, &got);
    f->pos += got;
    if (out_read) *out_read = got;
    if (NUE_FAILED(r)) return r;
    if (got == 0 && size > 0) return NUE_ERR_END_OF_DATA;
    return got < size ? NUE_OK_PARTIAL : NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_write_at(nue_file id, uint64_t offset, const void* data, size_t size, size_t* out_written) {
    GET_FILE(f, id);
    if (!(f->flags & (NUE_OPEN_WRITE | NUE_OPEN_APPEND))) return fail(NUE_ERR_ACCESS_DENIED, "file was not opened for writing");
    size_t put = 0;
    nue_result r = os_file_write_at(f->handle, offset, data, size, &put);
    if (out_written) *out_written = put;
    return r;
}

NUE_API nue_result NUE_CALL nue_file_write(nue_file id, const void* data, size_t size, size_t* out_written) {
    GET_FILE(f, id);
    if (!(f->flags & (NUE_OPEN_WRITE | NUE_OPEN_APPEND))) return fail(NUE_ERR_ACCESS_DENIED, "file was not opened for writing");
    std::lock_guard<std::mutex> lock(f->pos_mutex);
    if (f->flags & NUE_OPEN_APPEND) { uint64_t sz = 0; nue_result sr = os_file_size(f->handle, &sz); if (NUE_FAILED(sr)) return sr; f->pos = sz; }
    size_t put = 0;
    nue_result r = os_file_write_at(f->handle, f->pos, data, size, &put);
    f->pos += put;
    if (out_written) *out_written = put;
    return r;
}

NUE_API nue_result NUE_CALL nue_file_seek(nue_file id, int64_t offset, nue_seek_origin origin, uint64_t* out_pos) {
    GET_FILE(f, id);
    std::lock_guard<std::mutex> lock(f->pos_mutex);
    int64_t base = 0;
    if (origin == NUE_SEEK_CUR) base = int64_t(f->pos);
    else if (origin == NUE_SEEK_END) { uint64_t sz = 0; nue_result r = os_file_size(f->handle, &sz); if (NUE_FAILED(r)) return r; base = int64_t(sz); }
    else if (origin != NUE_SEEK_SET) return NUE_ERR_INVALID_ARGUMENT;
    int64_t target = base + offset;
    if (target < 0) return fail(NUE_ERR_INVALID_ARGUMENT, "seek before the start of the file");
    f->pos = uint64_t(target);
    if (out_pos) *out_pos = f->pos;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_tell(nue_file id, uint64_t* out_pos) {
    GET_FILE(f, id);
    std::lock_guard<std::mutex> lock(f->pos_mutex);
    if (out_pos) *out_pos = f->pos;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_size(nue_file id, uint64_t* out_size) {
    GET_FILE(f, id);
    uint64_t sz = 0;
    nue_result r = os_file_size(f->handle, &sz);
    if (out_size) *out_size = sz;
    return r;
}

NUE_API nue_result NUE_CALL nue_file_set_size(nue_file id, uint64_t size) {
    GET_FILE(f, id);
    if (!(f->flags & (NUE_OPEN_WRITE | NUE_OPEN_APPEND))) return fail(NUE_ERR_ACCESS_DENIED, "file was not opened for writing");
    return os_file_set_size(f->handle, size);
}

NUE_API nue_result NUE_CALL nue_file_flush(nue_file id) { GET_FILE(f, id); (void)f; return NUE_OK; }   // no user space buffering
NUE_API nue_result NUE_CALL nue_file_sync(nue_file id) { GET_FILE(f, id); return os_file_sync(f->handle); }

NUE_API nue_result NUE_CALL nue_file_get_native_path(nue_file id, char* buffer, size_t capacity, size_t* out_size) {
    GET_FILE(f, id);
    return copy_out_string(f->native_path, buffer, capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_file_load(const char* path, void** out_data, size_t* out_size) {
    if (!path || !out_data) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_file_load: path and out_data are required");
    *out_data = nullptr;
    if (out_size) *out_size = 0;
    nue_file f = 0;
    nue_result r = nue_file_open(path, NUE_OPEN_READ | NUE_OPEN_SEQUENTIAL, &f);
    if (NUE_FAILED(r)) return r;
    uint64_t sz = 0;
    r = nue_file_size(f, &sz);
    if (NUE_FAILED(r) || sz > (uint64_t)SIZE_MAX - 1 || (sizeof(size_t) == 4 && sz > 0x7FFFFFF0u)) { nue_file_close(f); return NUE_FAILED(r) ? r : fail(NUE_ERR_OUT_OF_MEMORY, "'%s' is too large to load", path); }
    uint8_t* mem = static_cast<uint8_t*>(nue_alloc(size_t(sz) + 1, 16));
    if (!mem) { nue_file_close(f); return NUE_ERR_OUT_OF_MEMORY; }
    size_t got = 0;
    r = sz ? nue_file_read_at(f, 0, mem, size_t(sz), &got) : NUE_OK;
    nue_file_close(f);
    if (NUE_FAILED(r) || got != sz) { nue_free(mem); return NUE_FAILED(r) ? r : fail(NUE_ERR_IO, "'%s' changed while it was being read", path); }
    mem[sz] = 0;
    *out_data = mem;
    if (out_size) *out_size = size_t(sz);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_save(const char* path, const void* data, size_t size) {
    if (!path || (!data && size)) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_file_save: bad arguments");
    std::string native;
    nue_result r = resolve_path(path, true, &native);
    if (NUE_FAILED(r)) return r;
    os_make_parent_dirs(native);
    std::string tmp = native + ".nuetmp";
    OsHandle h = kBadOsHandle;
    r = os_file_open(tmp, NUE_OPEN_WRITE | NUE_OPEN_CREATE | NUE_OPEN_TRUNCATE, &h);
    if (NUE_FAILED(r)) return fail(r, "cannot create '%s'", tmp.c_str());
    size_t put = 0;
    r = size ? os_file_write_at(h, 0, data, size, &put) : NUE_OK;
    if (NUE_SUCCEEDED(r)) r = os_file_sync(h);
    os_file_close(h);
    if (NUE_FAILED(r) || put != size) { os_remove_file(tmp); return NUE_FAILED(r) ? fail(r, "writing '%s' failed", path) : NUE_ERR_IO; }
    if (!os_rename_file(tmp, native)) { os_remove_file(tmp); return fail(NUE_ERR_IO, "cannot replace '%s'", native.c_str()); }
    invalidate_parent_of(native);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_map(nue_file id, uint64_t offset, size_t size, nue_file_map_info* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    GET_FILE(f, id);
    uint64_t total = 0;
    nue_result r = os_file_size(f->handle, &total);
    if (NUE_FAILED(r)) return r;
    if (offset > total) return fail(NUE_ERR_INVALID_ARGUMENT, "map offset beyond the end of the file");
    uint64_t len = size ? size : total - offset;
    if (offset + len > total) return fail(NUE_ERR_INVALID_ARGUMENT, "map range beyond the end of the file");
    void* mem = nue_alloc(size_t(len) + 1, 64);
    if (!mem) return NUE_ERR_OUT_OF_MEMORY;
    size_t got = 0;
    r = len ? os_file_read_at(f->handle, offset, mem, size_t(len), &got) : NUE_OK;
    if (NUE_FAILED(r) || got != len) { nue_free(mem); return NUE_FAILED(r) ? r : NUE_ERR_IO; }
    uint32_t keep = out->struct_size ? out->struct_size : uint32_t(sizeof(*out));
    out->struct_size = keep;
    out->data = mem;
    out->size = size_t(len);
    out->emulated = NUE_TRUE;   // real mmap is not implemented yet: a heap copy (documented in STATUS.md)
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_file_unmap(nue_file, const nue_file_map_info* map) {
    if (!map || !map->data) return NUE_ERR_INVALID_ARGUMENT;
    nue_free(const_cast<void*>(map->data));
    return NUE_OK;
}

// ---- paths and directories -------------------------------------------------------------------------------------------------------

NUE_API nue_bool NUE_CALL nue_vfs_exists(const char* path) {
    nue_stat st;
    NUE_STRUCT_INIT(&st);
    return NUE_SUCCEEDED(nue_vfs_stat(path, &st)) ? NUE_TRUE : NUE_FALSE;
}

NUE_API nue_result NUE_CALL nue_vfs_stat(const char* path, nue_stat* out) {
    if (!path || !out) return NUE_ERR_INVALID_ARGUMENT;
    std::string native;
    nue_result r = resolve_path(path, false, &native);
    if (NUE_FAILED(r)) return r;
    nue_stat st;
    if (!os_stat_path(native, &st)) return fail(NUE_ERR_NOT_FOUND, "'%s' does not exist", path);
    uint32_t keep = out->struct_size ? out->struct_size : uint32_t(sizeof(st));
    std::memcpy(out, &st, keep < sizeof(st) ? keep : sizeof(st));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vfs_create_directory(const char* path) {
    if (!path) return NUE_ERR_INVALID_ARGUMENT;
    std::string native;
    nue_result r = resolve_path(path, true, &native);
    if (NUE_FAILED(r)) return r;
    if (!os_make_dirs(native)) return fail(NUE_ERR_IO, "cannot create directory '%s'", native.c_str());
    invalidate_parent_of(native);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_vfs_remove(const char* path) {
    if (!path) return NUE_ERR_INVALID_ARGUMENT;
    std::string native;
    nue_result r = resolve_path(path, true, &native);
    if (NUE_FAILED(r)) return r;
    nue_stat st;
    if (!os_stat_path(native, &st)) return fail(NUE_ERR_NOT_FOUND, "'%s' does not exist", path);
    bool ok = st.kind == NUE_ENTRY_DIRECTORY ? os_remove_dir(native) : os_remove_file(native);
    invalidate_parent_of(native);
    return ok ? NUE_OK : fail(NUE_ERR_IO, "cannot remove '%s' (is a directory empty?)", path);
}

NUE_API nue_result NUE_CALL nue_vfs_remove_tree(const char* path) {
    if (!path) return NUE_ERR_INVALID_ARGUMENT;
    std::string root, rel;
    if (!split_root(path, &root, &rel)) return fail(NUE_ERR_ACCESS_DENIED, "nue_vfs_remove_tree only works inside the user, cache, temp and settings roots");
    bool allowed = false;
    for (const char* w : kWritableRoots) allowed |= root == w;
    if (!allowed) return fail(NUE_ERR_ACCESS_DENIED, "nue_vfs_remove_tree is not allowed in root '%s'", root.c_str());
    std::string norm;
    if (!normalize_relative(rel, &norm) || norm.empty()) return fail(NUE_ERR_ACCESS_DENIED, "refusing to remove a whole root or a path outside it");
    std::string native;
    nue_result r = resolve_path(path, true, &native);
    if (NUE_FAILED(r)) return r;
    bool ok = os_remove_dir_tree(native);
    nue_vfs_clear_cache();
    return ok ? NUE_OK : fail(NUE_ERR_IO, "cannot remove '%s'", native.c_str());
}

NUE_API nue_result NUE_CALL nue_vfs_rename(const char* from, const char* to) {
    if (!from || !to) return NUE_ERR_INVALID_ARGUMENT;
    std::string r1, rel1, r2, rel2;
    if (split_root(from, &r1, &rel1) != split_root(to, &r2, &rel2) || r1 != r2) return fail(NUE_ERR_INVALID_ARGUMENT, "nue_vfs_rename: both paths must be in the same root");
    std::string a, b;
    nue_result r = resolve_path(from, true, &a);
    if (NUE_FAILED(r)) return r;
    r = resolve_path(to, true, &b);
    if (NUE_FAILED(r)) return r;
    if (!native_exists(a)) return fail(NUE_ERR_NOT_FOUND, "'%s' does not exist in the writable mount", from);
    os_make_parent_dirs(b);
    if (!os_rename_file(a, b)) return fail(NUE_ERR_IO, "cannot rename '%s' to '%s'", from, to);
    invalidate_parent_of(a);
    invalidate_parent_of(b);
    return NUE_OK;
}

static nue_result list_impl(const char* path, const char* pattern, nue_dir_callback cb, void* ud);

NUE_API nue_result NUE_CALL nue_vfs_list(const char* path, nue_dir_callback cb, void* ud) { return list_impl(path, nullptr, cb, ud); }
NUE_API nue_result NUE_CALL nue_vfs_glob(const char* path, const char* pattern, nue_dir_callback cb, void* ud) {
    if (!pattern) return NUE_ERR_INVALID_ARGUMENT;
    return list_impl(path, pattern, cb, ud);
}

static bool wildcard_match(const std::string& pat, size_t pi, const std::string& s, size_t si) {
    while (pi < pat.size()) {
        if (pat[pi] == '*') {
            while (pi < pat.size() && pat[pi] == '*') ++pi;
            if (pi == pat.size()) return true;
            for (size_t k = si; k <= s.size(); ++k) if (wildcard_match(pat, pi, s, k)) return true;
            return false;
        }
        if (si >= s.size()) return false;
        if (pat[pi] != '?' && pat[pi] != s[si]) return false;
        ++pi; ++si;
    }
    return si == s.size();
}

static nue_result list_impl(const char* path, const char* pattern, nue_dir_callback cb, void* ud) {
    if (!path || !cb) return NUE_ERR_INVALID_ARGUMENT;
    std::string root, rel;
    std::vector<std::string> dirs;
    if (split_root(path, &root, &rel)) {
        std::string norm;
        if (!normalize_relative(rel, &norm)) return fail(NUE_ERR_ACCESS_DENIED, "path '%s' climbs above its root", path);
        std::vector<Mount> mounts;
        {
            VfsState& v = V();
            std::lock_guard<std::mutex> lock(v.mutex);
            auto it = v.roots.find(root);
            if (it == v.roots.end()) return fail(NUE_ERR_NOT_FOUND, "unknown storage root '%s'", root.c_str());
            mounts = it->second;
        }
        for (const Mount& m : mounts) {
            std::string native;
            if (resolve_in_mount(m, norm, true, &native) && native_is_dir(native)) dirs.push_back(native);
        }
        if (dirs.empty()) return fail(NUE_ERR_NOT_FOUND, "directory '%s' does not exist", path);
    } else {
        if (!native_is_dir(path)) return fail(NUE_ERR_NOT_FOUND, "directory '%s' does not exist", path);
        dirs.push_back(path);
    }
    std::map<std::string, OsDirEntry> merged;   // folded name -> entry; first mount wins
    for (const std::string& d : dirs) {
        std::vector<OsDirEntry> entries;
        os_list_dir(d, &entries);
        for (OsDirEntry& e : entries) merged.emplace(fold_case_utf8(e.name), std::move(e));
    }
    const std::string pat = pattern ? fold_case_utf8(pattern) : std::string();
    for (auto& kv : merged) {
        if (pattern && !wildcard_match(pat, 0, kv.first, 0)) continue;
        nue_dir_entry de;
        NUE_STRUCT_INIT(&de);
        de.name = kv.second.name.c_str();
        de.kind = kv.second.kind;
        de.size = kv.second.size;
        de.modified_unix_ms = kv.second.modified_unix_ms;
        if (!cb(ud, &de)) break;
    }
    return NUE_OK;
}

}  // extern "C"
