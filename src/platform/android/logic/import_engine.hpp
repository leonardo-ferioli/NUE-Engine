// Game-data import: copies the files the user picked (Storage Access Framework tree or a plain directory)
// into app-private storage, verifies them and leaves a marker.  The engine knows nothing about Android:
// bytes come from an ImportSource and go through a FileSystem, so the same code runs in host tests with
// directories and on the phone with file descriptors detached from the content resolver.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "storage_paths.hpp"

namespace nue::android {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Returns the number of bytes read, 0 at end of file, < 0 on error.
    virtual int64_t read(void* dst, size_t size) = 0;
};

struct SourceFile {
    std::string path;     // relative to the picked root, any separator style
    uint64_t size = 0;
};

class ImportSource {
public:
    virtual ~ImportSource() = default;
    virtual const std::vector<SourceFile>& files() const = 0;
    virtual std::unique_ptr<ByteSource> open(size_t index) = 0;
};

enum class WriteStatus { Ok, NoSpace, Error };

class WritableFile {
public:
    virtual ~WritableFile() = default;
    virtual WriteStatus write(const void* data, size_t size) = 0;
    // Flushes to stable storage when `sync`, then closes.  The object must not be used afterwards.
    virtual bool close(bool sync) = 0;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual bool make_dirs(const std::string& path) = 0;
    virtual std::unique_ptr<WritableFile> create(const std::string& path) = 0; // truncates
    virtual bool rename(const std::string& from, const std::string& to) = 0;   // replaces the target
    virtual bool remove(const std::string& path) = 0;                          // missing file is success
    virtual bool file_size(const std::string& path, uint64_t* size) = 0;
    virtual bool free_space(const std::string& path, uint64_t* bytes) = 0;
    virtual bool read_text(const std::string& path, std::string* out) = 0;
    virtual bool write_text_atomic(const std::string& path, const std::string& text) = 0;
    virtual bool append_text(const std::string& path, const std::string& text, bool sync) = 0;
};

// std::filesystem + stdio implementation (host tests, and the phone).
std::unique_ptr<FileSystem> make_std_file_system();

// ImportSource over a directory tree on the file system (host tests, Android/data drop folder).
std::unique_ptr<ImportSource> make_directory_source(const std::string& root);

enum class ImportPhase { Idle, Scanning, Copying, Verifying, Done, Cancelled, Failed };

struct ImportProgress {
    ImportPhase phase = ImportPhase::Idle;
    uint32_t files_done = 0, files_total = 0;
    uint64_t bytes_done = 0, bytes_total = 0;
    const char* current = ""; // valid during the callback only
};
using ProgressFn = std::function<void(const ImportProgress&)>;

enum class ImportResult {
    Ok,
    Cancelled,
    NoSpace,        // free-space pre-check or the device ran out while writing
    ReadError,
    WriteError,
    CrcMismatch,
    SizeMismatch,   // source file size differs from the manifest (wrong game version) or from its listing
    BadPath,
    NameCollision,  // two source files fold to the same destination name
    MissingRequired // a required manifest file is not in the source
};
const char* to_string(ImportResult r);

struct ImportOptions {
    NameCase name_case = NameCase::Lower;
    bool fsync_files = true;           // fsync every file before the rename: a crash never leaves a short file
    bool verify_crc = true;            // compare with the manifest CRC when the manifest has one
    size_t buffer_size = 1u << 20;     // 1 MiB: large enough for flash, small enough for the cancel latency
    uint64_t progress_step_bytes = 4u << 20;
    uint32_t marker_version = 1;
};

struct ImportReport {
    ImportResult result = ImportResult::Ok;
    uint32_t files_total = 0;
    uint32_t files_copied = 0;
    uint32_t files_resumed = 0;        // already present and journaled from an earlier run
    uint64_t bytes_total = 0;
    uint64_t bytes_copied = 0;
    SpaceReport space;
    std::vector<std::string> missing_required;
    std::vector<std::string> collisions;
    std::vector<std::string> bad_paths;
    std::string failed_file;           // the file being copied when a copy error happened
};

class ImportEngine {
public:
    ImportEngine(FileSystem& fs, std::string dest_root, ImportOptions options = {});

    // `manifest` may be null: then every source file is copied.  With a manifest only its entries are copied
    // (extra files in the source, e.g. the 2.5 GB the game never reads, are ignored).
    // `cancel` is polled between buffers.
    ImportReport run(ImportSource& source, const Manifest* manifest, const ProgressFn& progress,
                     const std::atomic<bool>& cancel);

    // True when the marker exists and parses; fills the counts.
    static bool is_complete(FileSystem& fs, const std::string& dest_root, uint32_t* files = nullptr,
                            uint64_t* bytes = nullptr);
    // Cheap re-check at game start: every required manifest file exists with the right size.
    static bool verify_installed(FileSystem& fs, const std::string& dest_root, const Manifest& manifest,
                                 std::vector<std::string>* problems);

private:
    FileSystem& fs_;
    std::string root_;
    ImportOptions opt_;
    ImportLayout layout_;
};

} // namespace nue::android
