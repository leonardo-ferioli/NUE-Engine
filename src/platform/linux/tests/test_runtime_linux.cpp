// Linux-only tests: exercise the real system calls. They use only temporary files and never
// open a window, an audio device or the GPU. Tests that depend on optional services (D-Bus,
// GameMode, io_uring) check that the failure path is graceful and pass without the service.
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "lx_crash.hpp"
#include "lx_dbus.hpp"
#include "lx_gamemode.hpp"
#include "lx_runtime.hpp"
#include "lx_sched.hpp"
#include "lx_steamos_runtime.hpp"
#include "lx_storage.hpp"
#include "lx_test.hpp"

using namespace nue::lx;

namespace {

std::string make_temp_dir() {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/nue-lx-test-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back(0);
    return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string();
}

void write_text(const std::string& path, const std::string& s) {
    std::ofstream f(path, std::ios::binary);
    f << s;
}

}  // namespace

TEST(rt_game_files_case_insensitive_on_real_fs) {
    std::string dir = make_temp_dir();
    CHECK(!dir.empty());
    make_dirs_posix(dir + "/GLOBAL");
    write_text(dir + "/GLOBAL/GlobalA.BUN", "payload");
    StdFs fs;
    PathResolveOptions o;
    o.base_dir = dir;
    PathResolver r(fs, o);
    GameFiles gf(r);
    int fd = gf.open_read("global\\globala.bun");
    CHECK(fd >= 0);
    char buf[8] = {};
    CHECK(pread_full(fd, buf, 7, 0));
    CHECK_EQ(std::string(buf), std::string("payload"));
    ::close(fd);
    uint64_t size = 0;
    CHECK(gf.file_size("GLOBAL\\GLOBALA.BUN", size) && size == 7);
    CHECK_EQ(gf.open_read("global\\missing.bun"), -ENOENT);

    // create through a differently spelled directory name; the file lands in the real directory
    int wfd = gf.open_write("Global\\new.sav", true);
    CHECK(wfd >= 0);
    CHECK(write_full(wfd, "x", 1));
    ::close(wfd);
    CHECK(gf.exists("GLOBAL/NEW.SAV"));  // the file on disk is "new.sav": lookups fold case
    CHECK(gf.exists("global\\NEW.sav"));
    CHECK(gf.make_dirs("Saves\\Slot1\\Profile"));
    CHECK(gf.exists("saves/slot1/profile"));
    CHECK(gf.remove("global\\new.sav"));
    CHECK(!gf.exists("global\\new.sav"));
}

TEST(rt_atomic_write_and_filemap) {
    std::string dir = make_temp_dir();
    std::string f = dir + "/settings.bin";
    const char data[] = "0123456789";
    CHECK(write_file_atomic(f, data, 10));
    CHECK(write_file_atomic(f, "abc", 3));  // replace
    std::string back;
    CHECK(read_whole_file(f, back) && back == "abc");
    FileMap m;
    CHECK(m.open_readonly(f));
    CHECK_EQ(m.size(), size_t(3));
    CHECK(std::memcmp(m.data(), "abc", 3) == 0);
    m.advise_sequential();
    uint64_t avail = 0;
    CHECK(free_space_bytes(dir, avail) && avail > 0);
}

TEST(rt_instance_lock_is_exclusive) {
    std::string dir = make_temp_dir();
    InstanceLock a, b;
    CHECK(a.acquire(dir + "/lock"));
    CHECK(!b.acquire(dir + "/lock"));
    a.release();
    CHECK(b.acquire(dir + "/lock"));
}

namespace {
void async_roundtrip(bool uring) {
    std::string dir = make_temp_dir();
    std::string f = dir + "/blob";
    std::string content(1 << 20, 'a');
    for (size_t i = 0; i < content.size(); i += 4099) content[i] = static_cast<char>('b' + i % 7);
    CHECK(write_file_atomic(f, content.data(), content.size()));
    int fd = ::open(f.c_str(), O_RDONLY);
    CHECK(fd >= 0);
    AsyncReader ar;
    AsyncReaderOptions o;
    o.prefer_io_uring = uring;
    CHECK(ar.start(o));
    const int N = 64;
    std::vector<std::string> bufs(N, std::string(16384, 0));
    std::mutex m;
    std::condition_variable cv;
    int done = 0;
    std::atomic<int> bad{0};
    for (int i = 0; i < N; ++i) {
        uint64_t off = static_cast<uint64_t>(i) * 16384;
        CHECK(ar.read(fd, &bufs[i][0], 16384, off, [&, i, off](int64_t r) {
            if (r != 16384 || std::memcmp(bufs[i].data(), content.data() + off, 16384) != 0) ++bad;
            std::lock_guard<std::mutex> g(m);
            ++done;
            cv.notify_all();
        }));
    }
    {
        std::unique_lock<std::mutex> l(m);
        CHECK(cv.wait_for(l, std::chrono::seconds(10), [&] { return done == N; }));
    }
    CHECK_EQ(bad.load(), 0);
    // short read at EOF
    std::string tail(8192, 0);
    int64_t got = -1;
    std::atomic<bool> got_set{false};
    CHECK(ar.read(fd, &tail[0], 8192, content.size() - 100, [&](int64_t r) { got = r; got_set = true; }));
    for (int i = 0; i < 1000 && !got_set; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(got, int64_t(100));
    ar.stop();
    ::close(fd);
}
}  // namespace

TEST(rt_async_reader_thread_pool) { async_roundtrip(false); }

TEST(rt_async_reader_io_uring_or_graceful_fallback) {
    // Where io_uring is blocked (seccomp, old kernel) start() silently uses the pool; both must read correctly.
    async_roundtrip(true);
    AsyncReader ar;
    AsyncReaderOptions o;
    o.prefer_io_uring = true;
    CHECK(ar.start(o));
    std::string b = ar.backend();
    CHECK(b == "io_uring" || b == "thread-pool");
}

TEST(rt_timers) {
    int64_t t0 = monotonic_ns();
    int64_t deadline = t0 + 5'000'000;  // 5 ms
    sleep_until_ns(deadline);
    int64_t t1 = monotonic_ns();
    CHECK(t1 >= deadline);
    CHECK(t1 - deadline < 5'000'000);  // generous: a loaded CI machine may add jitter, but not 5 ms of it
    PeriodicTimer p;
    CHECK(p.start(2'000'000));
    uint64_t ticks = 0;
    int64_t a = monotonic_ns();
    for (int i = 0; i < 5; ++i) ticks += p.wait();
    int64_t b = monotonic_ns();
    CHECK(ticks >= 5);
    CHECK(b - a >= 7'000'000);  // five 2 ms periods, first tick after one period
}

TEST(rt_scheduling_helpers_report_instead_of_aborting) {
    CHECK(current_tid() > 0);
    CHECK(set_thread_name("nue-test-thread"));
    std::vector<int> cpus;
    CHECK(get_thread_affinity(cpus) && !cpus.empty());
    CHECK(set_thread_affinity(cpus));  // re-applying the current mask is always allowed
    PriorityLimits lim = query_priority_limits();
    int cur_nice = 0;
    CHECK(get_thread_nice(cur_nice));
    // RLIMIT_RTPRIO / RLIMIT_NICE are 0 in containers and CI runners: only sanity is guaranteed, never a privilege.
    CHECK(lim.max_rt_priority >= 0 && lim.max_rt_priority <= 99);
    CHECK(lim.min_nice <= cur_nice && lim.min_nice >= -20);
    set_thread_nice(1);  // raising niceness is always allowed
    set_io_priority_best_effort(4);
    set_timer_slack_ns(1000);
    std::string applied = raise_thread_priority(false, "test");
    CHECK(!applied.empty());
}

TEST(rt_optional_services_fail_gracefully) {
    // None of these may crash or hang whether or not the services exist on the test machine.
    GameModeSession gm;
    GameModeOptions o;
    o.use_gamemode = true;
    o.use_power_profile = true;
    o.inhibit_idle = false;  // do not touch the desktop session during tests
    GameModeState st = gm.enter(o);
    CHECK(!st.messages.empty());
    gm.leave();
    DBusConn c;
    bool session = c.open(BusType::Session);
    if (session) CHECK(c.name_has_owner("org.freedesktop.DBus"));
    RuntimeCaps caps = query_runtime_caps();
    CHECK(!caps.libraries.empty());
}

TEST(rt_crash_handler_writes_report) {
    std::string dir = make_temp_dir();
    CrashOptions o;
    o.crash_dir = dir;
    o.app_name = "nue-test";
    o.app_version = "0.0";
    CHECK(install_crash_handler(o));
    std::string report = crash_handler_selftest(dir);
    CHECK(!report.empty());
    std::string text;
    CHECK(read_whole_file(report, text));
    CHECK(text.find("nue-test 0.0 crashed") != std::string::npos);
    CHECK(text.find("SIGSEGV") != std::string::npos);
    CHECK(text.find("maps:") != std::string::npos);
    uninstall_crash_handler();
}

TEST(rt_steamos_services_fail_gracefully) {
    SuspendWatcher w;
    std::atomic<int> calls{0};
    bool started = w.start([&](bool) { ++calls; });
    if (started) {
        CHECK(w.running());
        w.stop();
        CHECK(!w.running());
    } else {
        CHECK(!w.error().empty());  // no logind / no system bus: reported, not fatal
    }
    SteamOsManagerInfo m = query_steamos_manager();
    CHECK(m.available || !m.error.empty());
    CHECK(spawn_detached({"/bin/true"}));
    CHECK(!spawn_detached({}));
    CHECK(!spawn_detached({"/nonexistent/program"}));
    SteamworksLite sw;
    if (!sw.load({"/nonexistent-dir"})) {
        CHECK(!sw.loaded() && !sw.steam_running() && !sw.initialized());
        std::string err;
        CHECK(!sw.init(err));
    }
}
