// iOS storage layout, game-data validation and import planning as pure logic.
//
// Sandbox layout (names fixed here, directories resolved by the glue from NSSearchPathForDirectoriesInDomains):
//   Documents/<game_dir>/            the player's game files (Finder / Files app / import). Visible to the user (UIFileSharingEnabled).
//   Application Support/NUE/         saves and settings. Backed up by iCloud (small) unless excluded.
//   Caches/NUE/                      shader cache, transcoded textures. OS may purge it; never needed for correctness.
//   tmp/                             import staging
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nue::ios {

// ---- path hygiene ---------------------------------------------------------------------------------------------------------------------------

// Normalises a game-relative path: converts '\' to '/', removes "." and empty components, resolves "..", strips a leading slash/drive.
// Returns nullopt if the path would escape its root (zip-slip style) or contains NUL / control characters.
std::optional<std::string> normalize_relative_path(std::string_view path);

// ASCII case folding used for lookups. The game was written for Windows (case-insensitive); the iOS file system may be case-sensitive.
std::string fold_case(std::string_view s);

// Case-insensitive resolver over a directory listing taken once (relative paths as they exist on disk).
class CaseFoldIndex {
public:
    void add(std::string real_relative_path);
    // Returns the real on-disk spelling for any case variant of `relative_path` (normalised first), or nullopt.
    // If several on-disk entries fold to the same name the exact-case match wins, otherwise the lexicographically first.
    std::optional<std::string> resolve(std::string_view relative_path) const;
    size_t size() const { return map_.size(); }
    size_t collisions() const { return collisions_; }
private:
    std::unordered_map<std::string, std::string> map_;
    size_t collisions_ = 0;
};

// ---- game data validation -------------------------------------------------------------------------------------------------------------------

struct RequiredFile {
    std::string path;        // relative to the game root, any case
    uint64_t min_size = 1;   // anything smaller counts as truncated (a partial copy)
};

struct DirectoryEntry { std::string path; uint64_t size = 0; };

struct ValidationReport {
    std::vector<std::string> missing;
    std::vector<std::string> truncated;
    uint64_t total_bytes = 0;
    bool ok() const { return missing.empty() && truncated.empty(); }
};

ValidationReport validate_game_data(const std::vector<DirectoryEntry>& listing, const std::vector<RequiredFile>& required);

// If the player copied the folder one level too deep (Documents/NFSU2/NFSU2/SPEED2.EXE) find the real root.
// `marker` is a relative path expected inside the root (e.g. "SPEED2.EXE"). Returns the root prefix ("" = listing root, "NFSU2/" ...).
std::optional<std::string> find_game_root(const std::vector<DirectoryEntry>& listing, std::string_view marker, int max_depth = 2);

// ---- import planning (UIDocumentPicker -> sandbox copy) -----------------------------------------------------------------------------------

struct ImportItem { std::string source_relative; std::string dest_relative; uint64_t size = 0; };

struct ImportPlan {
    std::vector<ImportItem> items;
    std::vector<std::string> rejected;     // unsafe paths (escape the root) that were skipped
    uint64_t total_bytes = 0;
    uint64_t required_free_bytes = 0;      // total + safety margin
    bool enough_space = false;
};

// free_bytes: from NSURLVolumeAvailableCapacityForImportantUsageKey. margin: slack for filesystem overhead (default 5 % or 64 MiB).
ImportPlan plan_import(const std::vector<DirectoryEntry>& source_listing, uint64_t free_bytes, uint64_t already_present_bytes = 0);

// Progress accounting shared by the copy loop and the UI (monotonic, clamps at 100).
struct Progress {
    uint64_t total = 0, done = 0;
    int percent() const { return total ? static_cast<int>(done >= total ? 100 : done * 100 / total) : 100; }
};

// ---- integrity ------------------------------------------------------------------------------------------------------------------------------

class Crc32 {
public:
    void update(const void* data, size_t size);
    uint32_t value() const { return ~state_; }
private:
    uint32_t state_ = 0xFFFFFFFFu;
};
uint32_t crc32(const void* data, size_t size);

// ---- sandbox layout -------------------------------------------------------------------------------------------------------------------------

struct SandboxLayout {
    std::string game_dir_name = "NFSU2";    // under Documents
    std::string support_dir_name = "NUE";   // under Application Support and Caches
    std::string game_root(std::string_view documents) const;
    std::string saves_dir(std::string_view app_support) const;
    std::string shader_cache_dir(std::string_view caches) const;
    std::string import_staging_dir(std::string_view tmp) const;
};

std::string path_join(std::string_view a, std::string_view b);

}  // namespace nue::ios
