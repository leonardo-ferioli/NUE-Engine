#include "nue_legacy/platform/std_platform.hpp"
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nue::legacy::platform::stdlib {
namespace {

class MapConfig final : public Config {
public:
    explicit MapConfig(Storage* storage) : storage_(storage) {}
    bool has(std::string_view key) override { std::lock_guard lock(m_); return values_.count(std::string(key)) != 0; }
    Status get_string(std::string_view key, std::string& out) override {
        std::lock_guard lock(m_);
        auto it = values_.find(std::string(key));
        if (it == values_.end()) return Status::not_found;
        out = it->second;
        return Status::ok;
    }
    Status set_string(std::string_view key, std::string_view value) override {
        if (key.empty() || key.find('=') != std::string_view::npos || key.find('\n') != std::string_view::npos) return Status::invalid_argument;
        std::lock_guard lock(m_);
        values_[std::string(key)] = std::string(value);
        return Status::ok;
    }
    Status remove(std::string_view key) override { std::lock_guard lock(m_); return values_.erase(std::string(key)) ? Status::ok : Status::not_found; }
    std::vector<std::string> keys_with_prefix(std::string_view prefix) override {
        std::lock_guard lock(m_);
        std::vector<std::string> out;
        for (auto it = values_.lower_bound(std::string(prefix)); it != values_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) out.push_back(it->first);
        return out;
    }
    Status load_text(std::string_view text) override {
        std::lock_guard lock(m_);
        std::istringstream in{std::string(text)};
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            values_[line.substr(0, eq)] = unescape(line.substr(eq + 1));
        }
        return Status::ok;
    }
    std::string save_text() override {
        std::lock_guard lock(m_);
        std::string out;
        for (const auto& [k, v] : values_) { out += k; out += '='; out += escape(v); out += '\n'; }
        return out;
    }
    Status load(std::string_view path) override {
        if (!storage_) return Status::unsupported;
        auto file = storage_->open(path, open_flags::read);
        if (!file) return file.status;
        std::string text;
        char buffer[4096];
        for (;;) {
            size_t n = 0;
            storage_->read(file.value, MutableBytes(reinterpret_cast<std::byte*>(buffer), sizeof buffer), n);
            if (!n) break;
            text.append(buffer, n);
        }
        storage_->close(file.value);
        return load_text(text);
    }
    Status save(std::string_view path) override {
        if (!storage_) return Status::unsupported;
        const std::string text = save_text();
        auto file = storage_->open(path, open_flags::write | open_flags::create | open_flags::truncate);
        if (!file) return file.status;
        size_t n = 0;
        const Status s = storage_->write(file.value, Bytes(reinterpret_cast<const std::byte*>(text.data()), text.size()), n);
        storage_->close(file.value);
        return s;
    }
private:
    static std::string escape(const std::string& v) {   // values may hold newlines (REG_MULTI_SZ): '\' and line breaks are escaped
        std::string out;
        for (char c : v) { if (c == '\\') out += "\\\\"; else if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r"; else out += c; }
        return out;
    }
    static std::string unescape(const std::string& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size()) { ++i; out += v[i] == 'n' ? '\n' : v[i] == 'r' ? '\r' : v[i]; }
            else out += v[i];
        }
        return out;
    }
    std::mutex m_;
    std::map<std::string, std::string> values_;
    Storage* storage_;
};

class StdSystem final : public System {
public:
    SystemInfo info() override {
        SystemInfo i;
        i.logical_processors = std::max(1u, std::thread::hardware_concurrency());
#if defined(_WIN32)
        SYSTEM_INFO s{};
        GetNativeSystemInfo(&s);
        i.page_size = s.dwPageSize;
        i.allocation_granularity = s.dwAllocationGranularity;
        MEMORYSTATUSEX m{sizeof m};
        if (GlobalMemoryStatusEx(&m)) { i.total_memory = m.ullTotalPhys; i.available_memory = m.ullAvailPhys; i.total_virtual = m.ullTotalVirtual; i.available_virtual = m.ullAvailVirtual; }
        i.sse3 = IsProcessorFeaturePresent(PF_SSE3_INSTRUCTIONS_AVAILABLE);
        i.pf_mask = 0;
        for (unsigned f = 0; f < 64; ++f) if (IsProcessorFeaturePresent(f)) i.pf_mask |= 1ull << f;
        i.os_name = "Windows";
        i.ansi_code_page = GetACP(); i.oem_code_page = GetOEMCP(); i.user_lcid = GetUserDefaultLCID();
#else
        i.total_memory = i.available_memory = 4ull << 30;
        i.total_virtual = i.available_virtual = 4ull << 30;
        i.os_name = "Unix";
#endif
        return i;
    }
};
}  // namespace

std::unique_ptr<Config> make_config(Storage* storage) { return std::make_unique<MapConfig>(storage); }
std::unique_ptr<System> make_system() { return std::make_unique<StdSystem>(); }
}  // namespace nue::legacy::platform::stdlib
