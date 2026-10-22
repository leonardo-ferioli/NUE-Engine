#include "crc32.hpp"
#include "storage_paths.hpp"
#include "testing.hpp"

using namespace nue::android;

TEST(storage, crc32_known_vectors) {
    CHECK_EQ(crc32("", 0), 0u);
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
    // incremental == one shot, across the slicing-by-8 boundary
    const char* text = "The quick brown fox jumps over the lazy dog";
    Crc32 c;
    c.update(text, 5);
    c.update(text + 5, 17);
    c.update(text + 22, 21);
    CHECK_EQ(c.value(), 0x414FA339u);
}

TEST(storage, sanitize_accepts_pc_paths) {
    SafePath p = sanitize_relative_path("GLOBAL\\GlobalB.lzc");
    CHECK(p.status == PathStatus::Ok);
    CHECK_EQ(p.path, std::string("global/globalb.lzc"));
    p = sanitize_relative_path("a//b/./c.bin", NameCase::Preserve);
    CHECK_EQ(p.path, std::string("a/b/c.bin"));
    p = sanitize_relative_path("CARS/BMW M3 GTR/geometry.bin");
    CHECK_EQ(p.path, std::string("cars/bmw m3 gtr/geometry.bin"));
}

TEST(storage, sanitize_rejects_escapes) {
    CHECK(sanitize_relative_path("").status == PathStatus::Empty);
    CHECK(sanitize_relative_path("/etc/passwd").status == PathStatus::Absolute);
    CHECK(sanitize_relative_path("\\\\server\\share\\x").status == PathStatus::Absolute);
    CHECK(sanitize_relative_path("C:\\Windows\\x").status == PathStatus::Absolute);
    CHECK(sanitize_relative_path("c:x").status == PathStatus::Absolute);
    CHECK(sanitize_relative_path("a/../../b").status == PathStatus::Traversal);
    CHECK(sanitize_relative_path("..").status == PathStatus::Traversal);
    CHECK(sanitize_relative_path("a/b\x01").status == PathStatus::BadChar);
    CHECK(sanitize_relative_path("a/b:c").status == PathStatus::BadChar);
    CHECK(sanitize_relative_path("a/b*.txt").status == PathStatus::BadChar);
    CHECK(sanitize_relative_path(std::string(300, 'x')).status == PathStatus::TooLong);
    CHECK(sanitize_relative_path(".").status == PathStatus::Empty);
    std::string nul = "a";
    nul.push_back('\0');
    nul += "b";
    CHECK(sanitize_relative_path(nul).status == PathStatus::BadChar);
}

TEST(storage, manifest_parse_and_errors) {
    const char* text =
        "# nue manifest\n"
        "1024 deadbeef GLOBAL\\A.BIN\n"
        "? 5 - optional file.txt\r\n"
        "7 - ../evil\n"
        "xx 00000000 bad size\n"
        "9 zz bad crc\n"
        "10 00000001 global/a.bin\n"  // duplicate after case folding
        "\n"
        "33 000000ff dir/with space/f.dat";
    Manifest m = parse_manifest(text);
    CHECK_EQ(m.entries.size(), size_t(3));
    CHECK_EQ(m.errors.size(), size_t(4));
    CHECK_EQ(m.entries[0].path, std::string("global/a.bin"));
    CHECK(m.entries[0].has_crc && m.entries[0].crc == 0xDEADBEEFu);
    CHECK(m.entries[1].optional && !m.entries[1].has_crc && m.entries[1].path == "optional file.txt");
    CHECK_EQ(m.entries[2].path, std::string("dir/with space/f.dat"));
    CHECK_EQ(m.required_bytes(), uint64_t(1024 + 33));
    CHECK(m.find("global/a.bin") != nullptr);
    CHECK(m.find("nope") == nullptr);
    // formatting round trip
    Manifest again = parse_manifest(format_manifest_line(m.entries[0]) + "\n" + format_manifest_line(m.entries[1]));
    CHECK_EQ(again.entries.size(), size_t(2));
    CHECK(again.errors.empty() && again.entries[0].crc == 0xDEADBEEFu && again.entries[1].optional);
}

TEST(storage, free_space_margin) {
    // 1.2 GB of game data: margin 2 % = 24 MB < 64 MiB minimum, so 64 MiB applies.
    uint64_t need = 1200ull << 20;
    SpaceReport ok = check_free_space(need, need + (64ull << 20));
    CHECK(ok.result == SpaceCheck::Ok);
    SpaceReport bad = check_free_space(need, need + (63ull << 20));
    CHECK(bad.result == SpaceCheck::NotEnough);
    CHECK_EQ(bad.shortfall, uint64_t(1ull << 20));
    // big import: 2 % dominates
    SpaceReport big = check_free_space(100ull << 30, 100ull << 30);
    CHECK(big.result == SpaceCheck::NotEnough);
    CHECK_EQ(big.needed, (100ull << 30) + (100ull << 30) / 50);
}

TEST(storage, layout_and_markers) {
    ImportLayout l = ImportLayout::for_root("/data/user/0/app/files/gamedata/");
    CHECK_EQ(l.root, std::string("/data/user/0/app/files/gamedata"));
    CHECK_EQ(l.journal, std::string("/data/user/0/app/files/gamedata/.import/journal.txt"));
    CHECK_EQ(l.part_path("/x/y.bin"), std::string("/x/y.bin.part"));

    std::string marker = make_complete_marker(1, 2345, 1234567890123ull);
    uint32_t v = 0, f = 0;
    uint64_t b = 0;
    CHECK(parse_complete_marker(marker, &v, &f, &b));
    CHECK(v == 1 && f == 2345 && b == 1234567890123ull);
    CHECK(!parse_complete_marker("nue-import-complete 1 2", &v, &f, &b));
    CHECK(!parse_complete_marker("something else 1 2 3", &v, &f, &b));
    CHECK(!parse_complete_marker("nue-import-complete 1 2 3 4", &v, &f, &b));
}

TEST(storage, journal_tolerates_torn_line) {
    std::string j = format_journal_line({"a/b.bin", 100, 0x11223344}) + format_journal_line({"c d.bin", 7, 0xAABBCCDD});
    j += "55 0000";  // killed while appending: no newline, must be ignored
    std::vector<JournalEntry> e = parse_journal(j);
    CHECK_EQ(e.size(), size_t(2));
    CHECK_EQ(e[0].path, std::string("a/b.bin"));
    CHECK_EQ(e[1].path, std::string("c d.bin"));
    CHECK_EQ(e[1].crc, 0xAABBCCDDu);
}

TEST(storage, path_helpers) {
    CHECK_EQ(join_path("a", "b"), std::string("a/b"));
    CHECK_EQ(join_path("a/", "/b"), std::string("a/b"));
    CHECK_EQ(join_path("", "b"), std::string("b"));
    CHECK_EQ(parent_path("a/b/c"), std::string("a/b"));
    CHECK_EQ(parent_path("c"), std::string(""));
    CHECK_EQ(to_lower_ascii("ÄbC"), std::string("Äbc")); // non-ASCII bytes untouched
}
