// ABI guard: the layout of the public C API (struct sizes and member offsets, enum values, numeric constants, function names) must not change
// without an API version bump. The table is produced from include/nue/*.h by tools/abi/gen_abi_probe.py (build time) and compared with the committed
// reference tests/abi/reference/<layout>.txt. Rules and workflow: docs/ABI.md, section "ABI guard".
//
//   NUE_ABI_UPDATE=1 ctest -R nue_test_abi     rewrite the reference of this layout (only allowed when the version was bumped correctly)
#include "nue_test.h"
#include "nue/nue.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef NUE_ABI_REFERENCE_DIR
#  error "NUE_ABI_REFERENCE_DIR must point at tests/abi/reference"
#endif
#ifndef NUE_PROJECT_VERSION
#  error "NUE_PROJECT_VERSION (CMake project version) is required"
#endif

namespace {

// ---- the table of the library we are compiled against ----------------------------------------------------------------------------------

struct AbiTable {
    std::map<std::string, std::string> entries;    // "group kind name" -> value text
    std::map<std::string, unsigned long long> versions;   // group -> version number
    std::string group = "core";

    void set_group(const char* g, unsigned long long version) { group = g; versions[g] = version; }
    void add_struct(const char* name, size_t size, size_t align) { entries[group + " struct " + name] = "size=" + std::to_string(size) + " align=" + std::to_string(align); }
    void add_field(const char* st, const char* member, size_t off, size_t size) {
        entries[group + " field " + st + "." + member] = "off=" + std::to_string(off) + " size=" + std::to_string(size);
    }
    void add_enum(const char* name, long long v) { entries[group + " enum " + name] = std::to_string(v); }
    void add_define(const char* name, unsigned long long v) { entries[group + " define " + name] = std::to_string(v); }
    void add_function(const char* name) { entries[group + " function " + name] = ""; }
};

}  // namespace

#include "abi_probe.inc"   // generated: includes the public headers and defines probe_all(AbiTable&)

namespace {

std::string layout_key() {
    std::string key = sizeof(void*) == 8 ? "ptr64" : "ptr32";
    if (alignof(uint64_t) == 4) key += "-a4";   // i386 System V: 64-bit members align to 4
    return key;
}

// ---- reference file ----------------------------------------------------------------------------------------------------------------------

struct Reference {
    bool found = false;
    std::map<std::string, std::string> entries;
    std::map<std::string, unsigned long long> versions;
};

Reference load_reference(const std::string& path) {
    Reference ref;
    std::ifstream in(path);
    if (!in) return ref;
    ref.found = true;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string a, b, c;
        ss >> a >> b >> c;
        if (a == "version") {            // version <group> <number>
            ref.versions[b] = std::strtoull(c.c_str(), nullptr, 10);
            continue;
        }
        // "<group> <kind> <name> <value...>"
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        ref.entries[a + " " + b + " " + c] = rest;
    }
    return ref;
}

std::string render(const AbiTable& t, const std::string& key) {
    std::ostringstream out;
    out << "# NUE public C ABI reference for layout '" << key << "'. GENERATED: do not edit by hand.\n"
        << "# Regenerate with  NUE_ABI_UPDATE=1 ctest -R nue_test_abi  after bumping the API version (docs/ABI.md, \"ABI guard\").\n";
    for (const auto& v : t.versions) out << "version " << v.first << " " << v.second << "\n";
    for (const auto& e : t.entries) out << e.first << (e.second.empty() ? "" : " ") << e.second << "\n";
    return out.str();
}

// ---- comparison --------------------------------------------------------------------------------------------------------------------------

struct Diff {
    std::vector<std::string> breaking, added;
    std::map<std::string, bool> group_breaking, group_changed;   // by group
};

std::string group_of(const std::string& id) { return id.substr(0, id.find(' ')); }
std::string kind_of(const std::string& id) { size_t a = id.find(' '); return id.substr(a + 1, id.find(' ', a + 1) - a - 1); }
std::string name_of(const std::string& id) { size_t a = id.find(' '); return id.substr(id.find(' ', a + 1) + 1); }

unsigned long long number_after(const std::string& value, const char* tag) {
    size_t p = value.find(tag);
    return p == std::string::npos ? 0 : std::strtoull(value.c_str() + p + std::char_traits<char>::length(tag), nullptr, 10);
}

Diff compare(const std::map<std::string, std::string>& ref, const std::map<std::string, std::string>& now) {
    Diff d;
    auto breaking = [&](const std::string& id, const std::string& why) {
        d.breaking.push_back(id + ": " + why);
        d.group_breaking[group_of(id)] = d.group_changed[group_of(id)] = true;
    };
    auto added = [&](const std::string& id, const std::string& why) {
        d.added.push_back(id + ": " + why);
        d.group_changed[group_of(id)] = true;
    };
    for (const auto& r : ref) {
        auto it = now.find(r.first);
        if (it == now.end()) { breaking(r.first, "removed"); continue; }
        if (it->second == r.second) continue;
        const std::string kind = kind_of(r.first);
        if (kind == "struct") {
            const unsigned long long old_size = number_after(r.second, "size="), new_size = number_after(it->second, "size=");
            const bool same_align = number_after(r.second, "align=") == number_after(it->second, "align=");
            const std::string guard = group_of(r.first) + " field " + name_of(r.first) + ".struct_size";
            const bool versioned = ref.count(guard) && now.count(guard);   // starts with struct_size: may grow at the end
            if (same_align && new_size > old_size && versioned) added(r.first, "grew " + r.second + " -> " + it->second);
            else breaking(r.first, "was " + r.second + ", now " + it->second);
        } else {
            breaking(r.first, "was '" + r.second + "', now '" + it->second + "'");
        }
    }
    for (const auto& n : now)
        if (!ref.count(n.first)) added(n.first, "new " + n.second);
    return d;
}

std::string lines_of(const std::vector<std::string>& v, size_t limit) {
    std::string out;
    for (size_t i = 0; i < v.size() && i < limit; ++i) out += "    " + v[i] + "\n";
    if (v.size() > limit) out += "    ... and " + std::to_string(v.size() - limit) + " more\n";
    return out;
}

// ---- the tests ---------------------------------------------------------------------------------------------------------------------------

void check_group_versions(const Reference& ref, const AbiTable& now, const Diff& d, bool* needs_refresh, bool* ok) {
    for (const auto& g : d.group_changed) {
        const unsigned long long was = ref.versions.count(g.first) ? ref.versions.at(g.first) : 0;
        const unsigned long long is = now.versions.count(g.first) ? now.versions.at(g.first) : 0;
        if (is <= was) {
            std::printf("  ABI '%s': the layout changed but its API version did not increase (reference %llu, now %llu).\n"
                        "    additive change -> bump the minor/patch number; breaking change -> bump the major number (or the module version)\n",
                        g.first.c_str(), was, is);
            *ok = false;
        } else if (g.first == "core" && d.group_breaking.count("core")) {
            // breaking: the MAJOR number must change, except while the API is 0.x, where a MINOR bump is enough
            const unsigned was_major = unsigned((was >> 24) & 0xFF), now_major = unsigned((is >> 24) & 0xFF);
            const unsigned was_minor = unsigned((was >> 12) & 0xFFF), now_minor = unsigned((is >> 12) & 0xFFF);
            const bool allowed = now_major > was_major || (now_major == 0 && was_major == 0 && now_minor > was_minor);
            if (!allowed) {
                std::printf("  ABI 'core': BREAKING changes need a new MAJOR version (a MINOR bump while 0.x); the version went %u.%u.%llu -> %u.%u.%llu\n",
                            was_major, was_minor, was & 0xFFF, now_major, now_minor, is & 0xFFF);
                *ok = false;
            }
        }
        if (*ok) *needs_refresh = true;
    }
}

TEST_CASE(version_numbers_agree) {
    // the header macros, the library and the CMake project must name the same version
    char expected[32];
    std::snprintf(expected, sizeof(expected), "%d.%d.%d", NUE_VERSION_MAJOR, NUE_VERSION_MINOR, NUE_VERSION_PATCH);
    CHECK_STR(expected, NUE_PROJECT_VERSION);
    CHECK_EQ(nue_get_version(), NUE_API_VERSION);
}

TEST_CASE(public_abi_matches_the_committed_reference) {
    AbiTable now;
    probe_all(now);
    CHECK(now.entries.size() > 500);          // the generator really found the headers' content
    CHECK(now.versions.count("core") == 1);

    const std::string key = layout_key();
    const std::string path = std::string(NUE_ABI_REFERENCE_DIR) + "/" + key + ".txt";
    const bool update = std::getenv("NUE_ABI_UPDATE") && std::string(std::getenv("NUE_ABI_UPDATE")) == "1";
    Reference ref = load_reference(path);
    if (!ref.found) {
        if (update) {
            std::ofstream(path, std::ios::binary) << render(now, key);
            std::printf("  ABI: reference for layout '%s' created at %s\n", key.c_str(), path.c_str());
            return;
        }
        if (key == "ptr64" || key == "ptr32") {
            CHECK(!"missing ABI reference for a layout we ship: run NUE_ABI_UPDATE=1 ctest -R nue_test_abi on this platform and commit tests/abi/reference");
        }
        std::printf("  ABI: no reference table for layout '%s' (%s): skipped. Add one with NUE_ABI_UPDATE=1.\n", key.c_str(), path.c_str());
        return;
    }

    Diff d = compare(ref.entries, now.entries);
    bool versions_differ = false;
    for (const auto& v : now.versions) if (!ref.versions.count(v.first) || ref.versions[v.first] != v.second) versions_differ = true;
    if (d.breaking.empty() && d.added.empty()) {
        if (versions_differ && update) {          // only the version moved: refresh the stamp
            std::ofstream(path, std::ios::binary) << render(now, key);
            std::printf("  ABI: version stamp refreshed in %s\n", path.c_str());
        }
        CHECK(true);
        return;
    }

    std::printf("  ABI of layout '%s' differs from tests/abi/reference/%s.txt\n", key.c_str(), key.c_str());
    if (!d.breaking.empty()) std::printf("  BREAKING (%zu):\n%s", d.breaking.size(), lines_of(d.breaking, 40).c_str());
    if (!d.added.empty()) std::printf("  ADDED (%zu):\n%s", d.added.size(), lines_of(d.added, 40).c_str());

    bool ok = true, needs_refresh = false;
    check_group_versions(ref, now, d, &needs_refresh, &ok);
    if (!ok) { CHECK(!"ABI changed without the matching version bump (see the messages above and docs/ABI.md)"); return; }

    if (update) {
        std::ofstream(path, std::ios::binary) << render(now, key);
        std::printf("  ABI: reference %s rewritten for the bumped version; remember the other layouts (run the update on x86 too) and docs/API_CHANGES.md\n", path.c_str());
        return;
    }
    const std::string proposed = "abi_" + key + ".proposed.txt";
    std::ofstream(proposed, std::ios::binary) << render(now, key);
    std::printf("  The version was bumped, so the change is allowed: refresh the reference with NUE_ABI_UPDATE=1 ctest -R nue_test_abi\n"
                "  (or copy the proposed table ./%s over tests/abi/reference/%s.txt) and commit it.\n", proposed.c_str(), key.c_str());
    CHECK(!"the committed ABI reference is out of date");
}

TEST_CASE(structs_that_cross_the_abi_start_with_struct_size_or_are_plain_data) {
    // the sizes the whole design relies on (docs/ABI.md): the 64-byte event, fixed-width handles
    CHECK_EQ(sizeof(nue_event), 64u);
    CHECK_EQ(sizeof(nue_result), 4u);
    CHECK_EQ(sizeof(nue_bool), 1u);
    CHECK_EQ(sizeof(nue_file), 4u);
}

}  // namespace

TEST_MAIN_BEGIN
    RUN(version_numbers_agree);
    RUN(public_abi_matches_the_committed_reference);
    RUN(structs_that_cross_the_abi_start_with_struct_size_or_are_plain_data);
TEST_MAIN_END
