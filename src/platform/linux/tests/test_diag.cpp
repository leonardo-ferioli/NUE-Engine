// Portable tests for the diagnostics helpers and the thread pool.
#include <atomic>
#include <chrono>
#include <thread>

#include "lx_diag.hpp"
#include "lx_signal_safe.hpp"
#include "lx_taskqueue.hpp"
#include "lx_test.hpp"

using namespace nue::lx;

TEST(signal_safe_formatting) {
    char buf[64];
    FixedWriter w(buf, sizeof buf);
    w.str("sig ");
    w.dec(-42);
    w.chr(' ');
    w.udec(18446744073709551615ull);
    w.chr(' ');
    w.hex(0xdeadbeef, 16);
    CHECK_EQ(std::string(w.data(), w.size()), std::string("sig -42 18446744073709551615 0x00000000deadbeef"));
    char tiny[4];
    FixedWriter t(tiny, sizeof tiny);
    t.str("abcdefgh");
    CHECK_EQ(t.size(), size_t(4));  // truncates, never overflows
    CHECK_EQ(std::string(signal_name(11)), std::string("SIGSEGV"));
    CHECK_EQ(std::string(signal_name(7)), std::string("SIGBUS"));
    CHECK_EQ(std::string(signal_name(99)), std::string("SIG?"));
}

TEST(mountinfo_parsing_and_lookup) {
    const char* text =
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "40 22 259:3 / /home rw,relatime shared:2 - btrfs /dev/nvme0n1p3 rw,subvol=/@home\n"
        "55 22 8:17 / /run/media/leo/GAMES\\040DISK ro,nosuid - exfat /dev/sdb1 ro\n"
        "60 22 0:50 / /mnt/nas rw - cifs //nas/share rw\n";
    auto m = parse_mountinfo(text);
    CHECK_EQ(m.size(), size_t(4));
    CHECK_EQ(m[2].mount_point, std::string("/run/media/leo/GAMES DISK"));
    CHECK(m[2].read_only);
    const MountEntry* e = mount_for_path(m, "/home/leo/.local/share/game");
    CHECK(e && e->fs_type == "btrfs");
    e = mount_for_path(m, "/run/media/leo/GAMES DISK/NFSU2/GLOBAL");
    CHECK(e && e->fs_type == "exfat");
    e = mount_for_path(m, "/homeless");  // component-wise: not under /home
    CHECK(e && e->fs_type == "ext4");
    CHECK(!fs_type_warning("exfat").empty());
    CHECK(!fs_type_warning("cifs").empty());
    CHECK(fs_type_warning("ext4").empty());
}

TEST(core_dump_advice) {
    MemFs fs;
    fs.add_file("/proc/sys/kernel/core_pattern", "|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %h\n");
    CoreDumpInfo c = describe_core_dumps(fs, 0);
    CHECK(c.piped && c.systemd_coredump && !c.apport);
    CHECK_EQ(c.hints.size(), size_t(2));  // disabled limit + coredumpctl
    fs.add_file("/proc/sys/kernel/core_pattern", "core\n");
    c = describe_core_dumps(fs, UINT64_MAX);
    CHECK(!c.piped);
    CHECK_EQ(c.hints.size(), size_t(1));
}

TEST(sleep_plan) {
    SleepPlan p = plan_sleep(1000, 5000, 1500);
    CHECK(p.needs_sleep && p.needs_spin);
    CHECK_EQ(p.sleep_until_ns, int64_t(3500));
    CHECK_EQ(p.spin_until_ns, int64_t(5000));
    p = plan_sleep(1000, 2000, 1500);  // too close for a coarse sleep: spin only
    CHECK(!p.needs_sleep && p.needs_spin);
    p = plan_sleep(5000, 4000, 100);  // deadline passed
    CHECK(!p.needs_sleep && !p.needs_spin);
}

TEST(task_queue_runs_everything_before_shutdown_returns) {
    std::atomic<int> n{0};
    std::atomic<int> started{0};
    {
        TaskQueue q(3, [&](size_t) { ++started; });
        for (int i = 0; i < 200; ++i) CHECK(q.post([&] { ++n; }));
        q.shutdown();
        CHECK(!q.post([] {}));  // closed
    }
    CHECK_EQ(n.load(), 200);
    CHECK_EQ(started.load(), 3);
}
