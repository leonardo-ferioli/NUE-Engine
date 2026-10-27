// Files: the library on an in-memory storage (unit checks), and the same scenarios in a scratch folder through the library on the standard storage
// and through the real kernel32, with return values, errors, directory listings and attributes compared.
#include "nue_legacy/win32/files.hpp"
#include "nue_legacy/platform/null_services.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace nue::legacy::win32;
namespace stdlib = nue::legacy::platform::stdlib;
namespace null = nue::legacy::platform::null;
namespace fs = std::filesystem;

static void test_files() {
    null::MemoryStorage storage;
    storage.mount({"data", "", false, false, false});
    storage.put_file("data:GLOBAL/GLOBALB.LUT", "lookup table");
    storage.put_file("data:readme.txt", "hello");
    HandleTable handles;
    FileSystemConfig config;
    config.current_directory = "Z:\\";
    config.drives['Z'] = "data:";
    FileSystem fsys(storage, handles, config);
    // Case-insensitive, backslash paths through the drive mapping.
    auto h = fsys.create_file("z:\\global\\globalb.lut", access::generic_read, share::read, disposition::open_existing, 0);
    CHECK(h && h.value != kInvalidHandle);
    char buffer[64] = {};
    auto r = fsys.read_file(h.value, MutableBytes(reinterpret_cast<std::byte*>(buffer), sizeof buffer));
    CHECK(r && r.value == 12 && std::string(buffer, 12) == "lookup table");
    CHECK_EQ(fsys.read_file(h.value, MutableBytes(reinterpret_cast<std::byte*>(buffer), sizeof buffer)).value, 0u);   // end of file
    CHECK_EQ(fsys.get_file_size(h.value).value, 12ull);
    CHECK_EQ(fsys.get_file_type(h.value), uint32_t(file_type::disk));
    CHECK(fsys.set_file_pointer(h.value, -5, seek_method::end).value == 7);
    CHECK_EQ(fsys.set_file_pointer(h.value, -100, seek_method::begin).error, 131u);
    CHECK_EQ(fsys.write_file(h.value, Bytes(reinterpret_cast<const std::byte*>("x"), 1)).error, uint32_t(err::access_denied));
    CHECK(fsys.read_file(h.value, MutableBytes(reinterpret_cast<std::byte*>(buffer), 5)).value == 5 && std::string(buffer, 5) == "table");
    CHECK(handles.close(h.value));
    CHECK_EQ(fsys.read_file(h.value, MutableBytes(reinterpret_cast<std::byte*>(buffer), 5)).error, uint32_t(err::invalid_handle));
    // Relative names resolve against the virtual current directory, "..\" and "." are lexical.
    auto relative = fsys.create_file("GLOBAL\\..\\readme.txt", access::generic_read, share::read, disposition::open_existing, 0);
    CHECK(relative && relative.value != kInvalidHandle);
    CHECK(fsys.get_full_path_name("a\\.\\b\\..\\c").value == "Z:\\a\\c");
    CHECK(fsys.get_full_path_name("\\x").value == "Z:\\x");
    CHECK_EQ(fsys.create_file("bad?name", access::generic_read, 0, disposition::open_existing, 0).error, uint32_t(err::invalid_name));
    CHECK_EQ(fsys.create_file("nodir\\x.txt", access::generic_read, 0, disposition::open_existing, 0).error, uint32_t(err::path_not_found));
    CHECK_EQ(fsys.create_file("nofile.txt", access::generic_read, 0, disposition::open_existing, 0).error, uint32_t(err::file_not_found));
    // Writing, listing.
    auto out = fsys.create_file("saves\\profile.dat", access::generic_write, 0, disposition::create_always, 0);
    CHECK_EQ(out.error, uint32_t(err::path_not_found));                    // "saves" does not exist
    CHECK(fsys.create_directory("saves").value);
    out = fsys.create_file("saves\\profile.dat", access::generic_write, 0, disposition::create_always, 0);
    CHECK(out && out.value != kInvalidHandle);
    CHECK_EQ(fsys.write_file(out.value, Bytes(reinterpret_cast<const std::byte*>("data"), 4)).value, 4u);
    handles.close(out.value);
    FindData fd;
    auto find = fsys.find_first_file("saves\\*.dat", fd);
    CHECK(find && fd.name == "profile.dat" && fd.size == 4 && (fd.attributes & attribute::archive));
    CHECK_EQ(fsys.find_next_file(find.value, fd).error, uint32_t(err::no_more_files));
    CHECK(fsys.find_close(find.value));
    CHECK(wildcard_match("*.txt", "README.TXT") && !wildcard_match("*.txt", "a.dat") && wildcard_match("?.bin", "a.BIN") && wildcard_match("*.*", "noextension"));
}
REGISTER_TEST("files", test_files);

#if defined(_WIN32)
namespace {
// One scenario, two worlds: `W` (kernel32 in the current directory of the scratch folder) and `M` (the library, drive Z: mapped to the scratch folder).
struct World {
    virtual ~World() = default;
    virtual Handle open(const std::string& path, uint32_t access, uint32_t share, uint32_t disposition, uint32_t& error) = 0;
    virtual uint32_t read(Handle, size_t n, std::string& out, uint32_t& error) = 0;
    virtual uint32_t write(Handle, const std::string&, uint32_t& error) = 0;
    virtual int64_t seek(Handle, int64_t distance, uint32_t method, uint32_t& error) = 0;
    virtual int64_t size(Handle, uint32_t& error) = 0;
    virtual bool set_eof(Handle, uint32_t& error) = 0;
    virtual bool close(Handle) = 0;
    virtual bool make_dir(const std::string&, uint32_t& error) = 0;
    virtual bool remove_dir(const std::string&, uint32_t& error) = 0;
    virtual bool delete_file(const std::string&, uint32_t& error) = 0;
    virtual bool move(const std::string&, const std::string&, uint32_t& error) = 0;
    virtual uint32_t attributes(const std::string&, uint32_t& error) = 0;
    virtual std::vector<std::string> find(const std::string& pattern, uint32_t& error) = 0;   // "name:attributes:size" per entry
    virtual std::string full_path(const std::string&, uint32_t& error) = 0;
    virtual uint32_t file_type(Handle) = 0;
};

std::string join(const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += x + ","; return s; }

struct WindowsWorld : World {
    std::string cwd;
    std::vector<HANDLE> table{nullptr};
    Handle put(HANDLE h) { if (h == INVALID_HANDLE_VALUE) return 0; table.push_back(h); return Handle(table.size() - 1) * 4; }
    HANDLE get(Handle h) { return h / 4 < table.size() && h != 0 ? table[h / 4] : INVALID_HANDLE_VALUE; }
    Handle open(const std::string& p, uint32_t a, uint32_t s, uint32_t d, uint32_t& e) override { SetLastError(0); HANDLE h = CreateFileA(p.c_str(), a, s, nullptr, d, FILE_ATTRIBUTE_NORMAL, nullptr); e = GetLastError(); return put(h); }
    uint32_t read(Handle h, size_t n, std::string& out, uint32_t& e) override { out.assign(n, '\0'); DWORD got = 0; SetLastError(0); const BOOL ok = ReadFile(get(h), out.data(), DWORD(n), &got, nullptr); e = ok ? 0 : GetLastError(); out.resize(got); return got; }
    uint32_t write(Handle h, const std::string& d, uint32_t& e) override { DWORD got = 0; SetLastError(0); const BOOL ok = WriteFile(get(h), d.data(), DWORD(d.size()), &got, nullptr); e = ok ? 0 : GetLastError(); return got; }
    int64_t seek(Handle h, int64_t d, uint32_t m, uint32_t& e) override { LARGE_INTEGER in, out; in.QuadPart = d; SetLastError(0); const BOOL ok = SetFilePointerEx(get(h), in, &out, m); e = ok ? 0 : GetLastError(); return ok ? out.QuadPart : -1; }
    int64_t size(Handle h, uint32_t& e) override { LARGE_INTEGER s; SetLastError(0); const BOOL ok = GetFileSizeEx(get(h), &s); e = ok ? 0 : GetLastError(); return ok ? s.QuadPart : -1; }
    bool set_eof(Handle h, uint32_t& e) override { SetLastError(0); const BOOL ok = SetEndOfFile(get(h)); e = ok ? 0 : GetLastError(); return ok; }
    bool close(Handle h) override { if (!h) return false; const BOOL ok = CloseHandle(get(h)); table[h / 4] = INVALID_HANDLE_VALUE; return ok; }
    bool make_dir(const std::string& p, uint32_t& e) override { SetLastError(0); const BOOL ok = CreateDirectoryA(p.c_str(), nullptr); e = ok ? 0 : GetLastError(); return ok; }
    bool remove_dir(const std::string& p, uint32_t& e) override { SetLastError(0); const BOOL ok = RemoveDirectoryA(p.c_str()); e = ok ? 0 : GetLastError(); return ok; }
    bool delete_file(const std::string& p, uint32_t& e) override { SetLastError(0); const BOOL ok = DeleteFileA(p.c_str()); e = ok ? 0 : GetLastError(); return ok; }
    bool move(const std::string& a, const std::string& b, uint32_t& e) override { SetLastError(0); const BOOL ok = MoveFileA(a.c_str(), b.c_str()); e = ok ? 0 : GetLastError(); return ok; }
    uint32_t attributes(const std::string& p, uint32_t& e) override { SetLastError(0); const DWORD a = GetFileAttributesA(p.c_str()); e = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0; return a; }
    std::vector<std::string> find(const std::string& pattern, uint32_t& e) override {
        std::vector<std::string> out;
        WIN32_FIND_DATAA d;
        SetLastError(0);
        HANDLE h = FindFirstFileA(pattern.c_str(), &d);
        if (h == INVALID_HANDLE_VALUE) { e = GetLastError(); return out; }
        e = 0;
        do { out.push_back(std::string(d.cFileName) + ":" + std::to_string(d.dwFileAttributes) + ":" + std::to_string((uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow)); } while (FindNextFileA(h, &d));
        out.push_back("end:" + std::to_string(GetLastError()));
        FindClose(h);
        return out;
    }
    std::string full_path(const std::string& p, uint32_t& e) override {
        char buffer[1024]; SetLastError(0);
        const DWORD n = GetFullPathNameA(p.c_str(), sizeof buffer, buffer, nullptr);
        e = n ? 0 : GetLastError();
        std::string s(buffer, n);
        if (s.rfind(cwd, 0) == 0) s = "<root>" + s.substr(cwd.size());
        return s;
    }
    uint32_t file_type(Handle h) override { return GetFileType(get(h)); }
    ~WindowsWorld() override { for (size_t i = 1; i < table.size(); ++i) if (table[i] != INVALID_HANDLE_VALUE) CloseHandle(table[i]); }
};

struct LibraryWorld : World {
    LibraryWorld(const std::string& dir) : storage(stdlib::make_storage()) {
        storage->mount({"data", dir, false, true, false});
        FileSystemConfig config;
        config.current_directory = "Z:\\work";
        config.drives['Z'] = "data:";
        fsys = std::make_unique<FileSystem>(*storage, handles, config);
    }
    std::unique_ptr<nue::legacy::platform::Storage> storage;
    HandleTable handles;
    std::unique_ptr<FileSystem> fsys;
    std::vector<Handle> table{0};
    Handle put(Handle h) { if (h == kInvalidHandle) return 0; table.push_back(h); return Handle(table.size() - 1) * 4; }
    Handle get(Handle h) { return h / 4 < table.size() && h != 0 ? table[h / 4] : kInvalidHandle; }
    Handle open(const std::string& p, uint32_t a, uint32_t s, uint32_t d, uint32_t& e) override { auto r = fsys->create_file(p, a, s, d, 0); e = r.error; return put(r.value); }
    uint32_t read(Handle h, size_t n, std::string& out, uint32_t& e) override { out.assign(n, '\0'); auto r = fsys->read_file(get(h), MutableBytes(reinterpret_cast<std::byte*>(out.data()), n)); e = r.error; out.resize(r.value); return r.value; }
    uint32_t write(Handle h, const std::string& d, uint32_t& e) override { auto r = fsys->write_file(get(h), Bytes(reinterpret_cast<const std::byte*>(d.data()), d.size())); e = r.error; return r.value; }
    int64_t seek(Handle h, int64_t d, uint32_t m, uint32_t& e) override { auto r = fsys->set_file_pointer(get(h), d, m); e = r.error; return r.error ? -1 : int64_t(r.value); }
    int64_t size(Handle h, uint32_t& e) override { auto r = fsys->get_file_size(get(h)); e = r.error; return r.error ? -1 : int64_t(r.value); }
    bool set_eof(Handle h, uint32_t& e) override { auto r = fsys->set_end_of_file(get(h)); e = r.error; return r.value; }
    bool close(Handle h) override { return h && handles.close(get(h)); }
    bool make_dir(const std::string& p, uint32_t& e) override { auto r = fsys->create_directory(p); e = r.error; return r.value; }
    bool remove_dir(const std::string& p, uint32_t& e) override { auto r = fsys->remove_directory(p); e = r.error; return r.value; }
    bool delete_file(const std::string& p, uint32_t& e) override { auto r = fsys->delete_file(p); e = r.error; return r.value; }
    bool move(const std::string& a, const std::string& b, uint32_t& e) override { auto r = fsys->move_file(a, b); e = r.error; return r.value; }
    uint32_t attributes(const std::string& p, uint32_t& e) override { const uint32_t a = fsys->get_file_attributes(p); e = fsys->last_error; return a; }
    std::vector<std::string> find(const std::string& pattern, uint32_t& e) override {
        std::vector<std::string> out;
        FindData d;
        auto h = fsys->find_first_file(pattern, d);
        if (h.error) { e = h.error; return out; }
        e = 0;
        out.push_back(d.name + ":" + std::to_string(d.attributes) + ":" + std::to_string(d.size));
        Ret<bool> next;
        while ((next = fsys->find_next_file(h.value, d)).value) out.push_back(d.name + ":" + std::to_string(d.attributes) + ":" + std::to_string(d.size));
        out.push_back("end:" + std::to_string(next.error));
        fsys->find_close(h.value);
        return out;
    }
    std::string full_path(const std::string& p, uint32_t& e) override {
        auto r = fsys->get_full_path_name(p);
        e = r.error;
        std::string s = r.value;
        if (s.rfind("Z:", 0) == 0) s = "<root>" + s.substr(2);                    // Z:\ plays the part of the scratch folder
        return s;
    }
    uint32_t file_type(Handle h) override { return fsys->get_file_type(get(h)); }
};

constexpr uint32_t GR = access::generic_read, GW = access::generic_write;
std::string num(uint64_t v) { return std::to_string(v); }

std::vector<std::string> scenarios(World& w) {
    std::vector<std::string> log;
    auto L = [&](const std::string& what, const std::string& value) { log.push_back(what + " = " + value); };
    uint32_t e = 0;
    auto R = [&](const std::string& what, uint64_t value, uint32_t error) { L(what, num(value) + " err " + num(error)); };
    // One observation: the value of `expr` (which sets `e`) and the error, read in that order (operands of a call have no defined order).
#define OBS(what, expr) do { e = 0; const uint64_t observed_ = uint64_t(expr); L(what, num(observed_) + " err " + num(e)); } while (0)
    auto open = [&](const std::string& path, uint32_t access, uint32_t share, uint32_t disp) { uint32_t err = 0; Handle h = w.open(path, access, share, disp, err); return std::make_pair(h, err); };
    // 1. The disposition matrix on a missing and on an existing file.
    for (uint32_t disp = 1; disp <= 5; ++disp) {
        auto first = open("d" + num(disp) + ".txt", GR | GW, 0, disp);
        R("disp " + num(disp) + " on missing", first.first != 0, first.second);
        if (first.first) { uint32_t e2; w.write(first.first, "abc", e2); w.close(first.first); }
        auto second = open("d" + num(disp) + ".txt", GR | GW, 0, disp);
        R("disp " + num(disp) + " on existing", second.first != 0, second.second);
        if (second.first) { OBS("  size after", uint64_t(w.size(second.first, e))); w.close(second.first); }
    }
    // 2. Missing directories and bad names.
    auto missing_dir = open("nodir\\f.txt", GW, 0, disposition::create_always);
    R("create in a missing directory", missing_dir.first != 0, missing_dir.second);
    auto bad = open("bad?.txt", GR, 0, disposition::open_existing);
    R("open a name with '?'", bad.first != 0, bad.second);
    auto empty = open("", GR, 0, disposition::open_existing);
    R("open an empty name", empty.first != 0, empty.second);
    auto dots = open("trailing.dots...", GW, 0, disposition::create_always);
    R("create 'trailing.dots...'", dots.first != 0, dots.second);
    if (dots.first) w.close(dots.first);
    auto same = open("trailing.dots", GR, 0, disposition::open_existing);
    R("open it as 'trailing.dots'", same.first != 0, same.second);
    if (same.first) w.close(same.first);
    auto cased = open("CaseTest.TXT", GW, 0, disposition::create_always);
    if (cased.first) w.close(cased.first);
    auto lower = open("casetest.txt", GR, 0, disposition::open_existing);
    R("open with other case", lower.first != 0, lower.second);
    if (lower.first) w.close(lower.first);
    // 3. Read, write, seek, size, truncate.
    auto f = open("data.bin", GR | GW, 0, disposition::create_always);
    std::string text;
    OBS("write", w.write(f.first, "hello world", e));
    OBS("size", uint64_t(w.size(f.first, e)));
    OBS("seek begin 0", uint64_t(w.seek(f.first, 0, 0, e)));
    OBS("read 5", w.read(f.first, 5, text, e));
    L("  text", text);
    OBS("seek current +1", uint64_t(w.seek(f.first, 1, 1, e)));
    OBS("read 100", w.read(f.first, 100, text, e));
    L("  text", text);
    OBS("read at the end", w.read(f.first, 10, text, e));
    OBS("seek end -5", uint64_t(w.seek(f.first, -5, 2, e)));
    OBS("seek before the start", uint64_t(w.seek(f.first, -50, 0, e) + 1));
    OBS("seek past the end", uint64_t(w.seek(f.first, 100, 0, e)));
    OBS("write past the end", w.write(f.first, "!", e));
    OBS("size after sparse write", uint64_t(w.size(f.first, e)));
    OBS("seek 3", uint64_t(w.seek(f.first, 3, 0, e)));
    OBS("set end of file", w.set_eof(f.first, e));
    OBS("size after truncation", uint64_t(w.size(f.first, e)));
    OBS("file type", w.file_type(f.first));
    w.close(f.first);
    // 4. Access rights.
    auto wo = open("data.bin", GW, share::read, disposition::open_existing);
    OBS("read on a write-only handle", w.read(wo.first, 1, text, e));
    w.close(wo.first);
    auto ro = open("data.bin", GR, share::read, disposition::open_existing);
    OBS("write on a read-only handle", w.write(ro.first, "x", e));
    OBS("set end of file on a read-only handle", w.set_eof(ro.first, e));
    w.close(ro.first);
    OBS("read on a closed handle", w.read(ro.first, 1, text, e));
    // 5. Share modes.
    auto s1 = open("share.txt", GR | GW, 0, disposition::create_always);
    auto s2 = open("share.txt", GR, share::read | share::write, disposition::open_existing);
    R("second open of an exclusive file", s2.first != 0, s2.second);
    w.close(s1.first);
    auto s3 = open("share.txt", GR, share::read, disposition::open_existing);
    auto s4 = open("share.txt", GR, share::read, disposition::open_existing);
    R("two readers sharing read", s4.first != 0, s4.second);
    auto s5 = open("share.txt", GW, share::read | share::write, disposition::open_existing);
    R("writer while readers forbid writing", s5.first != 0, s5.second);
    w.close(s3.first); w.close(s4.first);
    auto s6 = open("share.txt", GR, share::read | share::write, disposition::open_existing);
    auto s7 = open("share.txt", GW, share::read | share::write, disposition::open_existing);
    R("reader and writer sharing everything", s7.first != 0, s7.second);
    w.close(s6.first); w.close(s7.first);
    // 6. Directories.
    OBS("create directory", w.make_dir("dir1", e));
    OBS("create it again", w.make_dir("dir1", e));
    OBS("create under a missing parent", w.make_dir("nodir\\sub", e));
    OBS("create a directory over a file", w.make_dir("data.bin", e));
    auto in_dir = open("dir1\\inner.txt", GW, 0, disposition::create_always);
    if (in_dir.first) w.close(in_dir.first);
    OBS("remove a non-empty directory", w.remove_dir("dir1", e));
    OBS("remove a file as a directory", w.remove_dir("data.bin", e));
    OBS("remove a missing directory", w.remove_dir("nope", e));
    R("open a directory as a file", open("dir1", GR, share::read, disposition::open_existing).second != 0, open("dir1", GR, share::read, disposition::open_existing).second);
    // 7. Delete and move.
    OBS("delete a missing file", w.delete_file("nope.txt", e));
    OBS("delete in a missing directory", w.delete_file("nodir\\nope.txt", e));
    OBS("delete a directory", w.delete_file("dir1", e));
    auto held = open("held.txt", GR | GW, share::read, disposition::create_always);
    OBS("delete an open file", w.delete_file("held.txt", e));
    w.close(held.first);
    OBS("delete it once closed", w.delete_file("held.txt", e));
    OBS("move onto an existing file", w.move("d1.txt", "d2.txt", e));
    OBS("move a missing file", w.move("nope.txt", "other.txt", e));
    OBS("move a file", w.move("d1.txt", "moved.txt", e));
    OBS("the source is gone", w.attributes("d1.txt", e));
    OBS("the target is there", w.attributes("moved.txt", e));
    // 8. Attributes.
    OBS("attributes of a file", w.attributes("data.bin", e));
    OBS("attributes of a directory", w.attributes("dir1", e));
    OBS("attributes of a missing file", w.attributes("nothing", e));
    OBS("attributes in a missing directory", w.attributes("nodir\\nothing", e));
    // 9. Listing: names (as Windows sorts them), attributes, sizes.
    OBS("create b.dat", open("b.dat", GW, 0, disposition::create_always).first != 0 ? 1 : 0);
    for (const char* name : {"a.txt", "B.TXT", "c.dat", "noext", "dir1\\z.txt"}) { auto h = open(name, GW, 0, disposition::create_always); if (h.first) { uint32_t e2; w.write(h.first, name, e2); w.close(h.first); } }
    for (const char* pattern : {"*", "*.*", "*.txt", "?.txt", "*.dat", "no*", "dir1\\*", "nodir\\*", "nomatch*", "dir1", "a.txt"}) {
        const auto list = w.find(pattern, e);
        L(std::string("find ") + pattern, join(list) + " err " + num(e));
    }
    // 10. Paths.
    for (const char* p : {"a", ".\\a", "..\\a", "sub\\..\\a", "sub\\.\\x\\", "dir1\\", "a.", "a ", "a/b"}) {   // (probes that climb above the root, "\a" and "\\?\a" differ by design: drive Z: is not C:, and the raw prefix is not interpreted)
        const std::string full = w.full_path(p, e);
        L(std::string("full path ") + p, full + " err " + num(e));
    }
    return log;
}
}  // namespace

static void test_files_against_windows() {
    const fs::path base = fs::temp_directory_path() / ("nue_legacy_files_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(base);
    fs::create_directories(base / "win" / "work");
    fs::create_directories(base / "mine" / "work");
    char saved[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, saved);
    SetCurrentDirectoryA((base / "win" / "work").string().c_str());
    WindowsWorld real;
    real.cwd = (base / "win").string();
    LibraryWorld mine((base / "mine").string());
    const auto expected = scenarios(real);
    const auto actual = scenarios(mine);
    SetCurrentDirectoryA(saved);
    CHECK_EQ(expected.size(), actual.size());
    int differences = 0;
    for (size_t i = 0; i < (std::min)(expected.size(), actual.size()); ++i) {
        if (expected[i] != actual[i]) { ++differences; std::fprintf(stderr, "windows: %s\nmine:    %s\n", expected[i].c_str(), actual[i].c_str()); }
    }
    std::printf("  %zu observations, %d differ\n", expected.size(), differences);
    CHECK_EQ(differences, 0);
    std::error_code ec;
    fs::remove_all(base, ec);
}
REGISTER_TEST("files_windows", test_files_against_windows);
#endif
