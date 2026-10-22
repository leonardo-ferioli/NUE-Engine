#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#include "crc32.hpp"
#include "import_engine.hpp"
#include "testing.hpp"

using namespace nue::android;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("nue_android_test_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str(const char* sub = "") const { return (path / sub).generic_string(); }
};

std::string pattern(size_t n, unsigned seed) {
    std::string s(n, '\0');
    unsigned x = seed * 2654435761u + 1;
    for (char& c : s) {
        x = x * 1664525u + 1013904223u;
        c = char(x >> 24);
    }
    return s;
}

void write_file(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(data.data(), std::streamsize(data.size()));
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Source tree used by most tests: 4 files, one larger than the 64 KiB test buffer, mixed case, Windows names.
struct Fixture {
    TempDir src, dst;
    std::string a = pattern(200 * 1024 + 17, 1);
    std::string b = pattern(1000, 2);
    std::string c = pattern(0, 3);
    std::string extra = pattern(5000, 4);
    Fixture() {
        write_file(src.path / "GLOBAL" / "GlobalA.lzc", a);
        write_file(src.path / "Cars" / "BMW M3" / "geo.bin", b);
        write_file(src.path / "empty.dat", c);
        write_file(src.path / "movies" / "intro.bik", extra); // not in the manifest
    }
    Manifest manifest(bool with_crc = true) const {
        Manifest m;
        auto add = [&](const char* p, const std::string& d) {
            ManifestEntry e;
            e.path = p;
            e.size = d.size();
            e.has_crc = with_crc;
            e.crc = crc32(d.data(), d.size());
            m.entries.push_back(e);
        };
        add("global/globala.lzc", a);
        add("cars/bmw m3/geo.bin", b);
        add("empty.dat", c);
        return m;
    }
    static ImportOptions options() {
        ImportOptions o;
        o.buffer_size = 64 * 1024;
        o.progress_step_bytes = 1;
        o.fsync_files = true;
        return o;
    }
};

// File system that fails writes after `budget` bytes (device full) and can fake the free space.
class LimitedFs final : public FileSystem {
public:
    explicit LimitedFs(FileSystem& inner) : in_(inner) {}
    int64_t write_budget = -1;     // < 0: unlimited
    int64_t fake_free = -1;        // < 0: real
    bool make_dirs(const std::string& p) override { return in_.make_dirs(p); }
    std::unique_ptr<WritableFile> create(const std::string& p) override {
        auto f = in_.create(p);
        if (!f) return nullptr;
        return std::make_unique<Wrap>(std::move(f), this);
    }
    bool rename(const std::string& a, const std::string& b) override { return in_.rename(a, b); }
    bool remove(const std::string& p) override { return in_.remove(p); }
    bool file_size(const std::string& p, uint64_t* s) override { return in_.file_size(p, s); }
    bool free_space(const std::string& p, uint64_t* b) override {
        if (fake_free >= 0) {
            *b = uint64_t(fake_free);
            return true;
        }
        return in_.free_space(p, b);
    }
    bool read_text(const std::string& p, std::string* o) override { return in_.read_text(p, o); }
    bool write_text_atomic(const std::string& p, const std::string& t) override { return in_.write_text_atomic(p, t); }
    bool append_text(const std::string& p, const std::string& t, bool s) override { return in_.append_text(p, t, s); }

private:
    struct Wrap final : WritableFile {
        Wrap(std::unique_ptr<WritableFile> f, LimitedFs* o) : f_(std::move(f)), owner_(o) {}
        WriteStatus write(const void* d, size_t n) override {
            if (owner_->write_budget >= 0) {
                if (int64_t(n) > owner_->write_budget) return WriteStatus::NoSpace;
                owner_->write_budget -= int64_t(n);
            }
            return f_->write(d, n);
        }
        bool close(bool sync) override { return f_->close(sync); }
        std::unique_ptr<WritableFile> f_;
        LimitedFs* owner_;
    };
    FileSystem& in_;
};

} // namespace

TEST(import, copies_manifest_files_and_skips_the_rest) {
    Fixture fx;
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    Manifest m = fx.manifest();
    ImportEngine eng(*fsys, fx.dst.str("gamedata"), Fixture::options());
    std::atomic<bool> cancel{false};
    uint64_t last_done = 0;
    int calls = 0;
    bool monotonic = true;
    ImportReport r = eng.run(*src, &m, [&](const ImportProgress& p) {
        ++calls;
        if (p.bytes_done < last_done) monotonic = false;
        last_done = p.bytes_done;
    }, cancel);
    CHECK(r.result == ImportResult::Ok);
    CHECK_EQ(r.files_total, 3u);
    CHECK_EQ(r.files_copied, 3u);
    CHECK_EQ(r.bytes_total, uint64_t(fx.a.size() + fx.b.size()));
    CHECK(calls > 4 && monotonic);
    CHECK_EQ(last_done, r.bytes_total);
    CHECK(read_file(fx.dst.path / "gamedata/global/globala.lzc") == fx.a);
    CHECK(read_file(fx.dst.path / "gamedata/cars/bmw m3/geo.bin") == fx.b);
    CHECK(fs::exists(fx.dst.path / "gamedata/empty.dat"));
    CHECK(!fs::exists(fx.dst.path / "gamedata/movies/intro.bik"));
    CHECK(!fs::exists(fx.dst.path / "gamedata/.import/journal.txt"));
    uint32_t files = 0;
    uint64_t bytes = 0;
    CHECK(ImportEngine::is_complete(*fsys, fx.dst.str("gamedata"), &files, &bytes));
    CHECK(files == 3 && bytes == r.bytes_total);
    std::vector<std::string> problems;
    CHECK(ImportEngine::verify_installed(*fsys, fx.dst.str("gamedata"), m, &problems));
    // no .part leftovers
    for (auto& e : fs::recursive_directory_iterator(fx.dst.path)) CHECK(e.path().extension() != ".part");
}

TEST(import, without_manifest_copies_everything) {
    Fixture fx;
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    ImportEngine eng(*fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    ImportReport r = eng.run(*src, nullptr, nullptr, cancel);
    CHECK(r.result == ImportResult::Ok);
    CHECK_EQ(r.files_total, 4u);
    CHECK(fs::exists(fx.dst.path / "g/movies/intro.bik"));
}

TEST(import, missing_required_fails_before_copying) {
    Fixture fx;
    fs::remove(fx.src.path / "Cars" / "BMW M3" / "geo.bin");
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    Manifest m = fx.manifest();
    ImportEngine eng(*fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    ImportReport r = eng.run(*src, &m, nullptr, cancel);
    CHECK(r.result == ImportResult::MissingRequired);
    CHECK_EQ(r.missing_required.size(), size_t(1));
    CHECK(!fs::exists(fx.dst.path / "g/global/globala.lzc")); // nothing copied
    CHECK(!ImportEngine::is_complete(*fsys, fx.dst.str("g")));
}

TEST(import, wrong_game_version_size_mismatch) {
    Fixture fx;
    Manifest m = fx.manifest();
    m.entries[1].size += 1; // manifest says the file is one byte larger than the user's copy
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    ImportEngine eng(*fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    ImportReport r = eng.run(*src, &m, nullptr, cancel);
    CHECK(r.result == ImportResult::SizeMismatch);
    CHECK_EQ(r.failed_file, std::string("cars/bmw m3/geo.bin"));
}

TEST(import, crc_mismatch_removes_partial_file) {
    Fixture fx;
    Manifest m = fx.manifest();
    m.entries[0].crc ^= 1;
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    ImportEngine eng(*fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    ImportReport r = eng.run(*src, &m, nullptr, cancel);
    CHECK(r.result == ImportResult::CrcMismatch);
    CHECK(!fs::exists(fx.dst.path / "g/global/globala.lzc"));
    CHECK(!fs::exists(fx.dst.path / "g/global/globala.lzc.part"));
    CHECK(!ImportEngine::is_complete(*fsys, fx.dst.str("g")));
}

TEST(import, name_collision_after_case_folding) {
    TempDir src, dst;
    write_file(src.path / "A.BIN", "1");
    write_file(src.path / "sub" / "x", "1");
    write_file(src.path / "sub" / "X", "2"); // distinct on a case-sensitive host, same name once folded
    auto fsys = make_std_file_system();
    auto s = make_directory_source(src.str());
    ImportEngine eng(*fsys, dst.str("g"));
    std::atomic<bool> cancel{false};
    ImportReport r = eng.run(*s, nullptr, nullptr, cancel);
#if defined(_WIN32)
    CHECK(r.result == ImportResult::Ok); // Windows is case-insensitive: only one file could be created
#else
    CHECK(r.result == ImportResult::NameCollision);
    CHECK_EQ(r.collisions.size(), size_t(1));
#endif
}

namespace {
class MemSource final : public ImportSource {
public:
    struct Item {
        std::string path, data;
    };
    explicit MemSource(std::vector<Item> items) : items_(std::move(items)) {
        for (const Item& i : items_) files_.push_back({i.path, i.data.size()});
    }
    const std::vector<SourceFile>& files() const override { return files_; }
    std::unique_ptr<ByteSource> open(size_t i) override {
        struct Bs final : ByteSource {
            explicit Bs(const std::string& d) : d_(d) {}
            int64_t read(void* dst, size_t n) override {
                size_t k = std::min(n, d_.size() - pos_);
                std::memcpy(dst, d_.data() + pos_, k);
                pos_ += k;
                return int64_t(k);
            }
            const std::string& d_;
            size_t pos_ = 0;
        };
        return std::make_unique<Bs>(items_[i].data);
    }

private:
    std::vector<Item> items_;
    std::vector<SourceFile> files_;
};
} // namespace

TEST(import, hostile_names_are_rejected) {
    TempDir dst;
    MemSource src({{"../escape.bin", "x"}, {"ok.bin", "y"}});
    auto fsys = make_std_file_system();
    ImportEngine eng(*fsys, dst.str("g"));
    std::atomic<bool> cancel{false};
    ImportReport r = eng.run(src, nullptr, nullptr, cancel);
    CHECK(r.result == ImportResult::BadPath);
    CHECK_EQ(r.bad_paths.size(), size_t(1));
    CHECK(!fs::exists(dst.path / "escape.bin"));
    CHECK(!fs::exists(dst.path.parent_path() / "escape.bin"));
}

TEST(import, free_space_precheck) {
    Fixture fx;
    auto real = make_std_file_system();
    LimitedFs fsys(*real);
    fsys.fake_free = 1000; // far below the 64 MiB margin
    auto src = make_directory_source(fx.src.str());
    Manifest m = fx.manifest();
    ImportEngine eng(fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    ImportReport r = eng.run(*src, &m, nullptr, cancel);
    CHECK(r.result == ImportResult::NoSpace);
    CHECK(r.space.result == SpaceCheck::NotEnough);
    CHECK(r.space.shortfall > 0);
    CHECK(!fs::exists(fx.dst.path / "g/global/globala.lzc"));
}

TEST(import, device_fills_up_midway_then_resume_succeeds) {
    Fixture fx;
    auto real = make_std_file_system();
    LimitedFs fsys(*real);
    auto src = make_directory_source(fx.src.str());
    Manifest m = fx.manifest();
    ImportEngine eng(fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};

    // sorted order: cars/bmw m3/geo.bin (1000), empty.dat (0), global/globala.lzc (200 KiB + 17)
    fsys.write_budget = 1000 + 1024; // the big file dies after 1 KiB
    ImportReport r1 = eng.run(*src, &m, nullptr, cancel);
    CHECK(r1.result == ImportResult::NoSpace);
    CHECK_EQ(r1.failed_file, std::string("global/globala.lzc"));
    CHECK(!ImportEngine::is_complete(*real, fx.dst.str("g")));
    CHECK(!fs::exists(fx.dst.path / "g/global/globala.lzc.part")); // partial removed
    CHECK(fs::exists(fx.dst.path / "g/cars/bmw m3/geo.bin"));       // finished file stays

    fsys.write_budget = -1;
    ImportReport r2 = eng.run(*src, &m, nullptr, cancel);
    CHECK(r2.result == ImportResult::Ok);
    CHECK_EQ(r2.files_resumed, 2u);   // geo.bin and empty.dat were journaled
    CHECK_EQ(r2.files_copied, 1u);
    CHECK(read_file(fx.dst.path / "g/global/globala.lzc") == fx.a);
    CHECK(ImportEngine::is_complete(*real, fx.dst.str("g")));
}

TEST(import, cancel_stops_and_resume_finishes) {
    Fixture fx;
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    Manifest m = fx.manifest();
    ImportEngine eng(*fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    ImportReport r1 = eng.run(*src, &m, [&](const ImportProgress& p) {
        if (p.bytes_done > 1500) cancel = true; // cancel inside the big file
    }, cancel);
    CHECK(r1.result == ImportResult::Cancelled);
    CHECK(!ImportEngine::is_complete(*fsys, fx.dst.str("g")));
    CHECK(!fs::exists(fx.dst.path / "g/global/globala.lzc"));
    CHECK(!fs::exists(fx.dst.path / "g/global/globala.lzc.part"));

    cancel = false;
    ImportReport r2 = eng.run(*src, &m, nullptr, cancel);
    CHECK(r2.result == ImportResult::Ok);
    CHECK(r2.files_resumed >= 1);
    CHECK(ImportEngine::is_complete(*fsys, fx.dst.str("g")));
}

TEST(import, resume_recopies_a_file_whose_size_changed_on_disk) {
    Fixture fx;
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    Manifest m = fx.manifest();
    {
        // pretend the first run was killed after finishing geo.bin and the journal says so, but the file is truncated
        ImportEngine first(*fsys, fx.dst.str("g"), Fixture::options());
        std::atomic<bool> cancel{false};
        // simulate the interruption with a cancel on the last file
        first.run(*src, &m, [&](const ImportProgress& p) {
            if (std::string(p.current) == "global/globala.lzc") cancel = true;
        }, cancel);
    }
    write_file(fx.dst.path / "g/cars/bmw m3/geo.bin", "short");
    ImportEngine second(*fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    ImportReport r = second.run(*src, &m, nullptr, cancel);
    CHECK(r.result == ImportResult::Ok);
    CHECK(read_file(fx.dst.path / "g/cars/bmw m3/geo.bin") == fx.b);
}

TEST(import, verify_installed_reports_problems) {
    Fixture fx;
    auto fsys = make_std_file_system();
    auto src = make_directory_source(fx.src.str());
    Manifest m = fx.manifest();
    ImportEngine eng(*fsys, fx.dst.str("g"), Fixture::options());
    std::atomic<bool> cancel{false};
    CHECK(eng.run(*src, &m, nullptr, cancel).result == ImportResult::Ok);
    fs::remove(fx.dst.path / "g/empty.dat");
    write_file(fx.dst.path / "g/cars/bmw m3/geo.bin", "x");
    std::vector<std::string> problems;
    CHECK(!ImportEngine::verify_installed(*fsys, fx.dst.str("g"), m, &problems));
    CHECK_EQ(problems.size(), size_t(2));
}
