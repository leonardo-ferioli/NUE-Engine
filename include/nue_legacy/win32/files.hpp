// Files and directories: CreateFile, ReadFile, WriteFile, SetFilePointer, GetFileSize, SetEndOfFile, FlushFileBuffers, GetFileType, DeleteFile, MoveFile,
// CreateDirectory, RemoveDirectory, FindFirstFile / FindNextFile / FindClose, GetFullPathName, GetLongPathName, GetCurrentDirectory, the drive queries,
// the standard handles and file mappings.
//
// The Win32 rules live here (dispositions and their errors, access and share modes, wildcard matching, "." and ".." entries, file attributes, path
// normalisation: drive letters, backslashes, "\\?\", trailing dots and spaces, ".." resolved lexically); the bytes come from platform::Storage.
// A Win32 path reaches Storage as "root:relative/path" when its drive letter is mapped to a root (the game's data folder is "data:"), and as a native
// path otherwise.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/win32/handles.hpp"
#include "nue_legacy/platform/storage.hpp"
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nue::legacy::win32 {

namespace access { enum : uint32_t { generic_read = 0x80000000u, generic_write = 0x40000000u, generic_execute = 0x20000000u, generic_all = 0x10000000u, file_read_data = 1, file_write_data = 2, file_append_data = 4, delete_ = 0x10000 }; }
namespace share { enum : uint32_t { read = 1, write = 2, delete_ = 4 }; }
namespace disposition { enum : uint32_t { create_new = 1, create_always = 2, open_existing = 3, open_always = 4, truncate_existing = 5 }; }
namespace attribute { enum : uint32_t { readonly = 1, hidden = 2, system = 4, directory = 0x10, archive = 0x20, normal = 0x80, invalid = 0xFFFFFFFFu }; }
namespace file_type { enum : uint32_t { unknown = 0, disk = 1, character = 2, pipe = 3 }; }
namespace seek_method { enum : uint32_t { begin = 0, current = 1, end = 2 }; }
namespace std_handle { enum : uint32_t { input = 0xFFFFFFF6u, output = 0xFFFFFFF5u, error = 0xFFFFFFF4u }; }   // STD_INPUT_HANDLE = (DWORD)-10 ...
constexpr uint64_t kInvalidFilePointer = 0xFFFFFFFFFFFFFFFFull;

struct FindData {                          // WIN32_FIND_DATA
    uint32_t attributes = 0;
    uint64_t creation_time = 0, access_time = 0, write_time = 0;   // FILETIME
    uint64_t size = 0;
    std::string name;                      // cFileName
    std::string alternate_name;            // cAlternateFileName (8.3): empty here unless the name needs none
};

struct FileSystemConfig {
    // The virtual current directory and the drive letters that are mapped to storage roots ("Z" -> "data:" ...). An unmapped drive letter is a
    // native path. With no mapping at all every path is native, which is what a Windows host running the game from its folder wants.
    std::string current_directory = "C:\\";
    std::map<char, std::string> drives;    // upper-case letter -> "root:" or "root:prefix/"
    uint32_t logical_drives_mask = 1u << 2;   // C: only
};

class FileSystem {
public:
    FileSystem(platform::Storage& storage, HandleTable& handles, FileSystemConfig config = {});

    Ret<Handle> create_file(std::string_view path, uint32_t desired_access, uint32_t share_mode, uint32_t disposition, uint32_t flags_and_attributes);
    Ret<uint32_t> read_file(Handle, MutableBytes out);                 // bytes read (0 at the end of the file)
    Ret<uint32_t> write_file(Handle, Bytes data);
    // SetFilePointer / SetFilePointerEx: returns the new position, or kInvalidFilePointer with ERROR_NEGATIVE_SEEK (131) / ERROR_INVALID_HANDLE.
    Ret<uint64_t> set_file_pointer(Handle, int64_t distance, uint32_t method);
    Ret<uint64_t> get_file_size(Handle);
    Ret<bool> set_end_of_file(Handle);
    Ret<bool> flush_file_buffers(Handle);
    uint32_t get_file_type(Handle);                                    // FILE_TYPE_UNKNOWN (with ERROR_INVALID_HANDLE reported by the facade) for a bad handle
    bool is_file_handle(Handle) const;

    Ret<bool> delete_file(std::string_view path);
    Ret<bool> move_file(std::string_view from, std::string_view to);
    Ret<bool> create_directory(std::string_view path);
    Ret<bool> remove_directory(std::string_view path);
    uint32_t get_file_attributes(std::string_view path);               // attribute::invalid with an error through `last_error`
    uint32_t last_error = 0;                                           // error of the previous get_file_attributes (single-threaded convenience)

    Ret<Handle> find_first_file(std::string_view pattern, FindData& out);
    Ret<bool> find_next_file(Handle, FindData& out);                   // ERROR_NO_MORE_FILES at the end
    bool find_close(Handle);

    // Path functions. `full_path` fails with ERROR_INVALID_NAME for a name Windows rejects.
    Ret<std::string> get_full_path_name(std::string_view path);
    Ret<std::string> get_long_path_name(std::string_view path);
    std::string get_current_directory() const { return config_.current_directory; }
    Ret<bool> set_current_directory(std::string_view path);
    uint32_t get_drive_type(std::string_view root);                    // DRIVE_FIXED (3), DRIVE_NO_ROOT_DIR (1) for a drive that does not exist
    uint32_t get_logical_drives() const { return config_.logical_drives_mask; }
    struct DiskSpace { uint64_t free_to_caller = 0, total = 0, total_free = 0; uint32_t sectors_per_cluster = 8, bytes_per_sector = 512; };
    Ret<DiskSpace> get_disk_free_space(std::string_view root);

    // Standard handles: console-like character devices unless the host sets them.
    Handle get_std_handle(uint32_t which);
    bool set_std_handle(uint32_t which, Handle);

    // File mappings: a copy of the file's bytes (documented: writes through a view reach the file when it is unmapped).
    Ret<Handle> create_file_mapping(Handle file, uint32_t protect, uint64_t maximum_size);
    Ret<MutableBytes> map_view_of_file(Handle mapping, bool write, uint64_t offset, size_t size);
    Ret<bool> unmap_view_of_file(const std::byte* base);

    // The mapping of a Win32 path to a Storage path, exposed for the facade and the tests.
    Ret<std::string> to_storage_path(std::string_view win32_path, bool* is_directory_syntax = nullptr);

    struct Impl;                           // shared state (open files for share-mode checks, standard handles); public only so the file objects can refer to it

private:
    std::shared_ptr<Impl> impl_;
    FileSystemConfig config_;
    platform::Storage& storage_;
    HandleTable& handles_;
};

// Wildcard matching with the DOS rules of FindFirstFile: '*' and '?', case-insensitive, "*.*" matches every name.
bool wildcard_match(std::string_view pattern, std::string_view name);

}  // namespace nue::legacy::win32
