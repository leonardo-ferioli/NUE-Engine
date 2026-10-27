#include "nue_legacy/win32/process.hpp"
#include "nue_legacy/platform/storage_path.hpp"
#include <algorithm>

namespace nue::legacy::win32 {
namespace {
thread_local uint32_t t_last_error = 0;

struct SnapshotObject : Object {
    SnapshotObject() : Object(ObjectKind::other) {}
    std::vector<ProcessEntry> entries;
    size_t next = 0;
};
}  // namespace

uint32_t ProcessApi::get_last_error() { return t_last_error; }
void ProcessApi::set_last_error(uint32_t e) { t_last_error = e; }

ProcessApi::ProcessApi(platform::System& system, HandleTable& handles, ProcessConfig config) : system_(system), handles_(handles), config_(std::move(config)) {
    const auto info = system_.info();
    const uint32_t cpus = std::min<uint32_t>(std::max<uint32_t>(info.logical_processors, 1), 32);
    affinity_ = cpus >= 32 ? 0xFFFFFFFFull : (1ull << cpus) - 1;
    // The main image is the module with an empty name; its handle is its base address.
    Module m;
    m.name = config_.exe_path.substr(config_.exe_path.find_last_of('\\') + 1);
    m.handle = config_.image_base;
    m.base = config_.image_base;
    m.references = 1;
    modules_.push_back(std::move(m));
}

std::string ProcessApi::folded(std::string_view s) { return platform::path::fold(s); }
std::string ProcessApi::stem(std::string_view s) {
    std::string f = folded(s);
    const size_t slash = f.find_last_of("\\/");
    if (slash != std::string::npos) f.erase(0, slash + 1);
    if (f.size() > 4 && f.compare(f.size() - 4, 4, ".dll") == 0) f.erase(f.size() - 4);
    return f;
}

ProcessApi::Module* ProcessApi::find(std::string_view name) {
    const std::string wanted = stem(name);
    for (auto& m : modules_) if (stem(m.name) == wanted) return &m;
    return nullptr;
}
ProcessApi::Module* ProcessApi::by_handle(Handle h) {
    for (auto& m : modules_) if (m.handle == h) return &m;
    return nullptr;
}

Handle ProcessApi::register_module(std::string_view name, uint32_t base, std::map<std::string, uint32_t> exports) {
    if (Module* existing = find(name)) { existing->exports.insert(exports.begin(), exports.end()); return existing->handle; }
    Module m;
    m.name = std::string(name);
    m.base = m.handle = base;
    m.exports = std::move(exports);
    modules_.push_back(std::move(m));
    return base;
}

void ProcessApi::register_ordinal(Handle module, uint16_t ordinal, uint32_t address) { if (Module* m = by_handle(module)) m->ordinals[ordinal] = address; }

Ret<Handle> ProcessApi::get_module_handle(std::string_view name) {
    if (name.empty()) return ok(Handle(config_.image_base));
    Module* m = find(name);
    if (!m) return fail<Handle>(err::mod_not_found);
    return ok(m->handle);
}

Ret<Handle> ProcessApi::load_library(std::string_view name) {
    if (name.empty()) return fail<Handle>(err::invalid_parameter);
    Module* m = find(name);
    if (!m) return fail<Handle>(err::mod_not_found);
    ++m->references;
    return ok(m->handle);
}

Ret<bool> ProcessApi::free_library(Handle h) {
    Module* m = by_handle(h);
    if (!m || m->references <= 0) return fail<bool>(err::invalid_handle);
    --m->references;
    return ok(true);
}

Ret<uint32_t> ProcessApi::get_proc_address(Handle module, std::string_view name) {
    Module* m = by_handle(module);
    if (!m) return fail<uint32_t>(err::invalid_handle);
    auto it = m->exports.find(std::string(name));
    if (it == m->exports.end()) {
        if (proc_resolver_) { if (const uint32_t address = proc_resolver_(stem(folded(m->name)), name)) return ok(address); }
        return fail<uint32_t>(err::proc_not_found);
    }
    return ok(it->second);
}
Ret<uint32_t> ProcessApi::get_proc_address_ordinal(Handle module, uint16_t ordinal) {
    Module* m = by_handle(module);
    if (!m) return fail<uint32_t>(err::invalid_handle);
    auto it = m->ordinals.find(ordinal);
    if (it == m->ordinals.end()) return fail<uint32_t>(err::proc_not_found);
    return ok(it->second);
}

Ret<std::string> ProcessApi::get_module_file_name(Handle module) {
    if (module == 0 || module == config_.image_base) return ok(config_.exe_path);
    Module* m = by_handle(module);
    if (!m) return fail<std::string>(err::mod_not_found);
    std::string name = m->name;
    if (name.find('.') == std::string::npos) name += ".dll";
    return ok("C:\\WINDOWS\\system32\\" + name);
}

std::string ProcessApi::environment_block() const {
    std::string block;
    for (const auto& e : config_.environment) { block += e; block.push_back('\0'); }
    block.push_back('\0');
    return block;
}

Ret<bool> ProcessApi::set_environment_variable(std::string_view name, std::string_view value, bool remove) {
    if (name.empty() || name.find('=') != std::string_view::npos) return fail<bool>(err::invalid_parameter);
    const std::string key = folded(name);
    for (auto it = config_.environment.begin(); it != config_.environment.end(); ++it) {
        const size_t eq = it->find('=');
        if (eq != std::string::npos && folded(std::string_view(*it).substr(0, eq)) == key) {
            if (remove) config_.environment.erase(it); else *it = std::string(name) + "=" + std::string(value);
            return ok(true);
        }
    }
    if (remove) return ok(true);                       // removing a variable that is not there succeeds, as it does on Windows (observed)
    config_.environment.push_back(std::string(name) + "=" + std::string(value));
    return ok(true);
}

Ret<std::string> ProcessApi::get_environment_variable(std::string_view name) const {
    const std::string key = folded(name);
    for (const auto& e : config_.environment) {
        const size_t eq = e.find('=');
        if (eq != std::string::npos && folded(std::string_view(e).substr(0, eq)) == key) return ok(e.substr(eq + 1));
    }
    return fail<std::string>(err::envvar_not_found);
}

SystemInfo32 ProcessApi::get_system_info() const {
    const auto info = system_.info();
    SystemInfo32 s;
    s.page_size = info.page_size ? info.page_size : 4096;
    s.allocation_granularity = info.allocation_granularity ? info.allocation_granularity : 65536;
    s.number_of_processors = std::min<uint32_t>(std::max<uint32_t>(info.logical_processors, 1), 32);
    s.active_processor_mask = s.number_of_processors >= 32 ? 0xFFFFFFFFu : (1u << s.number_of_processors) - 1;
    return s;
}

bool ProcessApi::is_processor_feature_present(uint32_t feature) const { return feature < 64 && ((system_.info().pf_mask >> feature) & 1); }

void ProcessApi::output_debug_string(std::string_view text) {
    ++debug_strings_;
    if (debug_sink_) debug_sink_(text);
}

bool ProcessApi::set_priority_class(uint32_t value) {
    switch (value) {
    case 0x20: case 0x40: case 0x80: case 0x100: case 0x4000: case 0x8000: priority_class_ = value; return true;
    default: return false;
    }
}

Ret<bool> ProcessApi::get_process_affinity_mask(uint64_t& process, uint64_t& system) const { process = affinity_; system = get_system_info().active_processor_mask; return ok(true); }
Ret<bool> ProcessApi::set_process_affinity_mask(uint64_t mask) {
    const uint64_t sys = get_system_info().active_processor_mask;
    if (mask == 0 || (mask & ~sys) != 0) return fail<bool>(err::invalid_parameter);
    affinity_ = mask;
    return ok(true);
}

Ret<Handle> ProcessApi::create_toolhelp_snapshot(uint32_t flags) {
    if (!(flags & 0x2)) return fail<Handle>(err::invalid_parameter, kInvalidHandle);        // only TH32CS_SNAPPROCESS is modelled
    auto s = std::make_shared<SnapshotObject>();
    ProcessEntry e;
    e.process_id = config_.process_id;
    e.exe_file = config_.exe_path.substr(config_.exe_path.find_last_of('\\') + 1);
    s->entries.push_back(e);
    const Handle h = handles_.add(s);
    return h ? ok(h) : fail<Handle>(err::not_enough_memory, kInvalidHandle);
}

Ret<bool> ProcessApi::process32_first(Handle snapshot, ProcessEntry& out) {
    auto o = handles_.get(snapshot);
    auto s = o ? std::dynamic_pointer_cast<SnapshotObject>(o) : nullptr;
    if (!s) return fail<bool>(err::invalid_handle);
    s->next = 0;
    return process32_next(snapshot, out);
}
Ret<bool> ProcessApi::process32_next(Handle snapshot, ProcessEntry& out) {
    auto o = handles_.get(snapshot);
    auto s = o ? std::dynamic_pointer_cast<SnapshotObject>(o) : nullptr;
    if (!s) return fail<bool>(err::invalid_handle);
    if (s->next >= s->entries.size()) return fail<bool>(err::no_more_files);
    out = s->entries[s->next++];
    return ok(true);
}

void ProcessApi::exit_process(uint32_t code) { if (exit_handler_) exit_handler_(code); }
Ret<bool> ProcessApi::terminate_process(Handle process, uint32_t code) {
    if (process != kCurrentProcessPseudoHandle) return fail<bool>(err::access_denied);
    exit_process(code);
    return ok(true);
}

}  // namespace nue::legacy::win32
