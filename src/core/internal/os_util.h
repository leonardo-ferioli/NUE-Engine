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

// Read-only memory mapping of [offset, offset+length) (length > 0, the range must lie inside the file). The offset need not be aligned:
// OsMapping remembers the real (aligned) region that os_file_unmap releases. The mapping stays valid after the file handle is closed.
struct OsMapping { void* base = nullptr; size_t length = 0; };
nue_result os_file_map(OsHandle h, uint64_t offset, size_t length, const void** out_data, OsMapping* out_mapping);
void os_file_unmap(const OsMapping& mapping);

// Virtual address space (nue/vm.h). Raw OS layer: sizes are multiples of the page size (granularity for reservations) and already validated by the caller.
struct OsVmInfo { uint32_t page_size = 4096, granularity = 4096, address_bits = 47; uint64_t address_limit = 0; bool can_exec = true; };
OsVmInfo os_vm_info();
// Reserves `size` bytes of address space (no commit charge, PROT_NONE / PAGE_NOACCESS) at a multiple of `alignment`. `hint` is a placement hint, or the exact
// address when `exact`. NUE_ERR_OUT_OF_MEMORY when the OS refuses.
nue_result os_vm_reserve(size_t size, size_t alignment, const void* hint, bool exact, void** out_base);
nue_result os_vm_commit(void* address, size_t size, uint32_t protection /* NUE_VM_* */);
nue_result os_vm_protect(void* address, size_t size, uint32_t protection);
nue_result os_vm_decommit(void* address, size_t size);   // back to reserved; contents dropped
void os_vm_release(void* base, size_t size);

std::string os_executable_dir();       // directory of the running executable, '/' separated, no trailing slash
std::string os_temp_dir();
std::string os_getenv(const char* name);
uint32_t os_process_id();

// Case folding used by path matching: ASCII + Latin-1/Greek/Cyrillic simple folding on UTF-8 text.
std::string fold_case_utf8(const std::string& s);

}  // namespace nuei
