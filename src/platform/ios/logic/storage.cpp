#include "storage.hpp"

#include <algorithm>
#include <array>

namespace nue::ios {

std::optional<std::string> normalize_relative_path(std::string_view path) {
    for (char ch : path)
        if (ch == '\0' || (static_cast<unsigned char>(ch) < 0x20)) return std::nullopt;
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() -> bool {
        if (cur.empty() || cur == ".") { cur.clear(); return true; }
        if (cur == "..") {
            if (parts.empty()) return false;   // escapes the root
            parts.pop_back();
        } else {
            parts.push_back(cur);
        }
        cur.clear();
        return true;
    };
    // Drop a Windows drive prefix ("C:") so an absolute Windows path cannot name a different root.
    size_t start = 0;
    if (path.size() >= 2 && path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) start = 2;
    for (size_t i = start; i < path.size(); ++i) {
        char ch = path[i];
        if (ch == '/' || ch == '\\') { if (!flush()) return std::nullopt; }
        else cur.push_back(ch);
    }
    if (!flush()) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out.push_back('/'); out += parts[i]; }
    return out;
}

std::string fold_case(std::string_view s) {
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

void CaseFoldIndex::add(std::string real) {
    auto key = fold_case(real);
    auto it = map_.find(key);
    if (it == map_.end()) { map_.emplace(std::move(key), std::move(real)); return; }
    ++collisions_;
    if (real < it->second) it->second = std::move(real);
}

std::optional<std::string> CaseFoldIndex::resolve(std::string_view relative_path) const {
    auto norm = normalize_relative_path(relative_path);
    if (!norm) return std::nullopt;
    auto it = map_.find(fold_case(*norm));
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

ValidationReport validate_game_data(const std::vector<DirectoryEntry>& listing, const std::vector<RequiredFile>& required) {
    ValidationReport r;
    std::unordered_map<std::string, uint64_t> sizes;
    for (const auto& e : listing) {
        auto n = normalize_relative_path(e.path);
        if (!n) continue;
        r.total_bytes += e.size;
        sizes[fold_case(*n)] = e.size;
    }
    for (const auto& req : required) {
        auto n = normalize_relative_path(req.path);
        if (!n) { r.missing.push_back(req.path); continue; }
        auto it = sizes.find(fold_case(*n));
        if (it == sizes.end()) r.missing.push_back(req.path);
        else if (it->second < req.min_size) r.truncated.push_back(req.path);
    }
    return r;
}

std::optional<std::string> find_game_root(const std::vector<DirectoryEntry>& listing, std::string_view marker, int max_depth) {
    auto m = normalize_relative_path(marker);
    if (!m) return std::nullopt;
    const std::string mf = fold_case(*m);
    std::optional<std::string> best;
    int best_depth = max_depth + 1;
    for (const auto& e : listing) {
        auto n = normalize_relative_path(e.path);
        if (!n) continue;
        std::string f = fold_case(*n);
        if (f.size() < mf.size() || f.compare(f.size() - mf.size(), mf.size(), mf) != 0) continue;
        std::string prefix = n->substr(0, n->size() - mf.size());
        if (!prefix.empty() && prefix.back() != '/') continue;   // suffix matched inside a longer file name
        int depth = static_cast<int>(std::count(prefix.begin(), prefix.end(), '/'));
        if (depth < best_depth) { best_depth = depth; best = prefix; }
    }
    return best;
}

ImportPlan plan_import(const std::vector<DirectoryEntry>& listing, uint64_t free_bytes, uint64_t already_present) {
    ImportPlan p;
    for (const auto& e : listing) {
        auto n = normalize_relative_path(e.path);
        if (!n || n->empty()) { p.rejected.push_back(e.path); continue; }
        p.items.push_back({e.path, *n, e.size});
        p.total_bytes += e.size;
    }
    // Files already present (a resumed import) do not need space twice.
    const uint64_t need = p.total_bytes > already_present ? p.total_bytes - already_present : 0;
    const uint64_t margin = std::max<uint64_t>(64ull << 20, need / 20);
    p.required_free_bytes = need + margin;
    p.enough_space = free_bytes >= p.required_free_bytes;
    return p;
}

namespace {
std::array<uint32_t, 256> make_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
}  // namespace

void Crc32::update(const void* data, size_t size) {
    static const std::array<uint32_t, 256> table = make_table();
    const auto* p = static_cast<const unsigned char*>(data);
    uint32_t s = state_;
    for (size_t i = 0; i < size; ++i) s = table[(s ^ p[i]) & 0xFF] ^ (s >> 8);
    state_ = s;
}

uint32_t crc32(const void* data, size_t size) {
    Crc32 c;
    c.update(data, size);
    return c.value();
}

std::string path_join(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    std::string r(a);
    if (r.back() != '/') r.push_back('/');
    r.append(b.begin() + (b.front() == '/' ? 1 : 0), b.end());
    return r;
}

std::string SandboxLayout::game_root(std::string_view documents) const { return path_join(documents, game_dir_name); }
std::string SandboxLayout::saves_dir(std::string_view app_support) const { return path_join(path_join(app_support, support_dir_name), "saves"); }
std::string SandboxLayout::shader_cache_dir(std::string_view caches) const { return path_join(path_join(caches, support_dir_name), "shaders"); }
std::string SandboxLayout::import_staging_dir(std::string_view tmp) const { return path_join(path_join(tmp, support_dir_name), "import"); }

}  // namespace nue::ios
