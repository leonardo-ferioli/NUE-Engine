// File-system and environment abstractions used by every pure-logic module of the Linux
// backend. Reading /proc, /sys, XDG directories and game data through these interfaces lets
// the same code run against the real system (StdFs rooted at "/"), against a fixture
// directory captured from another machine (StdFs rooted anywhere) or against an in-memory
// tree (MemFs, used by the unit tests).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace nue::lx {

struct DirEntry {
    std::string name;
    bool is_dir = false;
};

struct FileStat {
    bool exists = false;
    bool is_dir = false;
    bool is_file = false;
    uint64_t size = 0;
    int64_t mtime_ns = 0;  // modification time, nanoseconds (arbitrary epoch for MemFs)
};

class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    // Reads at most max_bytes. Works for /proc and /sys files whose reported size is 0.
    virtual bool read_file(const std::string& path, std::string& out, size_t max_bytes = 1u << 22) const = 0;
    virtual bool list_dir(const std::string& path, std::vector<DirEntry>& out) const = 0;
    virtual FileStat stat(const std::string& path) const = 0;
    // Target of a symbolic link (not resolved further); false if not a link.
    virtual bool read_link(const std::string& path, std::string& target) const = 0;
    // Current time on the same clock as FileStat::mtime_ns.
    virtual int64_t now_ns() const = 0;

    bool exists(const std::string& path) const { return stat(path).exists; }
    bool is_dir(const std::string& path) const { return stat(path).is_dir; }
    // Trimmed first line of a file ("" when missing). Handy for sysfs attributes.
    std::string read_line(const std::string& path) const;
};

// Real file system (std::filesystem). With a non-empty root every absolute path is prefixed
// with it, so `StdFs("fixtures/steamdeck")` presents a captured machine tree as "/".
class StdFs final : public IFileSystem {
public:
    explicit StdFs(std::string root = std::string()) : root_(std::move(root)) {}
    bool read_file(const std::string& path, std::string& out, size_t max_bytes = 1u << 22) const override;
    bool list_dir(const std::string& path, std::vector<DirEntry>& out) const override;
    FileStat stat(const std::string& path) const override;
    bool read_link(const std::string& path, std::string& target) const override;
    int64_t now_ns() const override;
    const std::string& root() const { return root_; }

private:
    std::string map(const std::string& path) const;
    std::string root_;
};

// In-memory tree with a controllable clock, for tests. Thread-safe.
class MemFs final : public IFileSystem {
public:
    void set_time_ns(int64_t t) { std::lock_guard<std::mutex> g(m_); now_ = t; }
    int64_t time_ns() const { std::lock_guard<std::mutex> g(m_); return now_; }
    void advance_ns(int64_t d) { std::lock_guard<std::mutex> g(m_); now_ += d; }

    void add_file(const std::string& path, std::string content);
    void add_dir(const std::string& path);
    void add_symlink(const std::string& path, const std::string& target);
    void remove(const std::string& path);  // file, link, or directory subtree

    // Visits every regular file (symbolic links are skipped), e.g. to write a fixture tree to disk.
    void for_each_file(const std::function<void(const std::string& path, const std::string& data)>& fn) const;

    // Number of list_dir calls since construction (to verify caching).
    int list_calls() const { std::lock_guard<std::mutex> g(m_); return list_calls_; }
    int stat_calls() const { std::lock_guard<std::mutex> g(m_); return stat_calls_; }

    bool read_file(const std::string& path, std::string& out, size_t max_bytes = 1u << 22) const override;
    bool list_dir(const std::string& path, std::vector<DirEntry>& out) const override;
    FileStat stat(const std::string& path) const override;
    bool read_link(const std::string& path, std::string& target) const override;
    int64_t now_ns() const override { return time_ns(); }

private:
    struct Node {
        enum Kind { File, Dir, Link } kind = File;
        std::string data;  // content, or link target
        int64_t mtime = 0;
    };
    static std::string norm(const std::string& p);
    static std::string parent_of(const std::string& p);
    void ensure_dirs_locked(const std::string& dir);
    void touch_locked(const std::string& dir);

    mutable std::mutex m_;
    std::map<std::string, Node> nodes_;
    int64_t now_ = 1000000000;
    mutable int list_calls_ = 0;
    mutable int stat_calls_ = 0;
};

// Environment view.
class IEnv {
public:
    virtual ~IEnv() = default;
    // Returns false when the variable is unset. Empty values are returned as set.
    virtual bool get(const std::string& name, std::string& value) const = 0;
    std::string get_or(const std::string& name, const std::string& fallback = std::string()) const {
        std::string v;
        return get(name, v) ? v : fallback;
    }
    bool is_set(const std::string& name) const { std::string v; return get(name, v); }
    // True for set and non-empty, non-"0".
    bool truthy(const std::string& name) const {
        std::string v;
        return get(name, v) && !v.empty() && v != "0";
    }
};

class ProcessEnv final : public IEnv {
public:
    bool get(const std::string& name, std::string& value) const override;
};

class MapEnv final : public IEnv {
public:
    MapEnv() = default;
    MapEnv(std::initializer_list<std::pair<const std::string, std::string>> init) : vars(init) {}
    bool get(const std::string& name, std::string& value) const override {
        auto it = vars.find(name);
        if (it == vars.end()) return false;
        value = it->second;
        return true;
    }
    std::map<std::string, std::string> vars;
};

}  // namespace nue::lx
