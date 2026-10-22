// Storage: Windows known folders (Saved Games, AppData...), long paths (\\?\), native case-insensitive semantics (and detection of
// per-directory case sensitivity), crash-safe file replacement, free space, cloud-placeholder awareness.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

enum class KnownFolder : uint8_t { SavedGames, LocalAppData, RoamingAppData, Documents, ProgramData, Desktop, Downloads, Temp };
// Full path without trailing separator. Redirected folders (OneDrive "Known Folder Move") are returned as Windows redirects them.
Status known_folder_path(KnownFolder folder, std::wstring& out);
// True when `path` lies under the OneDrive root (%OneDrive%, %OneDriveConsumer%, %OneDriveCommercial%): files may be placeholders.
bool path_is_under_onedrive(const std::wstring& path);

struct AppDirs {
    std::wstring saves;        // Saved Games\<vendor>\<app>   (what players expect to find and back up)
    std::wstring config;       // %AppData%\<vendor>\<app>     (roams with the profile)
    std::wstring cache;        // %LocalAppData%\<vendor>\<app>\cache
    std::wstring logs;         // %LocalAppData%\<vendor>\<app>\logs
    std::wstring crash_dumps;  // %LocalAppData%\<vendor>\<app>\crashdumps
    std::wstring install;      // directory of the running executable (read only for the game)
    bool saves_on_onedrive = false;
};
struct AppDirOptions {
    bool create = true;
    // Tests and portable installs: every writable directory goes under this root instead of the known folders (saves/, config/, cache/, logs/, crashdumps/).
    std::wstring root_override;
};
Status resolve_app_dirs(const std::string& vendor, const std::string& app, const AppDirOptions& options, AppDirs& out);

// ---- Paths ----------------------------------------------------------------------------------------------------------------------------------------
// Turns a path into what the wide Win32 file APIs accept for any length: absolute, normalised separators, "\\?\" (or "\\?\UNC\") prefix.
// Paths already prefixed, device paths and relative inputs are made absolute first. Dots are resolved by GetFullPathNameW.
std::wstring to_extended_path(const std::wstring& path);
std::wstring strip_extended_prefix(const std::wstring& path);
bool long_paths_enabled();   // the process opted in (manifest longPathAware + registry) so that even non-prefixed paths may exceed MAX_PATH
// UTF-8 in, UTF-16 out, slashes unified; what the core API's path arguments go through.
std::wstring path_from_utf8(const std::string& path);
std::string path_to_utf8(const std::wstring& path);
// Replaces characters Windows forbids in a file name and avoids reserved device names (CON, NUL, COM1...), trailing dots and spaces.
std::string sanitize_file_name(const std::string& name);
bool is_reserved_file_name(const std::string& name);

// ---- Case semantics -----------------------------------------------------------------------------------------------------------------------
enum class CaseMode : uint8_t { Insensitive, Sensitive, Unknown };
// NTFS is case-insensitive by default; WSL can switch single directories to case sensitive (fsutil file setCaseSensitiveInfo).
CaseMode directory_case_mode(const std::wstring& directory);
// The same comparison the file system uses by default (ordinal, case-insensitive, with Unicode simple case folding).
bool paths_equal_ignore_case(const std::wstring& a, const std::wstring& b);
// Finds the real on-disk spelling of `relative` below `base`, matching each component without regard to case. Needed only when
// `base` is case sensitive; on a normal directory it simply verifies existence. Returns false if a component does not exist.
bool resolve_case_insensitive(const std::wstring& base, const std::wstring& relative, std::wstring& real_path);

// ---- Files ------------------------------------------------------------------------------------------------------------------------------------
Status create_directories(const std::wstring& path);
Status read_file(const std::wstring& path, std::vector<uint8_t>& out, uint64_t max_size = 1ull << 31);
// Writes to a temporary file in the same directory, flushes it, then replaces the target with MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH):
// a power cut leaves either the old or the new file, never half of one.
Status write_file_atomic(const std::wstring& path, const void* data, size_t size);
Status remove_tree(const std::wstring& path);
struct DiskSpace { uint64_t free_for_user = 0, total = 0; };
Status disk_space(const std::wstring& path, DiskSpace& out);
// True for OneDrive "files on demand" placeholders: reading them downloads the file (latency, may fail offline).
bool file_is_cloud_placeholder(const std::wstring& path);

}  // namespace nue::win
