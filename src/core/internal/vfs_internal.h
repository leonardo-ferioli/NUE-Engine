// Internal VFS declarations shared between vfs.cpp (paths, mounts, files) and vfs_io.cpp (async reads, manifests, import).
#pragma once

#include "internal/core_internal.h"
#include "internal/os_util.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace nuei {

struct FileObj {
    OsHandle handle = kBadOsHandle;
    nue_open_flags flags = 0;
    std::string native_path;
    std::mutex pos_mutex;          // serialises sequential read/write/seek
    uint64_t pos = 0;
    ~FileObj() { os_file_close(handle); }
};

std::shared_ptr<FileObj> file_from_id(nue_file id);

// "root:rel/path" -> root (lower case) and normalised relative path. Returns false for native paths (no root prefix).
bool split_root(const std::string& path, std::string* root, std::string* rel);
// Normalise separators/dots of the relative part; false if it climbs above the root.
bool normalize_relative(const std::string& rel, std::string* out);

// Resolve a VFS or native path. See nue_vfs_resolve.
nue_result resolve_path(const std::string& path, bool for_write, std::string* native_out);

// Join a native directory and a '/'-separated relative path.
std::string native_join(const std::string& base, const std::string& rel);

}  // namespace nuei
