#include <algorithm>
#include <cstring>

#include "nue_win/storage.hpp"
#include "test_framework.hpp"

using namespace nue::win;

static std::wstring make_test_root() {
    std::wstring temp;
    known_folder_path(KnownFolder::Temp, temp);
    std::wstring root = temp + L"\\nue_win_storage_test_" + std::to_wstring(GetCurrentProcessId());
    remove_tree(root);
    create_directories(root);
    return root;
}

static void test_known_folders() {
    TEST_BEGIN("known folders");
    for (KnownFolder f : {KnownFolder::SavedGames, KnownFolder::LocalAppData, KnownFolder::RoamingAppData, KnownFolder::Documents, KnownFolder::ProgramData, KnownFolder::Desktop, KnownFolder::Downloads, KnownFolder::Temp}) {
        std::wstring p;
        Status s = known_folder_path(f, p);
        CHECK_OK(s);
        if (s.ok) {
            INFO("%d -> %s%s", int(f), path_to_utf8(p).c_str(), path_is_under_onedrive(p) ? "  [under OneDrive]" : "");
            CHECK(!p.empty() && p.back() != L'\\');
            CHECK(GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES);
        }
    }
}

static void test_app_dirs() {
    TEST_BEGIN("application directories");
    AppDirs d;
    AppDirOptions o;
    o.create = false;
    CHECK_OK(resolve_app_dirs("NUE Vendor", "Test: Game?", o, d));
    INFO("saves  %s", path_to_utf8(d.saves).c_str());
    INFO("config %s", path_to_utf8(d.config).c_str());
    INFO("logs   %s", path_to_utf8(d.logs).c_str());
    CHECK(d.saves.find(L"Saved Games") != std::wstring::npos || d.saves.find(L"My Games") != std::wstring::npos);
    CHECK(d.saves.find(L"Test_ Game_") != std::wstring::npos);   // forbidden characters replaced
    CHECK(d.config.find(L"AppData") != std::wstring::npos);
    CHECK(!d.install.empty());
    CHECK(GetFileAttributesW(d.saves.c_str()) == INVALID_FILE_ATTRIBUTES);   // create = false creates nothing

    const std::wstring root = make_test_root();
    AppDirOptions o2;
    o2.root_override = root + L"\\portable";
    CHECK_OK(resolve_app_dirs("v", "a", o2, d));
    for (const std::wstring* p : {&d.saves, &d.config, &d.cache, &d.logs, &d.crash_dumps}) CHECK((GetFileAttributesW(p->c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0);
    remove_tree(root);
}

static void test_names() {
    TEST_BEGIN("file names");
    CHECK(is_reserved_file_name("CON") && is_reserved_file_name("nul.txt") && is_reserved_file_name("COM1") && is_reserved_file_name("lpt9.log"));
    CHECK(!is_reserved_file_name("console") && !is_reserved_file_name("COM0") && !is_reserved_file_name("com10"));
    CHECK(sanitize_file_name("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j");
    CHECK(sanitize_file_name("name. ") == "name");
    CHECK(sanitize_file_name("CON") == "_CON");
    CHECK(sanitize_file_name("") == "_");
    CHECK(path_from_utf8("saves/slot1/\xc3\xa9.sav") == L"saves\\slot1\\\u00e9.sav");
    CHECK(path_to_utf8(L"\u00e9") == "\xc3\xa9");
}

static void test_extended_paths() {
    TEST_BEGIN("extended and long paths");
    CHECK(to_extended_path(L"C:\\a\\b").rfind(L"\\\\?\\C:\\a\\b", 0) == 0);
    CHECK(to_extended_path(L"C:/a/./b/../c") == L"\\\\?\\C:\\a\\c");
    CHECK(to_extended_path(L"\\\\server\\share\\x") == L"\\\\?\\UNC\\server\\share\\x");
    CHECK(to_extended_path(L"\\\\?\\C:\\already") == L"\\\\?\\C:\\already");
    CHECK(strip_extended_prefix(L"\\\\?\\C:\\a") == L"C:\\a" && strip_extended_prefix(L"\\\\?\\UNC\\s\\x") == L"\\\\s\\x" && strip_extended_prefix(L"C:\\a") == L"C:\\a");
    CHECK(to_extended_path(L"relative.txt").rfind(L"\\\\?\\", 0) == 0);
    INFO("long paths enabled for this process (manifest + registry): %d", int(long_paths_enabled()));

    const std::wstring root = make_test_root();
    std::wstring deep = root;
    while (deep.size() < 400) deep += L"\\segment_with_a_long_name_0123456789";   // > MAX_PATH
    CHECK_OK(create_directories(deep));
    const std::wstring file = deep + L"\\data.bin";
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_OK(write_file_atomic(file, payload, sizeof payload));
    std::vector<uint8_t> back;
    CHECK_OK(read_file(file, back));
    CHECK(back.size() == 8 && std::memcmp(back.data(), payload, 8) == 0);
    INFO("created and read a file at path length %zu", file.size());
    CHECK(file.size() > MAX_PATH);
    remove_tree(root);
    CHECK(GetFileAttributesW(to_extended_path(root).c_str()) == INVALID_FILE_ATTRIBUTES);   // the whole deep tree is gone
}

static void test_atomic_write() {
    TEST_BEGIN("atomic write");
    const std::wstring root = make_test_root();
    const std::wstring f = root + L"\\save.dat";
    const char a[] = "first version of the save";
    const char b[] = "second, longer version of the save game data";
    CHECK_OK(write_file_atomic(f, a, sizeof a));
    CHECK_OK(write_file_atomic(f, b, sizeof b));   // replaces
    std::vector<uint8_t> back;
    CHECK_OK(read_file(f, back));
    CHECK(back.size() == sizeof b && std::memcmp(back.data(), b, sizeof b) == 0);
    int leftovers = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (wcsstr(fd.cFileName, L"nue-tmp")) ++leftovers; } while (FindNextFileW(h, &fd)); FindClose(h); }
    CHECK(leftovers == 0);
    // a file held open for reading by another handle without FILE_SHARE_DELETE makes the replace fail cleanly and keeps the old content
    HANDLE hold = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Status s = write_file_atomic(f, a, sizeof a);
    CloseHandle(hold);
    CHECK(!s.ok);
    CHECK_OK(read_file(f, back));
    CHECK(back.size() == sizeof b);
    leftovers = 0;
    h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (wcsstr(fd.cFileName, L"nue-tmp")) ++leftovers; } while (FindNextFileW(h, &fd)); FindClose(h); }
    CHECK(leftovers == 0);   // the failed attempt cleaned its temp file
    CHECK(!read_file(root + L"\\missing.dat", back).ok);
    remove_tree(root);
}

static void test_case_semantics() {
    TEST_BEGIN("case-insensitive semantics");
    const std::wstring root = make_test_root();
    CHECK(paths_equal_ignore_case(L"C:\\Game\\Data\\File.BIN", L"c:/game/data/file.bin"));
    CHECK(!paths_equal_ignore_case(L"C:\\Game\\a", L"C:\\Game\\b"));
    CHECK(paths_equal_ignore_case(L"\u00c9cole", L"\u00e9cole"));   // non-ASCII folds too

    const CaseMode mode = directory_case_mode(root);
    INFO("temp directory case mode: %s", mode == CaseMode::Insensitive ? "insensitive (default NTFS)" : (mode == CaseMode::Sensitive ? "SENSITIVE" : "unknown"));
    CHECK(mode == CaseMode::Insensitive || mode == CaseMode::Unknown);

    create_directories(root + L"\\Data\\Cars");
    const uint8_t x = 1;
    write_file_atomic(root + L"\\Data\\Cars\\Lancer.BIN", &x, 1);
    std::vector<uint8_t> back;
    CHECK_OK(read_file(root + L"\\data\\CARS\\lancer.bin", back));   // the file system itself matches without regard to case
    std::wstring real;
    CHECK(resolve_case_insensitive(root, L"data/cars/LANCER.bin", real));
    CHECK(real == root + L"\\Data\\Cars\\Lancer.BIN");   // the real on-disk spelling comes back
    CHECK(!resolve_case_insensitive(root, L"data/cars/missing.bin", real));
    remove_tree(root);
}

static void test_disk() {
    TEST_BEGIN("disk space and placeholders");
    std::wstring temp;
    known_folder_path(KnownFolder::Temp, temp);
    DiskSpace d;
    CHECK_OK(disk_space(temp, d));
    CHECK(d.total > 0 && d.free_for_user <= d.total);
    INFO("temp volume: %.1f GiB free of %.1f GiB", d.free_for_user / 1073741824.0, d.total / 1073741824.0);
    CHECK(!file_is_cloud_placeholder(temp + L"\\definitely_missing_file.xyz"));
}

int main() {
    test_known_folders();
    test_app_dirs();
    test_names();
    test_extended_paths();
    test_atomic_write();
    test_case_semantics();
    test_disk();
    return finish("storage_tests");
}
