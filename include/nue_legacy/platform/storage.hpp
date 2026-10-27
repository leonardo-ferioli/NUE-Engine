// Platform API: storage, a virtual file system over named roots. Shaped like NUE's nue_vfs_* / nue_file_* (include/nue/storage.h).
//
// A path is "root:relative/path" ("data:GLOBAL/GLOBALB.LUT", "user:saves/profile1.dat"); '\' is normalised to '/', repeated separators and "."
// segments are removed and ".." cannot climb above the root. Root names are case-insensitive. Lookup is case-insensitive per segment (the
// first listing-order match wins); new names keep the case the caller gave. Roots: data (read-only game data), user (saves, logs), cache,
// settings, temp; an application may add more and stack several directories under one name (the most recently mounted wins for reads, writes
// go to the first writable mount). The Win32 file layer (CreateFile, FindFirstFile, GetFullPathName...) translates the game's
// "C:\Games\...\file.ext" / relative names into these paths and adds the Win32 rules on top (dispositions, share modes, wildcards, attributes).
#pragma once
#include "nue_legacy/platform/types.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nue::legacy::platform {

using FileHandle = Id<struct FileTag>;

enum class EntryKind : uint8_t { none, file, directory, other };

struct StatInfo {
    EntryKind kind = EntryKind::none;
    uint64_t size = 0;
    int64_t modified_unix_ms = 0, created_unix_ms = 0, accessed_unix_ms = 0;
    bool read_only = false, hidden = false;
};

struct DirEntry {
    std::string name;                   // as stored on disk
    StatInfo info;
};

namespace open_flags {
enum : uint32_t { read = 1, write = 2, create = 4, truncate = 8, append = 16, exclusive = 32 };
}

enum class SeekOrigin : uint8_t { set, current, end };

struct MountDesc {
    std::string root;                   // "data", "user", ...
    std::string native_path;
    bool read_only = false;
    bool create_directory = false;
    bool append = false;                // lowest priority instead of highest
};

class Storage {
public:
    virtual ~Storage() = default;

    virtual Status mount(const MountDesc&) = 0;
    virtual bool has_root(std::string_view root) = 0;
    // Normalises without touching the disk; false for a path that climbs above its root.
    virtual bool normalize(std::string_view path, std::string& out) = 0;
    // The real native path of an existing entry (case-insensitive), or where a new one would be created when `for_write`.
    virtual Result<std::string> resolve(std::string_view path, bool for_write) = 0;

    virtual Result<FileHandle> open(std::string_view path, uint32_t flags) = 0;
    virtual Status close(FileHandle) = 0;
    // Reads up to `out.size()` bytes at the file position; `read` receives the count (0 at the end of the file).
    virtual Status read(FileHandle, MutableBytes out, size_t& read) = 0;
    virtual Status write(FileHandle, Bytes data, size_t& written) = 0;
    virtual Status read_at(FileHandle, uint64_t offset, MutableBytes out, size_t& read) = 0;
    virtual Status write_at(FileHandle, uint64_t offset, Bytes data, size_t& written) = 0;
    virtual Status seek(FileHandle, int64_t offset, SeekOrigin, uint64_t& position) = 0;
    virtual Status size(FileHandle, uint64_t& out) = 0;
    virtual Status set_size(FileHandle, uint64_t) = 0;
    virtual Status flush(FileHandle) = 0;

    virtual Status stat(std::string_view path, StatInfo& out) = 0;
    virtual Status create_directory(std::string_view path) = 0;             // parents too; ok when it exists
    virtual Status remove(std::string_view path) = 0;                       // a file or an empty directory
    virtual Status rename(std::string_view from, std::string_view to) = 0;  // same root
    virtual Status list(std::string_view path, std::vector<DirEntry>& out) = 0;   // merged over the mounts of a root, no "." and ".."
};

}  // namespace nue::legacy::platform
