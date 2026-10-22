// Pure path / manifest / free-space logic of the game-data import (no Android and no file system access).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nue::android {

enum class PathStatus { Ok, Empty, Absolute, Traversal, BadChar, TooLong };

struct SafePath {
    PathStatus status = PathStatus::Empty;
    std::string path; // normalised: '/' separators, no "." / ".." / empty components
};

enum class NameCase { Preserve, Lower };

// Turns a path read from the document provider (or from a manifest) into a relative path that is safe to
// join below the import root.  Windows separators are accepted because the game data comes from a PC.
// Rejects absolute paths, drive letters, "..", NUL / control characters, components over 255 bytes and
// paths over 4096 bytes.  Lower case folding is ASCII only on purpose: the Windows game never uses
// non-ASCII names, and ASCII folding is stable across locales.
SafePath sanitize_relative_path(std::string_view in, NameCase name_case = NameCase::Lower);

std::string to_lower_ascii(std::string_view s);
std::string join_path(std::string_view a, std::string_view b);
// "dir/file.ext" -> "dir"; "file" -> "".
std::string parent_path(std::string_view p);

struct ManifestEntry {
    std::string path; // already sanitised
    uint64_t size = 0;
    bool has_crc = false;
    uint32_t crc = 0;
    bool optional = false;
};

struct Manifest {
    std::vector<ManifestEntry> entries;
    std::vector<std::string> errors; // human readable, one per rejected line
    uint64_t required_bytes() const;
    const ManifestEntry* find(std::string_view sanitised_path) const;
};

// Text format, one file per line:
//     # comment
//     <size> <crc32 as 8 hex digits or '-'> <relative path>      required file
//     ? <size> <crc32 or '-'> <relative path>                    optional file (copied when present)
// The path is the rest of the line (it may contain spaces).  Duplicate paths after case folding are errors.
Manifest parse_manifest(std::string_view text, NameCase name_case = NameCase::Lower);
std::string format_manifest_line(const ManifestEntry& e);

enum class SpaceCheck { Ok, NotEnough };

struct SpaceReport {
    SpaceCheck result = SpaceCheck::Ok;
    uint64_t needed = 0;   // bytes still to write plus the safety margin
    uint64_t available = 0;
    uint64_t shortfall = 0;
};

// Free space needed for an import: the bytes still to be copied plus a margin of max(64 MiB, 2 %) for
// file system metadata, the journal and `.part` rounding.  `already_present` bytes (resume) are not counted.
SpaceReport check_free_space(uint64_t bytes_to_copy, uint64_t free_bytes);

struct ImportLayout {
    std::string root;      // <files>/gamedata
    std::string staging;   // <root>/.import
    std::string journal;   // <root>/.import/journal.txt
    std::string complete;  // <root>/.import/complete.txt
    static ImportLayout for_root(std::string_view root);
    std::string part_path(std::string_view final_path) const { return std::string(final_path) + ".part"; }
};

// Marker file written when everything verified.  The game start only needs `is_complete_marker`.
std::string make_complete_marker(uint32_t version, uint32_t files, uint64_t bytes);
bool parse_complete_marker(std::string_view text, uint32_t* version, uint32_t* files, uint64_t* bytes);

struct JournalEntry {
    std::string path;
    uint64_t size = 0;
    uint32_t crc = 0;
};
std::string format_journal_line(const JournalEntry& e);
// Tolerates a torn last line (the app can be killed while appending).
std::vector<JournalEntry> parse_journal(std::string_view text);

} // namespace nue::android
