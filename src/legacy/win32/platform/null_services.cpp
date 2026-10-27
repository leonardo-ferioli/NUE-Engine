#include "nue_legacy/platform/null_services.hpp"
#include "nue_legacy/platform/storage_path.hpp"
#include <algorithm>
#include <cstring>

namespace nue::legacy::platform::null {

// ---------------------------------------------------------------------------------------------------------------------------------- storage
struct MemoryStorage::Impl {
    struct Node {
        bool directory = false;
        std::string name;
        std::string data;
        std::map<std::string, std::shared_ptr<Node>> children;   // key: folded name
        int64_t created = 0, modified = 0;
        bool read_only = false;
    };
    struct Mount { std::string root; std::shared_ptr<Node> top; bool read_only; };
    struct Open { std::shared_ptr<Node> node; uint64_t position = 0; bool append = false, can_write = false; };

    std::mutex m;
    std::vector<Mount> mounts;
    std::map<uint32_t, Open> files;
    uint32_t next = 1;
    int64_t clock = 1000000000000ll;   // ms; advances at every change

    std::shared_ptr<Node> make(bool directory, const std::string& name) {
        auto n = std::make_shared<Node>();
        n->directory = directory;
        n->name = name;
        n->created = n->modified = ++clock;
        return n;
    }
    // Walks `segments` below `top`; the node when it exists, null when not.
    static std::shared_ptr<Node> walk(std::shared_ptr<Node> top, const std::vector<std::string>& segments) {
        auto node = top;
        for (const auto& s : segments) {
            if (!node->directory) return nullptr;
            auto it = node->children.find(path::fold(s));
            if (it == node->children.end()) return nullptr;
            node = it->second;
        }
        return node;
    }
    struct Found { std::shared_ptr<Node> node; Mount* mount = nullptr; std::vector<std::string> segments; };
    // Finds the entry in the first mount that has it.
    Status find(std::string_view p, Found& out) {
        const auto split = path::split_root(p);
        if (split.native) return Status::not_found;
        if (!path::normalize_relative(split.rest, out.segments)) return Status::invalid_argument;
        bool any = false;
        for (auto& mount : mounts) {
            if (mount.root != split.root) continue;
            any = true;
            if (auto n = walk(mount.top, out.segments)) { out.node = n; out.mount = &mount; return Status::ok; }
        }
        return any ? Status::not_found : Status::not_found;
    }
    // The first writable mount of the path's root.
    Mount* writable(std::string_view p, std::vector<std::string>& segments) {
        const auto split = path::split_root(p);
        if (split.native || !path::normalize_relative(split.rest, segments)) return nullptr;
        for (auto& mount : mounts) if (mount.root == split.root && !mount.read_only) return &mount;
        return nullptr;
    }
    // Creates the directories of `segments` (all but `skip_last` of them) below a mount.
    std::shared_ptr<Node> ensure_dirs(Mount& mount, const std::vector<std::string>& segments, size_t count) {
        auto node = mount.top;
        for (size_t i = 0; i < count; ++i) {
            auto it = node->children.find(path::fold(segments[i]));
            if (it == node->children.end()) { auto d = make(true, segments[i]); node->children[path::fold(segments[i])] = d; node->modified = ++clock; node = d; }
            else if (!it->second->directory) return nullptr;
            else node = it->second;
        }
        return node;
    }
    static void fill(const Node& n, StatInfo& s) {
        s = StatInfo{};
        s.kind = n.directory ? EntryKind::directory : EntryKind::file;
        s.size = n.directory ? 0 : n.data.size();
        s.modified_unix_ms = n.modified; s.created_unix_ms = n.created; s.accessed_unix_ms = n.modified;
        s.read_only = n.read_only;
    }
};

MemoryStorage::MemoryStorage() : impl_(std::make_unique<Impl>()) {}
MemoryStorage::~MemoryStorage() = default;

Status MemoryStorage::mount(const MountDesc& d) {
    std::lock_guard lock(impl_->m);
    Impl::Mount m{path::fold(d.root), impl_->make(true, ""), d.read_only};
    if (d.append) impl_->mounts.push_back(std::move(m)); else impl_->mounts.insert(impl_->mounts.begin(), std::move(m));
    return Status::ok;
}
bool MemoryStorage::has_root(std::string_view root) {
    std::lock_guard lock(impl_->m);
    const std::string r = path::fold(root);
    for (auto& m : impl_->mounts) if (m.root == r) return true;
    return false;
}
bool MemoryStorage::normalize(std::string_view p, std::string& out) {
    const auto split = path::split_root(p);
    std::vector<std::string> segments;
    if (!path::normalize_relative(split.rest, segments)) return false;
    out = split.native ? path::join(segments) : split.root + ":" + path::join(segments);
    return true;
}
Result<std::string> MemoryStorage::resolve(std::string_view p, bool for_write) {
    std::lock_guard lock(impl_->m);
    Impl::Found f;
    const Status s = impl_->find(p, f);
    if (s == Status::ok) return {Status::ok, path::split_root(p).root + ":" + path::join(f.segments)};
    if (for_write) { std::vector<std::string> seg; if (impl_->writable(p, seg)) return {Status::not_found, path::split_root(p).root + ":" + path::join(seg)}; }
    return {s, {}};
}
Result<FileHandle> MemoryStorage::open(std::string_view p, uint32_t flags) {
    std::lock_guard lock(impl_->m);
    const bool want_write = flags & (open_flags::write | open_flags::append);
    Impl::Found f;
    const Status found = impl_->find(p, f);
    std::shared_ptr<Impl::Node> node;
    if (found == Status::ok) {
        if (f.node->directory) return {Status::invalid_argument, {}};
        if ((flags & open_flags::create) && (flags & open_flags::exclusive)) return {Status::already_exists, {}};
        if (want_write && (f.mount->read_only || f.node->read_only)) return {Status::invalid_argument, {}};
        node = f.node;
        if (flags & open_flags::truncate) { node->data.clear(); node->modified = ++impl_->clock; }
    } else {
        if (!(flags & open_flags::create)) return {Status::not_found, {}};
        std::vector<std::string> seg;
        auto* mount = impl_->writable(p, seg);
        if (!mount || seg.empty()) return {Status::invalid_argument, {}};
        auto parent = impl_->ensure_dirs(*mount, seg, seg.size() - 1);
        if (!parent) return {Status::invalid_argument, {}};
        node = impl_->make(false, seg.back());
        parent->children[path::fold(seg.back())] = node;
        parent->modified = ++impl_->clock;
    }
    Impl::Open o;
    o.node = node; o.append = flags & open_flags::append; o.can_write = want_write;
    if (o.append) o.position = node->data.size();
    const uint32_t id = impl_->next++;
    impl_->files[id] = o;
    return {Status::ok, FileHandle{id}};
}
Status MemoryStorage::close(FileHandle h) { std::lock_guard lock(impl_->m); return impl_->files.erase(h.value) ? Status::ok : Status::invalid_argument; }

#define MEMORY_FILE(h) std::lock_guard lock(impl_->m); auto it = impl_->files.find(h.value); if (it == impl_->files.end()) return Status::invalid_argument; auto& o = it->second;
Status MemoryStorage::read(FileHandle h, MutableBytes out, size_t& read) {
    read = 0; MEMORY_FILE(h)
    const auto& d = o.node->data;
    if (o.position < d.size()) read = std::min<size_t>(out.size(), d.size() - size_t(o.position));
    if (read) std::memcpy(out.data(), d.data() + o.position, read);
    o.position += read;
    return Status::ok;
}
Status MemoryStorage::read_at(FileHandle h, uint64_t offset, MutableBytes out, size_t& read) {
    read = 0; MEMORY_FILE(h)
    const auto& d = o.node->data;
    if (offset < d.size()) read = std::min<size_t>(out.size(), d.size() - size_t(offset));
    if (read) std::memcpy(out.data(), d.data() + offset, read);
    return Status::ok;
}
static void store(std::string& data, uint64_t offset, Bytes bytes) {
    if (data.size() < offset + bytes.size()) data.resize(size_t(offset + bytes.size()), '\0');
    if (!bytes.empty()) std::memcpy(data.data() + offset, bytes.data(), bytes.size());
}
Status MemoryStorage::write(FileHandle h, Bytes bytes, size_t& written) {
    written = 0; MEMORY_FILE(h)
    if (!o.can_write) return Status::invalid_argument;
    if (o.append) o.position = o.node->data.size();
    store(o.node->data, o.position, bytes);
    o.position += bytes.size(); written = bytes.size();
    o.node->modified = ++impl_->clock;
    return Status::ok;
}
Status MemoryStorage::write_at(FileHandle h, uint64_t offset, Bytes bytes, size_t& written) {
    written = 0; MEMORY_FILE(h)
    if (!o.can_write) return Status::invalid_argument;
    store(o.node->data, offset, bytes);
    written = bytes.size();
    o.node->modified = ++impl_->clock;
    return Status::ok;
}
Status MemoryStorage::seek(FileHandle h, int64_t offset, SeekOrigin origin, uint64_t& position) {
    MEMORY_FILE(h)
    const int64_t base = origin == SeekOrigin::set ? 0 : origin == SeekOrigin::current ? int64_t(o.position) : int64_t(o.node->data.size());
    if (base + offset < 0) return Status::invalid_argument;
    o.position = uint64_t(base + offset);
    position = o.position;
    return Status::ok;
}
Status MemoryStorage::size(FileHandle h, uint64_t& out) { MEMORY_FILE(h) out = o.node->data.size(); return Status::ok; }
Status MemoryStorage::set_size(FileHandle h, uint64_t size) {
    MEMORY_FILE(h)
    if (!o.can_write) return Status::invalid_argument;
    o.node->data.resize(size_t(size), '\0');
    o.node->modified = ++impl_->clock;
    return Status::ok;
}
Status MemoryStorage::flush(FileHandle h) { MEMORY_FILE(h) (void)o; return Status::ok; }
#undef MEMORY_FILE

Status MemoryStorage::stat(std::string_view p, StatInfo& out) {
    std::lock_guard lock(impl_->m);
    Impl::Found f;
    const Status s = impl_->find(p, f);
    if (s == Status::ok) Impl::fill(*f.node, out);
    return s;
}
Status MemoryStorage::create_directory(std::string_view p) {
    std::lock_guard lock(impl_->m);
    std::vector<std::string> seg;
    auto* mount = impl_->writable(p, seg);
    if (!mount) return Status::invalid_argument;
    return impl_->ensure_dirs(*mount, seg, seg.size()) ? Status::ok : Status::invalid_argument;
}
Status MemoryStorage::remove(std::string_view p) {
    std::lock_guard lock(impl_->m);
    Impl::Found f;
    Status s = impl_->find(p, f);
    if (s != Status::ok) return s;
    if (f.mount->read_only || f.segments.empty()) return Status::invalid_argument;
    if (f.node->directory && !f.node->children.empty()) return Status::invalid_argument;
    std::vector<std::string> parent_path(f.segments.begin(), f.segments.end() - 1);
    auto parent = Impl::walk(f.mount->top, parent_path);
    parent->children.erase(path::fold(f.segments.back()));
    parent->modified = ++impl_->clock;
    return Status::ok;
}
Status MemoryStorage::rename(std::string_view from, std::string_view to) {
    std::lock_guard lock(impl_->m);
    Impl::Found f;
    Status s = impl_->find(from, f);
    if (s != Status::ok) return s;
    std::vector<std::string> dst;
    auto* mount = impl_->writable(to, dst);
    if (!mount || mount != f.mount || dst.empty() || f.mount->read_only) return Status::invalid_argument;
    auto dst_parent = impl_->ensure_dirs(*mount, dst, dst.size() - 1);
    if (!dst_parent) return Status::invalid_argument;
    std::vector<std::string> src_parent_path(f.segments.begin(), f.segments.end() - 1);
    auto src_parent = Impl::walk(f.mount->top, src_parent_path);
    auto node = f.node;
    src_parent->children.erase(path::fold(f.segments.back()));
    node->name = dst.back();
    dst_parent->children[path::fold(dst.back())] = node;
    node->modified = ++impl_->clock;
    return Status::ok;
}
Status MemoryStorage::list(std::string_view p, std::vector<DirEntry>& out) {
    std::lock_guard lock(impl_->m);
    out.clear();
    const auto split = path::split_root(p);
    std::vector<std::string> seg;
    if (split.native || !path::normalize_relative(split.rest, seg)) return Status::invalid_argument;
    bool found = false;
    std::set<std::string> seen;
    for (auto& mount : impl_->mounts) {
        if (mount.root != split.root) continue;
        auto dir = Impl::walk(mount.top, seg);
        if (!dir || !dir->directory) continue;
        found = true;
        for (const auto& [key, child] : dir->children) {
            if (!seen.insert(key).second) continue;
            DirEntry e;
            e.name = child->name;
            Impl::fill(*child, e.info);
            out.push_back(std::move(e));
        }
    }
    return found ? Status::ok : Status::not_found;
}
void MemoryStorage::put_file(std::string_view p, std::string_view content) {
    auto f = open(p, open_flags::write | open_flags::create | open_flags::truncate);
    size_t n = 0;
    write(f.value, Bytes(reinterpret_cast<const std::byte*>(content.data()), content.size()), n);
    close(f.value);
}
std::string MemoryStorage::get_file(std::string_view p) {
    auto f = open(p, open_flags::read);
    if (!f) return {};
    std::string s;
    char buffer[1024];
    for (;;) { size_t n = 0; read(f.value, MutableBytes(reinterpret_cast<std::byte*>(buffer), sizeof buffer), n); if (!n) break; s.append(buffer, n); }
    close(f.value);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------- system
NullSystem::NullSystem() {
    value.logical_processors = 4;
    value.total_memory = 8ull << 30; value.available_memory = 4ull << 30;
    value.total_virtual = value.available_virtual = 2ull << 30;
    value.cpu_vendor = "NullCPU"; value.cpu_name = "Null processor"; value.os_name = "Null OS";
}
SystemInfo NullSystem::info() { return value; }

// ---------------------------------------------------------------------------------------------------------------------------------- input
NullInput::NullInput() = default;
void NullInput::deliver(const InputEvent& e) {
    switch (e.kind) {
    case InputEventKind::key: if (e.code < kScancodeCount) keys_[e.code] = e.value ? 1 : 0; break;
    case InputEventKind::mouse_button: if (e.code >= 1 && e.code <= 5) { const uint32_t bit = 1u << (e.code - 1); buttons_ = e.value ? (buttons_ | bit) : (buttons_ & ~bit); } break;
    case InputEventKind::mouse_motion: mouse_x_ += float(e.value); mouse_y_ += float(e.value2); rel_x_ += float(e.value); rel_y_ += float(e.value2); break;
    case InputEventKind::pad_axis: if (e.code < 8) pad_states_[e.pad].axes[e.code] = int16_t(e.value); break;
    case InputEventKind::pad_button: if (e.code < 32) { auto& b = pad_states_[e.pad].buttons; b = e.value ? (b | (1u << e.code)) : (b & ~(1u << e.code)); } break;
    case InputEventKind::pad_hat: if (e.code < 4) pad_states_[e.pad].hats[e.code] = uint8_t(e.value); break;
    default: break;
    }
}
void NullInput::pump() {
    std::vector<InputEvent> events;
    std::map<uint32_t, Listener> listeners;
    { std::lock_guard lock(m_); events.swap(queue_); listeners = listeners_; }
    for (const auto& e : events) {
        deliver(e);
        for (const auto& [id, fn] : listeners) fn(e);
    }
}
bool NullInput::gamepad_state(GamepadId id, GamepadState& out) {
    std::lock_guard lock(m_);
    for (const auto& p : pads_) if (p.id == id && p.connected) { out = pad_states_[id]; return true; }
    return false;
}
uint32_t NullInput::add_listener(Listener l) { std::lock_guard lock(m_); listeners_[next_listener_] = std::move(l); return next_listener_++; }
void NullInput::remove_listener(uint32_t id) { std::lock_guard lock(m_); listeners_.erase(id); }
void NullInput::push(const InputEvent& e) { std::lock_guard lock(m_); queue_.push_back(e); }
void NullInput::key(uint32_t sc, bool down, int64_t t) { std::lock_guard lock(m_); InputEvent e; e.kind = InputEventKind::key; e.code = sc; e.value = down; e.time_ns = t; queue_.push_back(e); }
void NullInput::mouse_button(uint32_t b, bool down, int64_t t) { std::lock_guard lock(m_); InputEvent e; e.kind = InputEventKind::mouse_button; e.code = b; e.value = down; e.time_ns = t; queue_.push_back(e); }
void NullInput::mouse_move(int32_t dx, int32_t dy, int64_t t) { std::lock_guard lock(m_); InputEvent e; e.kind = InputEventKind::mouse_motion; e.value = dx; e.value2 = dy; e.time_ns = t; queue_.push_back(e); }
void NullInput::mouse_wheel(int32_t notches, int64_t t) { std::lock_guard lock(m_); InputEvent e; e.kind = InputEventKind::mouse_wheel; e.value = notches * 120; e.time_ns = t; queue_.push_back(e); }
GamepadId NullInput::add_pad(const GamepadInfo& info) {
    std::lock_guard lock(m_);
    GamepadInfo p = info; p.id = next_pad_++; p.connected = true;
    pads_.push_back(p);
    InputEvent e; e.kind = InputEventKind::pad_added; e.pad = p.id; queue_.push_back(e);
    return p.id;
}
void NullInput::pad_axis(GamepadId id, uint32_t axis, int16_t v, int64_t t) { std::lock_guard lock(m_); InputEvent e; e.kind = InputEventKind::pad_axis; e.pad = id; e.code = axis; e.value = v; e.time_ns = t; queue_.push_back(e); }
void NullInput::pad_button(GamepadId id, uint32_t b, bool down, int64_t t) { std::lock_guard lock(m_); InputEvent e; e.kind = InputEventKind::pad_button; e.pad = id; e.code = b; e.value = down; e.time_ns = t; queue_.push_back(e); }
void NullInput::pad_hat(GamepadId id, uint32_t hat, uint8_t mask, int64_t t) { std::lock_guard lock(m_); InputEvent e; e.kind = InputEventKind::pad_hat; e.pad = id; e.code = hat; e.value = mask; e.time_ns = t; queue_.push_back(e); }
void NullInput::remove_pad(GamepadId id, int64_t t) {
    std::lock_guard lock(m_);
    for (auto& p : pads_) if (p.id == id) p.connected = false;
    InputEvent e; e.kind = InputEventKind::pad_removed; e.pad = id; e.time_ns = t; queue_.push_back(e);
}

}  // namespace nue::legacy::platform::null
