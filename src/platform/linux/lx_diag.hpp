// Pure (host-testable) diagnostics helpers: mount table, core-dump configuration advice and
// the sleep plan used by the frame limiter.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lx_fs.hpp"

namespace nue::lx {

// ---- mounts --------------------------------------------------------------------------
struct MountEntry {
    std::string mount_point;
    std::string fs_type;
    std::string source;
    bool read_only = false;
};
std::vector<MountEntry> parse_mountinfo(const std::string& text);
// Longest mount point that is a prefix of `path` (component-wise); nullptr if none.
const MountEntry* mount_for_path(const std::vector<MountEntry>& mounts, const std::string& path);
// "vfat", "exfat", "ntfs", "fuseblk", "nfs", "cifs", "9p" ...: file systems where case-insensitive
// matching, permissions or mmap behave differently, with a short reason; empty for ordinary ones.
std::string fs_type_warning(const std::string& fs_type);

// ---- core dumps ----------------------------------------------------------------------
struct CoreDumpInfo {
    std::string core_pattern;       // /proc/sys/kernel/core_pattern
    bool piped = false;             // starts with '|' (systemd-coredump, apport, abrt)
    bool systemd_coredump = false;
    bool apport = false;
    uint64_t soft_limit_bytes = 0;  // RLIMIT_CORE (UINT64_MAX = unlimited), passed by the caller
    std::vector<std::string> hints;
};
CoreDumpInfo describe_core_dumps(const IFileSystem& fs, uint64_t rlimit_core_soft);

// ---- frame limiter -------------------------------------------------------------------
struct SleepPlan {
    int64_t sleep_until_ns = 0;  // coarse sleep (clock_nanosleep absolute) up to here
    int64_t spin_until_ns = 0;   // then busy-wait until here (the deadline)
    bool needs_sleep = false;
    bool needs_spin = false;
};
// `margin_ns` is how early the coarse sleep must end to absorb scheduler wake-up latency.
SleepPlan plan_sleep(int64_t now_ns, int64_t deadline_ns, int64_t margin_ns);

}  // namespace nue::lx
