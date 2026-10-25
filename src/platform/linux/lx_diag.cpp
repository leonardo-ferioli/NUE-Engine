#include "lx_diag.hpp"

#include "lx_signal_safe.hpp"
#include "lx_text.hpp"

namespace nue::lx {

const char* signal_name(int sig) {
    switch (sig) {  // numbers are identical on Linux x86-64 and arm64
        case 1: return "SIGHUP";
        case 2: return "SIGINT";
        case 3: return "SIGQUIT";
        case 4: return "SIGILL";
        case 5: return "SIGTRAP";
        case 6: return "SIGABRT";
        case 7: return "SIGBUS";
        case 8: return "SIGFPE";
        case 9: return "SIGKILL";
        case 11: return "SIGSEGV";
        case 13: return "SIGPIPE";
        case 15: return "SIGTERM";
        case 31: return "SIGSYS";
        default: return "SIG?";
    }
}

// ---------------------------------------------------------------------------- mounts

namespace {
// mountinfo escapes space, tab, newline and backslash as \040 \011 \012 \134
std::string unescape_mount(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
            int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out += static_cast<char>(v);
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}
}  // namespace

std::vector<MountEntry> parse_mountinfo(const std::string& text) {
    std::vector<MountEntry> out;
    for (std::string_view line : split_lines(text)) {
        // 36 35 98:0 /root /mnt rw,noatime master:1 - ext3 /dev/root rw,errors=continue
        size_t dash = line.find(" - ");
        if (dash == std::string_view::npos) continue;
        auto pre = split(line.substr(0, dash), ' ');
        auto post = split(line.substr(dash + 3), ' ');
        if (pre.size() < 6 || post.size() < 2) continue;
        MountEntry m;
        m.mount_point = unescape_mount(pre[4]);
        m.read_only = split(pre[5], ',').size() > 0 && split(pre[5], ',')[0] == "ro";
        m.fs_type = std::string(post[0]);
        m.source = unescape_mount(post[1]);
        out.push_back(std::move(m));
    }
    return out;
}

const MountEntry* mount_for_path(const std::vector<MountEntry>& mounts, const std::string& path) {
    const MountEntry* best = nullptr;
    for (const auto& m : mounts) {
        const std::string& mp = m.mount_point;
        bool under = mp == "/" || path == mp || (path.size() > mp.size() && path.compare(0, mp.size(), mp) == 0 && path[mp.size()] == '/');
        if (under && (!best || mp.size() >= best->mount_point.size())) best = &m;
    }
    return best;
}

std::string fs_type_warning(const std::string& t) {
    if (t == "vfat" || t == "msdos") return "FAT file system: no permissions, 2 s timestamps, case-insensitive; fine for game data, not for saves";
    if (t == "exfat") return "exFAT: no permissions or symlinks, case-insensitive on disk";
    if (t == "ntfs" || t == "ntfs3" || t == "fuseblk") return "NTFS: case-insensitive, slow metadata through FUSE, no Unix permissions";
    if (t == "nfs" || t == "nfs4" || t == "cifs" || t == "smb3" || t == "9p" || t == "sshfs" || t == "fuse.sshfs") return "network file system: mmap and file locking may misbehave, high latency for loose files";
    if (t == "overlay") return "overlay file system (container): writes go to a volatile layer";
    return std::string();
}

// ---------------------------------------------------------------------------- core dumps

CoreDumpInfo describe_core_dumps(const IFileSystem& fs, uint64_t soft) {
    CoreDumpInfo c;
    c.core_pattern = fs.read_line("/proc/sys/kernel/core_pattern");
    c.soft_limit_bytes = soft;
    c.piped = !c.core_pattern.empty() && c.core_pattern[0] == '|';
    c.systemd_coredump = c.piped && c.core_pattern.find("systemd-coredump") != std::string::npos;
    c.apport = c.piped && c.core_pattern.find("apport") != std::string::npos;
    if (soft == 0) c.hints.push_back("core dumps are disabled (ulimit -c 0): the engine raises the soft limit to the hard limit at startup, or run `ulimit -c unlimited`");
    if (c.systemd_coredump) c.hints.push_back("systemd-coredump collects crashes: use `coredumpctl list` and `coredumpctl debug`");
    else if (c.apport) c.hints.push_back("apport collects crashes: see /var/crash");
    else if (c.piped) c.hints.push_back("core dumps are piped to a handler (" + c.core_pattern + ")");
    else if (!c.core_pattern.empty()) c.hints.push_back("core dumps are written as '" + c.core_pattern + "' in the working directory");
    return c;
}

// ---------------------------------------------------------------------------- frame limiter

SleepPlan plan_sleep(int64_t now, int64_t deadline, int64_t margin) {
    SleepPlan p;
    p.spin_until_ns = deadline;
    if (deadline <= now) return p;
    int64_t coarse = deadline - margin;
    if (coarse > now) {
        p.needs_sleep = true;
        p.sleep_until_ns = coarse;
        p.needs_spin = margin > 0;
    } else {
        p.needs_spin = true;
    }
    return p;
}

}  // namespace nue::lx
