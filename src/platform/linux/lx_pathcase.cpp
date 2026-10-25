#include "lx_pathcase.hpp"

#include <algorithm>

#include "lx_text.hpp"

namespace nue::lx {

PathResolver::PathResolver(const IFileSystem& fs, PathResolveOptions opt) : fs_(fs), opt_(std::move(opt)) {
    while (opt_.base_dir.size() > 1 && opt_.base_dir.back() == '/') opt_.base_dir.pop_back();
}

PathResolver::Normalized PathResolver::normalize(std::string_view in) {
    Normalized n;
    std::string s(in);
    for (char& c : s)
        if (c == '\\') c = '/';
    std::string_view v = s;
    // "\\?\C:\x" and "\\.\C:\x" device prefixes; "\\?\UNC\..." is not supported (no network paths).
    if (starts_with(v, "//?/") || starts_with(v, "//./")) {
        v.remove_prefix(4);
    } else if (!v.empty() && in[0] == '/') {
        n.host_absolute = true;
    } else if (!v.empty() && v[0] == '/') {
        n.rooted = true;  // original started with a backslash
    }
    if (!n.host_absolute && !n.rooted) {
        if (v.size() >= 2 && v[1] == ':' && ((v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z'))) {
            n.drive = static_cast<char>(v[0] >= 'a' ? v[0] - 32 : v[0]);
            v.remove_prefix(2);
            if (!v.empty() && v[0] == '/') n.rooted = true;
        }
    }
    n.trailing_slash = !v.empty() && v.back() == '/';
    for (std::string_view c : split(v, '/')) {
        if (c == ".") continue;
        if (c == "..") {
            if (!n.parts.empty()) n.parts.pop_back();
            continue;
        }
        n.parts.emplace_back(c);
    }
    return n;
}

const PathResolver::DirIndex* PathResolver::index_locked(const std::string& dir, bool force) const {
    auto it = cache_.find(dir);
    if (it != cache_.end() && !force) {
        ++st_.cache_hits;
        return &it->second;
    }
    DirIndex idx;
    build_locked(dir, idx);
    if (idx.scanned_ns == 0) {  // listing failed
        cache_.erase(dir);
        return nullptr;
    }
    auto& slot = cache_[dir];
    slot = std::move(idx);
    return &slot;
}

void PathResolver::build_locked(const std::string& dir, DirIndex& idx) const {
    FileStat s = fs_.stat(dir);  // before the listing: a concurrent change then shows as a newer mtime later
    if (!s.exists || !s.is_dir) return;
    std::vector<DirEntry> entries;
    if (!fs_.list_dir(dir, entries)) return;
    ++st_.dir_scans;
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    idx.mtime_ns = s.mtime_ns;
    idx.scanned_ns = fs_.now_ns();
    if (idx.scanned_ns == 0) idx.scanned_ns = 1;
    idx.entries = std::move(entries);
    for (size_t i = 0; i < idx.entries.size(); ++i) {
        idx.exact.emplace(idx.entries[i].name, i);
        idx.folded[fold_name(idx.entries[i].name)].push_back(i);
    }
}

PathResolver::Lookup PathResolver::find_in_dir_locked(const std::string& dir, const std::string& name, DirEntry& found,
                                                      bool& ambiguous) const {
    ambiguous = false;
    const std::string folded_name = fold_name(name);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DirIndex* idx = index_locked(dir, attempt > 0);
        if (!idx) return Lookup::NotADir;
        auto e = idx->exact.find(name);
        if (e != idx->exact.end()) {
            found = idx->entries[e->second];
            return Lookup::Exact;
        }
        auto f = idx->folded.find(folded_name);
        if (f != idx->folded.end()) {
            // entries are sorted by name, so the first index is the byte-wise smallest variant
            found = idx->entries[f->second.front()];
            ambiguous = f->second.size() > 1;
            ++st_.fold_hits;
            return ambiguous ? Lookup::Ambiguous : Lookup::Folded;
        }
        if (attempt == 1) break;
        // Miss on a cached index: rescan only if the directory may have changed.
        FileStat s = fs_.stat(dir);
        if (!s.exists) {
            cache_.erase(dir);
            return Lookup::NotADir;
        }
        bool changed = s.mtime_ns != idx->mtime_ns;
        bool untrusted = idx->scanned_ns - idx->mtime_ns < opt_.trust_age_ns;
        if (!changed && !untrusted) break;
        ++st_.rescans_on_miss;
    }
    ++st_.misses;
    return Lookup::Missing;
}

ResolveResult PathResolver::resolve(std::string_view path) const {
    Normalized n = normalize(path);
    ResolveResult r;
    std::string cur;
    if (n.host_absolute) {
        cur = "/";
    } else if (n.drive && opt_.drive_roots.count(n.drive)) {
        cur = opt_.drive_roots.at(n.drive);
    } else if (n.rooted && opt_.drive_roots.count('C') && !n.drive) {
        cur = opt_.drive_roots.at('C');
    } else {
        cur = opt_.base_dir.empty() ? "/" : opt_.base_dir;
    }
    while (cur.size() > 1 && cur.back() == '/') cur.pop_back();

    std::lock_guard<std::mutex> g(m_);
    ++st_.lookups;
    FileStat start = fs_.stat(cur);
    if (!start.exists || !start.is_dir) {
        r.path = cur;
        for (const auto& p : n.parts) r.path += (r.path == "/" ? "" : "/") + p;
        return r;
    }
    r.parent_found = true;
    r.found = true;
    for (size_t i = 0; i < n.parts.size(); ++i) {
        const bool last = i + 1 == n.parts.size();
        DirEntry e;
        bool amb = false;
        Lookup k = find_in_dir_locked(cur, n.parts[i], e, amb);
        if (k == Lookup::Missing || k == Lookup::NotADir) {
            r.found = false;
            r.parent_found = last && k == Lookup::Missing;
            // keep the caller's spelling for the rest
            for (size_t j = i; j < n.parts.size(); ++j) cur += (cur == "/" ? "" : "/") + n.parts[j];
            break;
        }
        if (k == Lookup::Folded || k == Lookup::Ambiguous) r.case_changed = true;
        if (amb) r.ambiguous = true;
        if (!last && !e.is_dir) {
            // A file where a directory was expected.
            r.found = false;
            r.parent_found = false;
            cur += (cur == "/" ? "" : "/") + e.name;
            for (size_t j = i + 1; j < n.parts.size(); ++j) cur += "/" + n.parts[j];
            break;
        }
        if (last && n.trailing_slash && !e.is_dir) {
            r.found = false;
            r.parent_found = false;
        }
        cur += (cur == "/" ? "" : "/") + e.name;
    }
    r.path = cur;
    return r;
}

bool PathResolver::list_matching(std::string_view dir, std::string_view pattern, std::vector<DirEntry>& out) const {
    ResolveResult d = resolve(dir);
    if (!d.found) return false;
    std::lock_guard<std::mutex> g(m_);
    const DirIndex* idx = index_locked(d.path, false);
    if (!idx) return false;
    out.clear();
    for (const DirEntry& e : idx->entries)
        if (wildcard_match(pattern, e.name)) out.push_back(e);
    return true;
}

void PathResolver::invalidate_dir(std::string_view dir) {
    std::lock_guard<std::mutex> g(m_);
    cache_.erase(std::string(dir));
}

void PathResolver::invalidate_all() {
    std::lock_guard<std::mutex> g(m_);
    cache_.clear();
}

void PathResolver::note_created(std::string_view posix_path, bool) {
    size_t pos = posix_path.rfind('/');
    invalidate_dir(pos == std::string_view::npos ? std::string_view(".") : (pos == 0 ? posix_path.substr(0, 1) : posix_path.substr(0, pos)));
}

void PathResolver::note_removed(std::string_view posix_path) { note_created(posix_path, false); }

PathResolverStats PathResolver::stats() const {
    std::lock_guard<std::mutex> g(m_);
    return st_;
}

}  // namespace nue::lx
