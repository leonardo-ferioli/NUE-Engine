// Thin OS file-system / time helpers used by the core (UTF-8 paths on every platform). Implemented in os_util.cpp with #ifdefs.
#pragma once

#include "nue/nue.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace nuei {

FILE* fopen_utf8(const std::string& path, const char* mode);
bool os_remove_file(const std::string& path);                     // file only
bool os_rename_file(const std::string& from, const std::string& to);   // replaces an existing target
bool os_make_dirs(const std::string& path);                       // creates parents, true if it exists afterwards
bool os_make_parent_dirs(const std::string& path);
bool os_remove_dir_tree(const std::string& path);
bool os_remove_dir(const std::string& path);
void os_localtime(std::time_t t, std::tm* out);
nue_result os_last_error();                                       // the calling thread's last OS error (errno / GetLastError) as a nue_result

struct OsDirEntry {
    std::string name;
    nue_entry_kind kind = NUE_ENTRY_NONE;
    uint64_t size = 0;
    int64_t modified_unix_ms = 0;
};
bool os_stat_path(const std::string& path, nue_stat* out);
bool os_list_dir(const std::string& path, std::vector<OsDirEntry>* out);   // false if not a directory

using OsHandle = intptr_t;
constexpr OsHandle kBadOsHandle = -1;
nue_result os_file_open(const std::string& path, uint32_t open_flags /* NUE_OPEN_* */, OsHandle* out);
void os_file_close(OsHandle h);
nue_result os_file_read_at(OsHandle h, uint64_t offset, void* buffer, size_t size, size_t* out_read);
nue_result os_file_write_at(OsHandle h, uint64_t offset, const void* data, size_t size, size_t* out_written);
nue_result os_file_size(OsHandle h, uint64_t* out);
nue_result os_file_set_size(OsHandle h, uint64_t size);
nue_result os_file_sync(OsHandle h);

std::string os_executable_dir();       // directory of the running executable, '/' separated, no trailing slash
std::string os_temp_dir();
std::string os_getenv(const char* name);

// Case folding used by path matching: ASCII + Latin-1/Greek/Cyrillic simple folding on UTF-8 text.
std::string fold_case_utf8(const std::string& s);

}  // namespace nuei
