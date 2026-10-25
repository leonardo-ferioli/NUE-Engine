#include "lx_fs.hpp"
#include "lx_pathcase.hpp"
#include "lx_test.hpp"
#include "lx_text.hpp"

using namespace nue::lx;

TEST(text_basics) {
    CHECK_EQ(std::string(trim("  a b \r\n")), std::string("a b"));
    CHECK_EQ(split("a,,b", ',').size(), size_t(2));
    CHECK_EQ(split("a,,b", ',', true).size(), size_t(3));
    uint64_t v = 0;
    CHECK(parse_u64("0x1F", v, 0) && v == 31);
    CHECK(parse_u64("10de", v, 16) && v == 0x10de);
    CHECK(!parse_u64("12x", v));
    CHECK(!parse_u64("18446744073709551616", v));
    int64_t i = 0;
    CHECK(parse_i64("-42", i) && i == -42);
    std::vector<int> cpus;
    CHECK(parse_cpulist("0-3,8,10-11", cpus));
    CHECK_EQ(cpus.size(), size_t(7));
    CHECK_EQ(format_cpulist(cpus), std::string("0-3,8,10-11"));
    CHECK(!parse_cpulist("3-1", cpus));
    CHECK(parse_cpulist("", cpus) && cpus.empty());
}

TEST(text_fold_and_wildcards) {
    CHECK_EQ(fold_name("GlobalA.BUN"), std::string("globala.bun"));
    CHECK_EQ(fold_name("\xC3\x89t\xC3\xA9"), fold_name("\xC3\xA9T\xC3\x89"));  // Été == éTÉ
    CHECK_EQ(fold_name("\xD0\x90"), std::string("\xD0\xB0"));                  // А -> а
    CHECK(wildcard_match("*.bun", "GLOBALA.BUN"));
    CHECK(wildcard_match("GLOBAL?.bun", "globala.BUN"));
    CHECK(!wildcard_match("*.bun", "a.bin"));
    CHECK(wildcard_match("*.*", "noextension"));
    CHECK(wildcard_match("a*b*c", "aXXbYYc"));
    CHECK(!wildcard_match("a*b*c", "aXXbYY"));
}

TEST(memfs_basics) {
    MemFs fs;
    fs.add_file("/g/a/b.txt", "hi");
    CHECK(fs.is_dir("/g/a"));
    CHECK(fs.stat("/g/a/b.txt").is_file);
    std::vector<DirEntry> e;
    CHECK(fs.list_dir("/g", e) && e.size() == 1 && e[0].name == "a" && e[0].is_dir);
    std::string s;
    CHECK(fs.read_file("/g/a/b.txt", s) && s == "hi");
    int64_t m0 = fs.stat("/g/a").mtime_ns;
    fs.add_file("/g/a/c.txt", "x");
    CHECK(fs.stat("/g/a").mtime_ns > m0);  // creating a child bumps the directory mtime
    fs.remove("/g/a");
    CHECK(!fs.exists("/g/a/b.txt"));
}

TEST(path_normalize) {
    auto n = PathResolver::normalize("C:\\Games\\NFSU2\\.\\Cars\\..\\GLOBAL\\x.bun");
    CHECK_EQ(n.drive, 'C');
    CHECK(n.rooted);
    CHECK_EQ(n.parts.size(), size_t(4));  // Games NFSU2 GLOBAL x.bun -> 4 parts? (Cars folded away)
    CHECK_EQ(n.parts[2], std::string("GLOBAL"));
    auto r = PathResolver::normalize(".\\a\\b\\");
    CHECK(!r.rooted && r.drive == 0 && r.trailing_slash && r.parts.size() == 2);
    auto h = PathResolver::normalize("/home/u/x");
    CHECK(h.host_absolute && h.parts.size() == 3);
    auto up = PathResolver::normalize("..\\..\\a");
    CHECK(up.parts.size() == 1 && up.parts[0] == "a");  // cannot climb out
    auto dev = PathResolver::normalize("\\\\?\\D:\\data\\f.bin");
    CHECK(dev.drive == 'D' && dev.parts.size() == 2);
    auto rooted = PathResolver::normalize("\\Windows\\system32");
    CHECK(rooted.rooted && rooted.drive == 0 && rooted.parts.size() == 2);
}

namespace {
void make_game(MemFs& fs) {
    fs.add_file("/game/GLOBAL/GlobalA.BUN", "a");
    fs.add_file("/game/GLOBAL/Fonts/Arial.FFN", "f");
    fs.add_file("/game/CARS/PEUGEOT_206/GEOMETRY.BIN", "g");
    fs.add_file("/game/config.ini", "c");
    fs.advance_ns(10'000'000'000);  // directories are old enough to be trusted by the cache
}
PathResolveOptions opts() {
    PathResolveOptions o;
    o.base_dir = "/game";
    return o;
}
}  // namespace

TEST(resolve_case_insensitive) {
    MemFs fs;
    make_game(fs);
    PathResolver r(fs, opts());
    auto a = r.resolve("global\\globala.bun");
    CHECK(a.found && a.case_changed && !a.ambiguous);
    CHECK_EQ(a.path, std::string("/game/GLOBAL/GlobalA.BUN"));
    auto b = r.resolve(".\\Cars\\peugeot_206\\geometry.bin");
    CHECK(b.found);
    CHECK_EQ(b.path, std::string("/game/CARS/PEUGEOT_206/GEOMETRY.BIN"));
    auto c = r.resolve("CONFIG.INI");
    CHECK(c.found && c.path == "/game/config.ini");
    auto exact = r.resolve("GLOBAL/GlobalA.BUN");
    CHECK(exact.found && !exact.case_changed);
}

TEST(resolve_windows_absolute_forms) {
    MemFs fs;
    make_game(fs);
    PathResolveOptions o = opts();
    PathResolver nomount(fs, o);
    // No drive mounted: drive is stripped and the rest is relative to the game directory.
    auto a = nomount.resolve("C:\\GLOBAL\\globala.bun");
    CHECK(a.found && a.path == "/game/GLOBAL/GlobalA.BUN");
    o.drive_roots['D'] = "/game/CARS";
    PathResolver mounted(fs, o);
    auto b = mounted.resolve("d:\\peugeot_206\\geometry.bin");
    CHECK(b.found && b.path == "/game/CARS/PEUGEOT_206/GEOMETRY.BIN");
    auto host = mounted.resolve("/game/global/globala.bun");
    CHECK(host.found && host.case_changed);
}

TEST(resolve_missing_and_create) {
    MemFs fs;
    make_game(fs);
    PathResolver r(fs, opts());
    auto n = r.resolve("global\\new.sav");  // directory matches, file does not exist: can be created
    CHECK(!n.found && n.parent_found);
    CHECK_EQ(n.path, std::string("/game/GLOBAL/new.sav"));
    auto m = r.resolve("nodir\\x\\y.bin");
    CHECK(!m.found && !m.parent_found);
    CHECK_EQ(m.path, std::string("/game/nodir/x/y.bin"));
    auto f = r.resolve("config.ini\\child");  // a file where a directory is needed
    CHECK(!f.found && !f.parent_found);
}

TEST(resolve_ambiguous_is_deterministic) {
    MemFs fs;
    fs.add_file("/game/Data/readme.TXT", "1");
    fs.add_file("/game/Data/README.txt", "2");
    fs.advance_ns(10'000'000'000);
    PathResolver r(fs, opts());
    auto a = r.resolve("data\\Readme.txt");
    CHECK(a.found && a.ambiguous);
    CHECK_EQ(a.path, std::string("/game/Data/README.txt"));  // byte-wise smallest
    auto exact = r.resolve("Data/readme.TXT");
    CHECK(exact.found && !exact.ambiguous);  // exact spelling always wins
    CHECK_EQ(exact.path, std::string("/game/Data/readme.TXT"));
}

TEST(resolve_cache_costs_no_listing_on_hits) {
    MemFs fs;
    make_game(fs);
    PathResolver r(fs, opts());
    r.resolve("global\\globala.bun");
    int lists = fs.list_calls();
    for (int i = 0; i < 100; ++i) r.resolve("GLOBAL\\GLOBALA.BUN");
    CHECK_EQ(fs.list_calls(), lists);  // hits are served from the index
    CHECK(r.stats().cache_hits >= 100);
}

TEST(resolve_sees_new_files_after_miss) {
    MemFs fs;
    make_game(fs);
    PathResolver r(fs, opts());
    CHECK(!r.resolve("global\\late.bun").found);
    fs.add_file("/game/GLOBAL/Late.Bun", "x");  // another process creates it
    auto a = r.resolve("global\\late.bun");
    CHECK(a.found && a.path == "/game/GLOBAL/Late.Bun");
}

TEST(resolve_negative_lookup_does_not_rescan_stable_dirs) {
    MemFs fs;
    make_game(fs);
    PathResolver r(fs, opts());
    r.resolve("global\\nothere1");
    int lists = fs.list_calls();
    for (int i = 0; i < 20; ++i) r.resolve("global\\nothere1");
    CHECK_EQ(fs.list_calls(), lists);  // old directory + unchanged mtime: a miss costs one stat
}

TEST(resolve_recent_dir_rescans_on_miss) {
    MemFs fs;
    fs.add_file("/game/a/one", "1");  // no time advance: directory is "too fresh to trust"
    PathResolver r(fs, opts());
    CHECK(!r.resolve("a/two").found);
    fs.add_file("/game/a/two", "2");  // same coarse mtime tick in real file systems
    CHECK(r.resolve("a/two").found);
}

TEST(list_matching_wildcards) {
    MemFs fs;
    make_game(fs);
    fs.add_file("/game/GLOBAL/Extra.bun", "e");
    fs.advance_ns(10'000'000'000);
    PathResolver r(fs, opts());
    std::vector<DirEntry> out;
    CHECK(r.list_matching("global", "*.BUN", out));
    CHECK_EQ(out.size(), size_t(2));
    CHECK(out[0].name == "Extra.bun" && out[1].name == "GlobalA.BUN");
    CHECK(!r.list_matching("missingdir", "*", out));
}
