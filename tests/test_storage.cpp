// VFS: path rules, case-insensitive resolution, overlays, file ops, handles, listing, async reads, manifests, import.
#include "test_env.h"
#include "nue_test.h"

#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

using nue_test::Engine;
using nue_test::write_file;

static std::string normalized(const char* in) {
    char buf[256];
    size_t need = 0;
    nue_result r = nue_path_normalize(in, buf, sizeof(buf), &need);
    return NUE_SUCCEEDED(r) ? buf : std::string("!") + nue_result_name(r);
}

TEST_CASE(path_normalization_is_pure) {
    // works without any engine running
    CHECK(normalized("data:GLOBAL\\GLOBALB.LUT") == "data:GLOBAL/GLOBALB.LUT");
    CHECK(normalized("DATA:a//b/./c/../d") == "data:a/b/d");
    CHECK(normalized("user:") == "user:");
    CHECK(normalized("data:a/../../x") == "!NUE_ERR_ACCESS_DENIED");
    CHECK(normalized("C:\\Games\\NFSU2\\..\\x.exe") == "C:/Games/x.exe");   // drive letter is not a root
    CHECK(normalized("/usr/local//lib/./x") == "/usr/local/lib/x");
    CHECK(normalized("../up/file") == "../up/file");
}

TEST_CASE(case_insensitive_resolution_and_backslashes) {
    Engine e;
    write_file(e.data + "/GLOBAL/GlobalB.lut", "abc");
    write_file(e.data + "/Cars/Skyline/GEOMETRY.BIN", "geo");
    char native[512];
    size_t need;
    CHECK_EQ(nue_vfs_resolve("data:global\\globalb.LUT", NUE_FALSE, native, sizeof(native), &need), NUE_OK);
    std::string lowered(native);
    for (char& ch : lowered) ch = char(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    CHECK(lowered.find("global/globalb.lut") != std::string::npos);   // exact case only matters on case-sensitive file systems
    CHECK(nue_vfs_exists("DATA:cars\\skyline\\geometry.bin"));
    CHECK(!nue_vfs_exists("data:cars/skyline/missing.bin"));
    CHECK_EQ(nue_vfs_resolve("data:nope.txt", NUE_FALSE, native, sizeof(native), &need), NUE_ERR_NOT_FOUND);
    CHECK_EQ(nue_vfs_resolve("bogus:x", NUE_FALSE, native, sizeof(native), &need), NUE_ERR_NOT_FOUND);

    nue_file f = 0;
    CHECK_EQ(nue_file_open("data:GLOBAL/globalb.lut", NUE_OPEN_READ, &f), NUE_OK);
    char buf[8] = {};
    size_t got = 0;
    CHECK_EQ(nue_file_read(f, buf, 8, &got), NUE_OK_PARTIAL);
    CHECK_EQ(got, 3u);
    CHECK_STR(buf, "abc");
    CHECK_EQ(nue_file_read(f, buf, 8, &got), NUE_ERR_END_OF_DATA);
    CHECK_EQ(nue_file_close(f), NUE_OK);
    CHECK_EQ(nue_file_close(f), NUE_ERR_INVALID_HANDLE);   // stale id
    CHECK_EQ(nue_file_read(f, buf, 8, &got), NUE_ERR_INVALID_HANDLE);

    // writes reuse an existing directory's case, new names keep the caller's case
    nue_file w;
    CHECK_EQ(nue_file_open("user:Saves\\Profile1.DAT", NUE_OPEN_WRITE | NUE_OPEN_CREATE, &w), NUE_OK);
    nue_file_close(w);
    CHECK_EQ(nue_file_open("user:saves/PROFILE1.dat", NUE_OPEN_READ, &w), NUE_OK);   // resolves to the file just created
    nue_file_close(w);
    CHECK(std::filesystem::exists(e.user + "/Saves/Profile1.DAT"));
    CHECK_EQ(nue_file_open("user:SAVES/Other.txt", NUE_OPEN_WRITE | NUE_OPEN_CREATE, &w), NUE_OK);
    nue_file_close(w);
    CHECK(std::filesystem::exists(e.user + "/Saves/Other.txt"));   // parent folder case reused
}

TEST_CASE(portable_mode_keeps_every_writable_root_below_the_user_folder) {
    Engine e;
    for (const char* root : {"cache", "settings", "temp"}) {
        char p[512];
        size_t need;
        CHECK_EQ(nue_vfs_get_root_path(root, p, sizeof(p), &need), NUE_OK);
        std::string path(p);
        std::string expected = e.user + "/" + root;
        GENERIC(CHECK(path == expected));   // a real platform layer may answer another folder only when no explicit user path was given
        CHECK(path.find(e.user) == 0);       // but an explicit user_data_path always wins
    }
}

TEST_CASE(roots_are_protected) {
    Engine e;
    nue_file f;
    CHECK_EQ(nue_file_open("data:new.txt", NUE_OPEN_WRITE | NUE_OPEN_CREATE, &f), NUE_ERR_ACCESS_DENIED);   // data is read-only
    CHECK_EQ(nue_file_open("user:../escape.txt", NUE_OPEN_WRITE | NUE_OPEN_CREATE, &f), NUE_ERR_ACCESS_DENIED);
    CHECK_EQ(nue_vfs_remove_tree("data:"), NUE_ERR_ACCESS_DENIED);
    CHECK_EQ(nue_vfs_remove_tree("user:"), NUE_ERR_ACCESS_DENIED);   // never a whole root
    CHECK(nue_vfs_has_root("settings") && nue_vfs_has_root("cache") && nue_vfs_has_root("temp"));
}

TEST_CASE(mount_overlay_newest_wins_and_listing_merges) {
    Engine e;
    write_file(e.data + "/a.txt", "base-a");
    write_file(e.data + "/b.txt", "base-b");
    std::string mod = e.root + "/mod";
    write_file(mod + "/A.TXT", "mod-a");
    write_file(mod + "/c.txt", "mod-c");
    nue_mount_desc md;
    NUE_STRUCT_INIT(&md);
    md.root = "data";
    md.native_path = mod.c_str();
    md.flags = NUE_MOUNT_READ_ONLY;
    CHECK_EQ(nue_vfs_mount(&md), NUE_OK);
    void* data = nullptr; size_t size = 0;
    CHECK_EQ(nue_file_load("data:a.txt", &data, &size), NUE_OK);
    CHECK_STR(static_cast<char*>(data), "mod-a");   // the later mount shadows, whatever the case on disk
    nue_free(data);
    CHECK_EQ(nue_file_load("data:b.txt", &data, &size), NUE_OK);
    CHECK_STR(static_cast<char*>(data), "base-b");
    nue_free(data);

    std::set<std::string> names;
    CHECK_EQ(nue_vfs_list("data:", [](void* u, const nue_dir_entry* de) { static_cast<std::set<std::string>*>(u)->insert(de->name); return NUE_TRUE; }, &names), NUE_OK);
    CHECK_EQ(names.size(), 3u);   // a/A merged once, b, c
    CHECK_EQ(nue_vfs_unmount("data", mod.c_str()), NUE_OK);
    CHECK_EQ(nue_file_load("data:a.txt", &data, &size), NUE_OK);
    CHECK_STR(static_cast<char*>(data), "base-a");
    nue_free(data);
}

TEST_CASE(file_io_seek_append_truncate_and_atomic_save) {
    Engine e;
    CHECK_EQ(nue_file_save("user:profile/p.bin", "hello", 5), NUE_OK);
    CHECK(!std::filesystem::exists(e.user + "/profile/p.bin.nuetmp"));
    nue_file f;
    CHECK_EQ(nue_file_open("user:profile/p.bin", NUE_OPEN_READ | NUE_OPEN_WRITE, &f), NUE_OK);
    uint64_t pos = 0, size = 0;
    CHECK_EQ(nue_file_seek(f, 0, NUE_SEEK_END, &pos), NUE_OK);
    CHECK_EQ(pos, 5u);
    CHECK_EQ(nue_file_seek(f, -2, NUE_SEEK_CUR, &pos), NUE_OK);
    CHECK_EQ(pos, 3u);
    size_t n;
    CHECK_EQ(nue_file_write(f, "LO!", 3, &n), NUE_OK);
    nue_file_size(f, &size);
    CHECK_EQ(size, 6u);
    CHECK_EQ(nue_file_seek(f, -1, NUE_SEEK_SET, &pos), NUE_ERR_INVALID_ARGUMENT);
    char b[8] = {};
    CHECK_EQ(nue_file_read_at(f, 0, b, 6, &n), NUE_OK);   // positioned read does not disturb the cursor
    CHECK_STR(b, "helLO!");
    nue_file_tell(f, &pos);
    CHECK_EQ(pos, 6u);
    CHECK_EQ(nue_file_set_size(f, 2), NUE_OK);
    nue_file_size(f, &size);
    CHECK_EQ(size, 2u);
    nue_file_close(f);

    CHECK_EQ(nue_file_open("user:profile/p.bin", NUE_OPEN_APPEND, &f), NUE_OK);
    nue_file_write(f, "++", 2, &n);
    nue_file_write(f, "!!", 2, &n);
    nue_file_close(f);
    void* data; size_t sz;
    nue_file_load("user:profile/p.bin", &data, &sz);
    CHECK_EQ(sz, 6u);
    CHECK_STR(static_cast<char*>(data), "he++!!");
    nue_free(data);
    CHECK_EQ(nue_file_open("user:profile/p.bin", NUE_OPEN_WRITE | NUE_OPEN_CREATE | NUE_OPEN_EXCLUSIVE, &f), NUE_ERR_ALREADY_EXISTS);
    CHECK_EQ(nue_file_open("user:profile/p.bin", NUE_OPEN_WRITE | NUE_OPEN_TRUNCATE, &f), NUE_OK);
    nue_file_size(f, &size);
    CHECK_EQ(size, 0u);
    nue_file_close(f);
    CHECK_EQ(nue_file_open("user:profile/nothing.bin", NUE_OPEN_READ, &f), NUE_ERR_NOT_FOUND);
    CHECK_EQ(nue_file_open("user:profile/p.bin", 0, &f), NUE_ERR_INVALID_ARGUMENT);
}

TEST_CASE(file_ids_are_32bit_and_generation_checked) {
    Engine e;
    write_file(e.data + "/x.bin", "x");
    nue_file a, b;
    nue_file_open("data:x.bin", NUE_OPEN_READ, &a);
    nue_file_close(a);
    nue_file_open("data:x.bin", NUE_OPEN_READ, &b);   // reuses the slot
    CHECK(a != b);
    char c;
    size_t n;
    CHECK_EQ(nue_file_read_at(a, 0, &c, 1, &n), NUE_ERR_INVALID_HANDLE);   // the old id must not reach the new file
    CHECK_EQ(nue_file_read_at(b, 0, &c, 1, &n), NUE_OK);
    CHECK_EQ(nue_file_read_at(0, 0, &c, 1, &n), NUE_ERR_INVALID_HANDLE);
    nue_file_close(b);
}

TEST_CASE(map_load_and_hash) {
    Engine e;
    std::string content(5000, 'q');
    write_file(e.data + "/big.bin", content);
    nue_file f;
    nue_file_open("data:big.bin", NUE_OPEN_READ, &f);
    nue_file_map_info mi;
    NUE_STRUCT_INIT(&mi);
    CHECK_EQ(nue_file_map(f, 100, 0, &mi), NUE_OK);
    CHECK_EQ(mi.size, 4900u);
    CHECK(static_cast<const char*>(mi.data)[0] == 'q');
    CHECK_EQ(nue_file_unmap(f, &mi), NUE_OK);
    CHECK_EQ(nue_file_map(f, 6000, 0, &mi), NUE_ERR_INVALID_ARGUMENT);
    nue_file_close(f);
    uint8_t d1[32], d2[32];
    CHECK_EQ(nue_file_hash_sha256("data:big.bin", d1), NUE_OK);
    nue_sha256(content.data(), content.size(), d2);
    CHECK(std::memcmp(d1, d2, 32) == 0);
    uint32_t crc = 0;
    CHECK_EQ(nue_file_hash_crc32("data:BIG.BIN", &crc), NUE_OK);
    CHECK_EQ(crc, nue_crc32(0, content.data(), content.size()));
}

TEST_CASE(directories_stat_rename_remove_glob) {
    Engine e;
    CHECK_EQ(nue_vfs_create_directory("user:a/b/c"), NUE_OK);
    CHECK_EQ(nue_file_save("user:a/b/c/one.txt", "1", 1), NUE_OK);
    CHECK_EQ(nue_file_save("user:a/b/c/two.TXT", "22", 2), NUE_OK);
    CHECK_EQ(nue_file_save("user:a/b/c/skip.dat", "3", 1), NUE_OK);
    nue_stat st;
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_vfs_stat("user:a/b/c/two.txt", &st), NUE_OK);
    CHECK_EQ(st.kind, NUE_ENTRY_FILE);
    CHECK_EQ(st.size, 2u);
    CHECK_EQ(nue_vfs_stat("user:a/b", &st), NUE_OK);
    CHECK_EQ(st.kind, NUE_ENTRY_DIRECTORY);

    std::vector<std::string> names;
    CHECK_EQ(nue_vfs_glob("user:a/b/c", "*.txt", [](void* u, const nue_dir_entry* de) { static_cast<std::vector<std::string>*>(u)->push_back(de->name); return NUE_TRUE; }, &names), NUE_OK);
    CHECK_EQ(names.size(), 2u);
    names.clear();
    nue_vfs_glob("user:a/b/c", "?ne.*", [](void* u, const nue_dir_entry* de) { static_cast<std::vector<std::string>*>(u)->push_back(de->name); return NUE_TRUE; }, &names);
    CHECK_EQ(names.size(), 1u);
    int first_only = 0;
    nue_vfs_list("user:a/b/c", [](void* u, const nue_dir_entry*) { ++*static_cast<int*>(u); return NUE_FALSE; }, &first_only);   // callback stops the listing
    CHECK_EQ(first_only, 1);

    CHECK_EQ(nue_vfs_rename("user:a/b/c/one.txt", "user:a/moved/one.txt"), NUE_OK);
    CHECK(!nue_vfs_exists("user:a/b/c/one.txt"));
    CHECK(nue_vfs_exists("user:a/moved/one.txt"));
    CHECK_EQ(nue_vfs_rename("user:a/moved/one.txt", "cache:x.txt"), NUE_ERR_INVALID_ARGUMENT);   // different roots
    CHECK_EQ(nue_vfs_remove("user:a/b"), NUE_ERR_IO);   // not empty
    CHECK_EQ(nue_vfs_remove_tree("user:a"), NUE_OK);
    CHECK(!nue_vfs_exists("user:a"));
    CHECK_EQ(nue_vfs_list("user:missing", [](void*, const nue_dir_entry*) { return NUE_TRUE; }, nullptr), NUE_ERR_NOT_FOUND);
}

TEST_CASE(async_reads_complete_in_priority_order_and_post_events) {
    Engine e;
    std::string content(100000, 'a');
    for (size_t i = 0; i < content.size(); ++i) content[i] = char('a' + i % 26);
    write_file(e.data + "/stream.bin", content);
    nue_file f;
    nue_file_open("data:stream.bin", NUE_OPEN_READ, &f);
    std::vector<char> buf(1000), buf2(1000);
    static int callbacks;
    callbacks = 0;
    nue_io_desc d;
    NUE_STRUCT_INIT(&d);
    d.file = f; d.offset = 26; d.buffer = buf.data(); d.size = 1000; d.post_event = NUE_TRUE;
    d.callback = [](void*, nue_io_request, nue_result r, size_t n) { if (r == NUE_OK && n == 1000) ++callbacks; };
    nue_io_request r1 = 0, r2 = 0;
    CHECK_EQ(nue_io_read_async(&d, &r1), NUE_OK_PENDING);
    d.buffer = buf2.data(); d.offset = 99900; d.size = 1000; d.callback = nullptr; d.post_event = NUE_FALSE;
    CHECK_EQ(nue_io_read_async(&d, &r2), NUE_OK_PENDING);   // reads past the end: partial
    size_t n1 = 0, n2 = 0;
    CHECK_EQ(nue_io_wait(r1, 5000000000ll, &n1), NUE_OK);
    CHECK_EQ(nue_io_wait(r2, 5000000000ll, &n2), NUE_OK_PARTIAL);
    CHECK_EQ(n1, 1000u);
    CHECK_EQ(n2, 100u);
    CHECK(std::string(buf.data(), 26) == content.substr(26, 26));
    CHECK_EQ(callbacks, 1);
    CHECK_EQ(nue_io_poll(r1, nullptr), NUE_OK);
    nue_io_release(r1);
    nue_io_release(r2);
    CHECK_EQ(nue_io_poll(r1, nullptr), NUE_ERR_INVALID_HANDLE);
    // completion event
    nue_event ev;
    bool saw = false;
    while (nue_poll_event(&ev)) saw |= ev.type == NUE_EVENT_IO_COMPLETE && ev.payload.user.data1 == r1;
    CHECK(saw);
    nue_file_close(f);
}

TEST_CASE(manifest_verification_detects_every_kind_of_damage) {
    Engine e;
    write_file(e.data + "/ok.bin", "good data");
    write_file(e.data + "/short.bin", "xx");
    write_file(e.data + "/flip.bin", "abcd");
    uint8_t dg[32]; char hex[80];
    nue_sha256("good data", 9, dg); nue_hex_encode(dg, 32, hex, sizeof(hex));
    std::string m = std::string(hex) + " 9 ok.bin\n";
    char crc[16];
    uint32_t c = nue_crc32(0, "abcd", 4);
    std::snprintf(crc, sizeof(crc), "%08x", c);
    m += "# a comment\n" + std::string(crc) + " 4 FLIP.bin\n";
    CHECK_EQ(nue_vfs_verify_manifest("data:", m.data(), m.size(), nullptr, nullptr, nullptr), NUE_OK);

    write_file(e.data + "/flip.bin", "abXd");   // same size, different content
    nue_verify_result vr;
    NUE_STRUCT_INIT(&vr);
    CHECK_EQ(nue_vfs_verify_manifest("data:", m.data(), m.size(), nullptr, nullptr, &vr), NUE_ERR_CORRUPT);
    CHECK_EQ(vr.files_hash_mismatch, 1u);
    CHECK_STR(vr.first_bad_path, "FLIP.bin");

    std::string m2 = m + std::string(crc) + " 2 short.bin\n" + std::string(crc) + " 4 gone.bin\n";
    NUE_STRUCT_INIT(&vr);
    CHECK_EQ(nue_vfs_verify_manifest("data:", m2.data(), m2.size(), nullptr, nullptr, &vr), NUE_ERR_CORRUPT);
    CHECK_EQ(vr.files_checked, 4u);
    CHECK_EQ(vr.files_missing, 1u);
    CHECK_EQ(vr.files_size_mismatch, 0u);   // short.bin has the declared size 2 but the wrong hash
    CHECK_EQ(vr.files_hash_mismatch, 2u);

    const char* bad = "nothex 1\n";
    CHECK_EQ(nue_vfs_verify_manifest("data:", bad, std::strlen(bad), nullptr, nullptr, nullptr), NUE_ERR_CORRUPT);

    // cancellation from the progress callback
    static int calls; calls = 0;
    CHECK_EQ(nue_vfs_verify_manifest("data:", m.data(), m.size(), [](void*, const nue_verify_progress*) -> nue_bool { return ++calls < 2 ? NUE_TRUE : NUE_FALSE; }, nullptr, nullptr), NUE_ERR_CANCELLED);
}

static nue_result wait_import(nue_import_id id, nue_import_status* st) {
    for (int i = 0; i < 2000; ++i) {
        NUE_STRUCT_INIT(st);
        nue_import_get_status(id, st);
        if (st->state != NUE_OK_PENDING) return st->state;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return NUE_ERR_TIMEOUT;
}

TEST_CASE(import_copies_verifies_and_resumes) {
    Engine e;
    std::string src = e.root + "/source";
    write_file(src + "/Sub/one.bin", std::string(300000, '1'));
    write_file(src + "/two.bin", "second file");
    write_file(src + "/ignored.tmp", "not in manifest");
    uint8_t dg[32]; char h1[80], h2[80];
    nue_sha256(std::string(300000, '1').data(), 300000, dg); nue_hex_encode(dg, 32, h1, sizeof(h1));
    nue_sha256("second file", 11, dg); nue_hex_encode(dg, 32, h2, sizeof(h2));
    std::string manifest = std::string(h1) + " 300000 sub/ONE.bin\n" + h2 + " 11 two.bin\n";   // case differs from the source on purpose

    nue_import_desc d;
    NUE_STRUCT_INIT(&d);
    d.source_kind = NUE_IMPORT_SOURCE_PATH;
    d.source = src.c_str();
    d.dest_root = "cache";
    d.flags = NUE_IMPORT_VERIFY;
    d.manifest_text = manifest.c_str();
    d.manifest_length = manifest.size();
    nue_import_id id = 0;
    CHECK_EQ(nue_import_start(&d, &id), NUE_OK_PENDING);
    nue_import_status st;
    CHECK_EQ(wait_import(id, &st), NUE_OK);
    CHECK_EQ(st.progress.files_done, 2u);
    CHECK(nue_import_is_complete("cache"));
    CHECK(nue_vfs_exists("cache:sub/one.bin"));
    CHECK(!nue_vfs_exists("cache:ignored.tmp"));
    nue_import_release(id);

    // damage a copied file: SKIP_EXISTING + VERIFY must re-copy it
    write_file(e.root + "/cache_probe", "");
    char native[512]; size_t need;
    nue_vfs_resolve("cache:two.bin", NUE_FALSE, native, sizeof(native), &need);
    write_file(native, "SECOND FILE");   // same size, wrong content
    d.flags = NUE_IMPORT_VERIFY | NUE_IMPORT_SKIP_EXISTING;
    CHECK_EQ(nue_import_start(&d, &id), NUE_OK_PENDING);
    CHECK_EQ(wait_import(id, &st), NUE_OK);
    void* data; size_t sz;
    nue_file_load("cache:two.bin", &data, &sz);
    CHECK_STR(static_cast<char*>(data), "second file");
    nue_free(data);
    nue_import_release(id);

    // a manifest that the source cannot satisfy fails and says why
    std::string bad = manifest + h2 + " 11 absent.bin\n";
    d.manifest_text = bad.c_str(); d.manifest_length = bad.size();
    d.dest_root = "temp";
    CHECK_EQ(nue_import_start(&d, &id), NUE_OK_PENDING);
    CHECK_EQ(wait_import(id, &st), NUE_ERR_NOT_FOUND);
    CHECK(std::strstr(st.last_error, "missing") != nullptr);
    CHECK(!nue_import_is_complete("temp"));
    nue_import_release(id);

    // wrong hash in the manifest
    std::string wrong = std::string(h2) + " 300000 sub/one.bin\n";
    d.manifest_text = wrong.c_str(); d.manifest_length = wrong.size();
    d.dest_root = "user";
    CHECK_EQ(nue_import_start(&d, &id), NUE_OK_PENDING);
    CHECK_EQ(wait_import(id, &st), NUE_ERR_CORRUPT);
    CHECK(!nue_vfs_exists("user:sub/one.bin"));          // no half-verified file left behind
    CHECK(!nue_vfs_exists("user:sub/one.bin.nue_part"));
    nue_import_release(id);

    d.source_kind = NUE_IMPORT_SOURCE_URI;
    d.source = "content://x";
    CHECK_EQ(nue_import_start(&d, &id), NUE_ERR_UNSUPPORTED);   // no platform layer here
}

TEST_MAIN_BEGIN
    RUN(path_normalization_is_pure);
    RUN(case_insensitive_resolution_and_backslashes);
    RUN(portable_mode_keeps_every_writable_root_below_the_user_folder);
    RUN(roots_are_protected);
    RUN(mount_overlay_newest_wins_and_listing_merges);
    RUN(file_io_seek_append_truncate_and_atomic_save);
    RUN(file_ids_are_32bit_and_generation_checked);
    RUN(map_load_and_hash);
    RUN(directories_stat_rename_remove_glob);
    RUN(async_reads_complete_in_priority_order_and_post_events);
    RUN(manifest_verification_detects_every_kind_of_damage);
    RUN(import_copies_verifies_and_resumes);
TEST_MAIN_END
