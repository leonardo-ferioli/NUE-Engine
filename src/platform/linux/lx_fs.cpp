#include "lx_fs.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "lx_text.hpp"

namespace nue::lx {

namespace fs = std::filesystem;

std::string IFileSystem::read_line(const std::string& path) const {
    std::string s;
    if (!read_file(path, s, 4096)) return std::string();
    size_t nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    return std::string(trim(s));
}

// ---------------------------------------------------------------------------- StdFs

std::string StdFs::map(const std::string& path) const {
    if (root_.empty()) return path;
    if (!path.empty() && path[0] == '/') return root_ + path;
    return root_ + "/" + path;
}

bool StdFs::read_file(const std::string& path, std::string& out, size_t max_bytes) const {
    std::ifstream f(fs::u8path(map(path)), std::ios::binary);
    if (!f) return false;
    out.clear();
    char buf[4096];
    while (f && out.size() < max_bytes) {
        f.read(buf, static_cast<std::streamsize>(std::min(sizeof buf, max_bytes - out.size())));
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    // A directory opens fine on some libcs and then fails to read; treat as unreadable.
    return !f.bad();
}

bool StdFs::list_dir(const std::string& path, std::vector<DirEntry>& out) const {
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(map(path)), fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    out.clear();
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        DirEntry e;
        e.name = it->path().filename().u8string();
        std::error_code ec2;
        e.is_dir = it->is_directory(ec2);  // follows symlinks
        out.push_back(std::move(e));
    }
    return true;
}

FileStat StdFs::stat(const std::string& path) const {
    FileStat st;
    std::error_code ec;
    fs::path p = fs::u8path(map(path));
    fs::file_status s = fs::status(p, ec);
    if (ec || !fs::exists(s)) return st;
    st.exists = true;
    st.is_dir = fs::is_directory(s);
    st.is_file = fs::is_regular_file(s);
    if (st.is_file) {
        st.size = fs::file_size(p, ec);
        if (ec) st.size = 0;
    }
    auto t = fs::last_write_time(p, ec);
    if (!ec) st.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    return st;
}

bool StdFs::read_link(const std::string& path, std::string& target) const {
    std::error_code ec;
    fs::path t = fs::read_symlink(fs::u8path(map(path)), ec);
    if (ec) return false;
    target = t.generic_u8string();
    return true;
}

int64_t StdFs::now_ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(fs::file_time_type::clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------- MemFs

std::string MemFs::norm(const std::string& p) {
    std::string out;
    for (std::string_view c : split(p, '/')) {
        if (c == ".") continue;
        out += '/';
        out += c;
    }
    return out.empty() ? "/" : out;
}

std::string MemFs::parent_of(const std::string& p) {
    size_t pos = p.rfind('/');
    if (pos == std::string::npos || pos == 0) return "/";
    return p.substr(0, pos);
}

void MemFs::ensure_dirs_locked(const std::string& dir) {
    if (dir == "/") {
        nodes_.try_emplace("/", Node{Node::Dir, {}, now_});
        return;
    }
    if (nodes_.count(dir)) return;
    ensure_dirs_locked(parent_of(dir));
    nodes_[dir] = Node{Node::Dir, {}, now_};
    touch_locked(parent_of(dir));
}

void MemFs::touch_locked(const std::string& dir) {
    auto it = nodes_.find(dir);
    if (it != nodes_.end()) it->second.mtime = ++now_;
}

void MemFs::add_file(const std::string& path, std::string content) {
    std::lock_guard<std::mutex> g(m_);
    std::string p = norm(path);
    ensure_dirs_locked(parent_of(p));
    bool is_new = !nodes_.count(p);
    nodes_[p] = Node{Node::File, std::move(content), now_};
    if (is_new) touch_locked(parent_of(p));
}

void MemFs::add_dir(const std::string& path) {
    std::lock_guard<std::mutex> g(m_);
    ensure_dirs_locked(norm(path));
}

void MemFs::add_symlink(const std::string& path, const std::string& target) {
    std::lock_guard<std::mutex> g(m_);
    std::string p = norm(path);
    ensure_dirs_locked(parent_of(p));
    bool is_new = !nodes_.count(p);
    nodes_[p] = Node{Node::Link, target, now_};
    if (is_new) touch_locked(parent_of(p));
}

void MemFs::remove(const std::string& path) {
    std::lock_guard<std::mutex> g(m_);
    std::string p = norm(path);
    auto it = nodes_.find(p);
    if (it == nodes_.end()) return;
    std::string prefix = p + "/";
    for (auto i = nodes_.begin(); i != nodes_.end();) {
        if (i->first == p || starts_with(i->first, prefix)) i = nodes_.erase(i);
        else ++i;
    }
    touch_locked(parent_of(p));
}

void MemFs::for_each_file(const std::function<void(const std::string&, const std::string&)>& fn) const {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& kv : nodes_)
        if (kv.second.kind == Node::File) fn(kv.first, kv.second.data);
}

bool MemFs::read_file(const std::string& path, std::string& out, size_t max_bytes) const {
    std::lock_guard<std::mutex> g(m_);
    auto it = nodes_.find(norm(path));
    // Follow one level of file symlink to an absolute target (enough for sysfs-style fixtures).
    if (it != nodes_.end() && it->second.kind == Node::Link) it = nodes_.find(norm(it->second.data));
    if (it == nodes_.end() || it->second.kind != Node::File) return false;
    out = it->second.data.substr(0, max_bytes);
    return true;
}

bool MemFs::list_dir(const std::string& path, std::vector<DirEntry>& out) const {
    std::lock_guard<std::mutex> g(m_);
    ++list_calls_;
    std::string p = norm(path);
    auto d = nodes_.find(p);
    if (d != nodes_.end() && d->second.kind == Node::Link) d = nodes_.find(norm(d->second.data));
    if (d == nodes_.end() || d->second.kind != Node::Dir) return false;
    std::string base = d->first == "/" ? "/" : d->first + "/";
    out.clear();
    for (auto it = nodes_.lower_bound(base); it != nodes_.end() && starts_with(it->first, base); ++it) {
        std::string_view rest = std::string_view(it->first).substr(base.size());
        if (rest.empty() || rest.find('/') != std::string_view::npos) continue;
        out.push_back(DirEntry{std::string(rest), it->second.kind == Node::Dir});
    }
    return true;
}

FileStat MemFs::stat(const std::string& path) const {
    std::lock_guard<std::mutex> g(m_);
    ++stat_calls_;
    FileStat st;
    auto it = nodes_.find(norm(path));
    if (it != nodes_.end() && it->second.kind == Node::Link) it = nodes_.find(norm(it->second.data));
    if (it == nodes_.end()) return st;
    st.exists = true;
    st.is_dir = it->second.kind == Node::Dir;
    st.is_file = it->second.kind == Node::File;
    st.size = st.is_file ? it->second.data.size() : 0;
    st.mtime_ns = it->second.mtime;
    return st;
}

bool MemFs::read_link(const std::string& path, std::string& target) const {
    std::lock_guard<std::mutex> g(m_);
    auto it = nodes_.find(norm(path));
    if (it == nodes_.end() || it->second.kind != Node::Link) return false;
    target = it->second.data;
    return true;
}

// ---------------------------------------------------------------------------- env

bool ProcessEnv::get(const std::string& name, std::string& value) const {
    const char* v = std::getenv(name.c_str());
    if (!v) return false;
    value = v;
    return true;
}

}  // namespace nue::lx
