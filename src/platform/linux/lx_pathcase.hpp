// Case-insensitive resolution of Windows-style paths on a case-sensitive file system.
//
// The recompiled game uses paths such as "C:\Games\NFSU2\GLOBAL\GlobalA.BUN" whose spelling
// rarely matches the files on disk. PathResolver converts them to the real POSIX path:
//   1. normalise separators ('\' -> '/'), drop "\\?\" and drive prefixes, fold "." and "..";
//   2. walk the components; an exact name match wins, otherwise the unique case-folded match;
//   3. directory listings are cached (one scan per directory) and revalidated only on a miss
//      using the directory's mtime, so a hit costs no system call.
// Names that differ only by case on disk are reported as ambiguous and resolved
// deterministically (the byte-wise smallest name), never at random.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "lx_fs.hpp"

namespace nue::lx {

struct PathResolveOptions {
    // Directory used for relative paths (absolute POSIX, case-exact).
    std::string base_dir;
    // Where "X:\..." paths go. Missing letter: the drive is stripped and the rest is taken
    // relative to base_dir (the usual case for a game that stores its own absolute path).
    std::map<char, std::string> drive_roots;
    // A directory whose mtime is younger than this when scanned is rescanned on any miss
    // (coarse-granularity file systems cannot prove "unchanged").
    int64_t trust_age_ns = 2'000'000'000;
};

struct ResolveResult {
    std::string path;          // best POSIX path (the input spelling for components that do not exist)
    bool found = false;        // the full path exists
    bool parent_found = false; // every directory before the last component exists (file can be created)
    bool case_changed = false; // at least one component matched only after case folding
    bool ambiguous = false;    // a component had several case-variants on disk
};

struct PathResolverStats {
    uint64_t lookups = 0, dir_scans = 0, cache_hits = 0, fold_hits = 0, misses = 0, rescans_on_miss = 0;
};

class PathResolver {
public:
    PathResolver(const IFileSystem& fs, PathResolveOptions opt);

    // Pure text step, no file system access. ".." above the top is dropped (a game path cannot
    // climb out of the directory it is mapped to).
    struct Normalized {
        std::vector<std::string> parts;
        bool host_absolute = false;  // started with '/': a host path, used as is
        bool rooted = false;         // started with '': root of the (implicit) drive
        char drive = 0;              // upper-case drive letter or 0
        bool trailing_slash = false;
    };
    static Normalized normalize(std::string_view windows_or_posix_path);

    ResolveResult resolve(std::string_view path) const;

    // All entries of a directory matching a Windows wildcard (FindFirstFile semantics),
    // with the real on-disk names. `dir` is resolved like any other path.
    bool list_matching(std::string_view dir, std::string_view pattern, std::vector<DirEntry>& out) const;

    // Cache maintenance. note_created/removed keep the index exact for our own mutations.
    void invalidate_dir(std::string_view dir_posix);
    void invalidate_all();
    void note_created(std::string_view posix_path, bool is_dir);
    void note_removed(std::string_view posix_path);

    PathResolverStats stats() const;

private:
    struct DirIndex {
        int64_t mtime_ns = 0;
        int64_t scanned_ns = 0;
        std::vector<DirEntry> entries;
        std::unordered_map<std::string, size_t> exact;                // name -> entries index
        std::unordered_map<std::string, std::vector<size_t>> folded;  // fold(name) -> entries indices
    };
    enum class Lookup { Exact, Folded, Ambiguous, Missing, NotADir };
    const DirIndex* index_locked(const std::string& dir, bool force) const;
    void build_locked(const std::string& dir, DirIndex& idx) const;
    Lookup find_in_dir_locked(const std::string& dir, const std::string& name, DirEntry& found, bool& ambiguous) const;

    const IFileSystem& fs_;
    PathResolveOptions opt_;
    mutable std::mutex m_;
    mutable std::unordered_map<std::string, DirIndex> cache_;
    mutable PathResolverStats st_;
};

}  // namespace nue::lx
