// The worker-thread mode of the dll: see threaded.hpp.
#include "threaded.hpp"
#include "facade.hpp"
#include "layer/d3d9_consts.hpp"
#include "layer/formats.hpp"
#include <algorithm>
#include <cstring>

namespace nue::facade {
using namespace d3d9::k;
using d3d9::TextureId; using d3d9::BufferId; using d3d9::SurfaceId; using d3d9::DeclarationId; using d3d9::ShaderId; using d3d9::StateBlockId;

namespace {

constexpr uint32_t kBlockBytes = 1u << 20;     // the queue is a chain of 1 MB blocks, reused
constexpr uint32_t kInlineLimit = 64u << 10;   // larger payloads travel as a heap block the worker frees
constexpr uint32_t kPublishEvery = 32;         // the worker sees new commands every 32 calls (and at once at Present and when the game waits)
constexpr uint32_t kWakeEvery = 2048;          // a sleeping worker is woken every 2048 calls (and at Present, when the game waits, at a new block):
                                               // waking it is a system call, and the worker usually drains a batch faster than the game records it

enum class Op : uint16_t {
    fn, sync, stop, present, reset,
    create_texture, create_cube, create_vb, create_ib, create_rt, create_ds, create_decl, create_vs, create_ps, texture_surface,
    destroy, buffer_write, texture_write, texture_unlock_direct, stretch_rect, update_texture,
    rs, ss, tss, transform, light_enable, light, material, viewport, texture, render_target, depth_stencil, decl, fvf, vs, ps,
    const_f, const_i, const_b, stream, indices, begin_block, end_block, apply, clear, draw_indexed, draw_up,
    request_texture_report, mark_missing, reset_profile,
};
enum Kind : uint8_t { k_texture, k_buffer, k_surface, k_decl, k_shader, k_block, k_count };

struct Header { Op op; uint16_t pad; uint32_t size; };

struct Block {
    std::unique_ptr<std::byte[]> data{new std::byte[kBlockBytes]};
    std::atomic<uint32_t> published{0};
    std::atomic<Block*> next{nullptr};
};

// one id space per kind on the game thread; ids freed at destroy are used again (the worker sees the destroy before the next creation)
struct ProxyIds {
    uint32_t next = 1;
    std::vector<uint32_t> free;
    uint32_t take() { if (!free.empty()) { const uint32_t id = free.back(); free.pop_back(); return id; } return next++; }
    void give(uint32_t id) { if (id) free.push_back(id); }
};

struct BufferInfo {
    bool alive = false, has_data = false, locked = false, read_only = false;
    uint32_t bytes = 0, lock_offset = 0, lock_size = 0, lock_flags = 0;
    std::vector<std::byte> mirror;               // the buffer as the game sees it (kept once a lock had to show the contents)
    std::unique_ptr<std::byte[]> staging;        // a lock without a mirror: the written range, sent to the worker at unlock
};
struct TextureLock { bool active = false, direct = false, has_rect = false; std::unique_ptr<std::byte[]> staging; uint32_t size = 0, pitch = 0, flags = 0; int32_t rect[4] = {}; };
struct TextureInfo {
    bool alive = false, cube = false, render_target = false;
    uint32_t width = 0, height = 0, levels = 1, format = 0, pool = 0;
    const d3d9::FormatInfo* info = nullptr;
    std::vector<uint8_t> has_data;               // per face * levels + level
    std::vector<TextureLock> locks;
};
struct SurfaceInfo { uint32_t texture = 0, level = 0, face = 0; };

template <class T> T& slot(std::vector<T>& v, uint32_t id) { if (id >= v.size()) v.resize(size_t(id) + 1); return v[id]; }

// Waits on the game thread for `done`, handling the messages other threads send to its windows meanwhile (DXGI on the worker may send some and
// wait for the answer). Posted messages (input) stay in the queue for the game.
template <class F> void wait_game_thread(HANDLE event, F done) {
    while (!done()) {
        const DWORD r = MsgWaitForMultipleObjects(1, &event, FALSE, 50, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0 + 1) { MSG m; PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE); }
    }
}

// a wait outside Present: counted for the log
template <class F> void timed_wait(HANDLE event, F done) {
    LARGE_INTEGER frequency, begin, end;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
    wait_game_thread(event, done);
    QueryPerformanceCounter(&end);
    ++g_worker_waits;
    g_worker_wait_us += uint64_t((end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart);
}

}  // namespace

struct ThreadedLayer::Impl {
    ThreadedLayer& self;
    explicit Impl(ThreadedLayer& s, uint64_t budget) : self(s), copy_budget(budget) {}

    // ---- the queue (single producer: the game thread; single consumer: the worker)
    Block* write_block = nullptr;
    uint32_t write_pos = 0, unpublished = 0;
    std::mutex free_mutex;
    std::vector<Block*> free_blocks;
    std::vector<std::unique_ptr<Block>> all_blocks;
    Block* read_block = nullptr;
    std::atomic<bool> worker_sleeping{false};
    HANDLE work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker;
    bool stopped = false;
    std::atomic<uint64_t> syncs_done{0};
    uint64_t syncs_sent = 0, buffer_copies = 0, texture_reads = 0;   // waits for the worker outside Present, and why (logged at the end)
    std::atomic<uint64_t> presents_done{0};
    uint64_t presents_sent = 0;

    // ---- the game thread's side
    ProxyIds ids[k_count];
    std::vector<BufferInfo> buffers;
    std::vector<TextureInfo> textures;
    std::vector<SurfaceInfo> surfaces;
    uint64_t copy_budget, copies_bytes = 0;
    // the last value sent per render / sampler / stage state: the layer ignores a state set to the value it has, so such a call is not queued.
    // A value is known when its epoch is the current one; applying or recording a state block and Reset forget them all.
    struct Known { uint32_t value = 0, epoch = 0; };
    Known rs_sent[256], ss_sent[16][14], tss_sent[8][33];
    uint32_t epoch = 1;
    bool recording = false;
    bool unchanged(Known& k, uint32_t value) {
        if (recording) return false;
        if (k.epoch == epoch && k.value == value) return true;
        k.value = value; k.epoch = epoch;
        return false;
    }
    void forget_states() { ++epoch; }

    // ---- the worker's side
    std::vector<uint32_t> map[k_count];
    mutable std::mutex snapshot_mutex;
    d3d9::LayerStats layer_stats{};
    gpu::BgfxStats gpu_stats{};
    std::string texture_report, report_text;
    bool texture_report_ready = false;
    uint64_t worker_frames = 0;

    Block* new_block() {
        {
            std::lock_guard<std::mutex> lock(free_mutex);
            if (!free_blocks.empty()) { Block* b = free_blocks.back(); free_blocks.pop_back(); b->published.store(0, std::memory_order_relaxed); b->next.store(nullptr, std::memory_order_relaxed); return b; }
        }
        std::lock_guard<std::mutex> lock(free_mutex);
        all_blocks.push_back(std::make_unique<Block>());
        return all_blocks.back().get();
    }
    uint32_t since_wake = 0;   // calls published since the worker was last woken
    void publish(bool wake) {
        write_block->published.store(write_pos, std::memory_order_seq_cst);
        since_wake += unpublished;
        unpublished = 0;
        if ((wake || since_wake >= kWakeEvery) && worker_sleeping.load(std::memory_order_seq_cst)) { SetEvent(work_event); since_wake = 0; }
    }
    // room for one command: the payload pointer; commit() ends it
    std::byte* begin(Op op, uint32_t payload) {
        const uint32_t need = uint32_t(sizeof(Header)) + ((payload + 7u) & ~7u);
        if (write_pos + need > kBlockBytes) {
            Block* next = new_block();
            write_block->published.store(write_pos, std::memory_order_seq_cst);
            write_block->next.store(next, std::memory_order_seq_cst);
            if (worker_sleeping.load(std::memory_order_seq_cst)) SetEvent(work_event);
            write_block = next; write_pos = 0; unpublished = 0; since_wake = 0;
        }
        std::byte* at = write_block->data.get() + write_pos;
        const Header h{op, 0, need - uint32_t(sizeof(Header))};
        std::memcpy(at, &h, sizeof h);
        write_pos += need;
        return at + sizeof(Header);
    }
    void commit(bool now = false) { ++unpublished; if (now || unpublished >= kPublishEvery) publish(now); }
    template <class T> void put(Op op, const T& value, bool now = false) { std::memcpy(begin(op, sizeof(T)), &value, sizeof(T)); commit(now); }
    template <class T> void put_with(Op op, const T& value, const void* data, uint32_t bytes) {
        std::byte* p = begin(op, uint32_t(sizeof(T)) + bytes);
        std::memcpy(p, &value, sizeof(T));
        if (bytes) std::memcpy(p + sizeof(T), data, bytes);
        commit();
    }

    // ---- the worker
    uint32_t real(Kind k, uint32_t proxy) const { return proxy && proxy < map[k].size() ? map[k][proxy] : 0; }
    void bind(Kind k, uint32_t proxy, uint32_t value) { slot(map[k], proxy) = value; }
    void run_worker();
    bool execute(Op op, const std::byte* p);
    void after_present();
};

namespace {
struct FnCmd { std::function<void()>* fn; };
struct SyncCmd { const std::function<void()>* fn; uint64_t ticket; };
struct ResetCmd { d3d9::DeviceParams params; Status* result; uint64_t ticket; };
struct CreateTextureCmd { uint32_t proxy, width, height, levels, usage, format, pool; };
struct CreateCubeCmd { uint32_t proxy, edge, levels, usage, format, pool; };
struct CreateBufferCmd { uint32_t proxy, bytes, usage, format_or_fvf, pool; };
struct CreateSurfaceCmd { uint32_t proxy, width, height, format, multisample; bool lockable; };
struct CreateDataCmd { uint32_t proxy, bytes; };
struct TextureSurfaceCmd { uint32_t proxy, texture, level, face; };
struct DestroyCmd { uint32_t kind, proxy; };
struct BufferWriteCmd { uint32_t proxy, offset, size, flags; std::byte* heap; };
struct TextureWriteCmd { uint32_t proxy, level, face, flags, pitch, size; bool has_rect; int32_t rect[4]; std::byte* heap; };
struct TextureUnlockCmd { uint32_t proxy, level, face; };
struct StretchCmd { uint32_t from, to, filter; bool has_from, has_to; int32_t from_rect[4], to_rect[4]; };
struct PairCmd { uint32_t a, b; };
struct TripleCmd { uint32_t a, b, c; };
struct TransformCmd { uint32_t type; float m[16]; };
struct LightCmd { uint32_t index; d3d9::LightDesc light; };
struct ViewportCmd { uint32_t r[4]; float min_z, max_z; };
struct ConstCmd { uint32_t pixel, start, count; };
struct StreamCmd { uint32_t stream, proxy, offset, stride; };
struct ClearCmd { uint32_t count, flags, argb; float z; uint32_t stencil; };
struct DrawIndexedCmd { uint32_t primitive; int32_t base_vertex; uint32_t min_vertex, vertex_count, start_index, primitive_count; };
struct DrawUpCmd { uint32_t primitive, primitive_count, stride, bytes; std::byte* heap; };
template <class T> T load(const std::byte* p) { T v; std::memcpy(&v, p, sizeof v); return v; }
}  // namespace

bool ThreadedLayer::Impl::execute(Op op, const std::byte* p) {
    d3d9::Layer& L = *self.real_;
    switch (op) {
    case Op::fn: { std::unique_ptr<std::function<void()>> fn(load<FnCmd>(p).fn); (*fn)(); break; }
    case Op::sync: {   // the game thread waits for the ticket: it is given back even when fn throws
        auto c = load<SyncCmd>(p);
        struct Done { Impl& impl; uint64_t ticket; ~Done() { impl.syncs_done.store(ticket, std::memory_order_release); SetEvent(impl.done_event); } } done{*this, c.ticket};
        (*c.fn)();
        break;
    }
    case Op::stop: return false;
    case Op::present: L.present(); after_present(); break;
    case Op::reset: {
        auto c = load<ResetCmd>(p);
        struct Done { Impl& impl; uint64_t ticket; ~Done() { impl.syncs_done.store(ticket, std::memory_order_release); SetEvent(impl.done_event); } } done{*this, c.ticket};
        *c.result = Status::invalid_argument;
        *c.result = L.reset(c.params);
        break;
    }
    case Op::create_texture: { auto c = load<CreateTextureCmd>(p); auto r = L.create_texture(c.width, c.height, c.levels, c.usage, c.format, c.pool); bind(k_texture, c.proxy, r ? r.value.value : 0); break; }
    case Op::create_cube: { auto c = load<CreateCubeCmd>(p); auto r = L.create_cube_texture(c.edge, c.levels, c.usage, c.format, c.pool); bind(k_texture, c.proxy, r ? r.value.value : 0); break; }
    case Op::create_vb: { auto c = load<CreateBufferCmd>(p); auto r = L.create_vertex_buffer(c.bytes, c.usage, c.format_or_fvf, c.pool); bind(k_buffer, c.proxy, r ? r.value.value : 0); break; }
    case Op::create_ib: { auto c = load<CreateBufferCmd>(p); auto r = L.create_index_buffer(c.bytes, c.usage, c.format_or_fvf, c.pool); bind(k_buffer, c.proxy, r ? r.value.value : 0); break; }
    case Op::create_rt: { auto c = load<CreateSurfaceCmd>(p); auto r = L.create_render_target(c.width, c.height, c.format, c.multisample, c.lockable); bind(k_surface, c.proxy, r ? r.value.value : 0); break; }
    case Op::create_ds: { auto c = load<CreateSurfaceCmd>(p); auto r = L.create_depth_stencil(c.width, c.height, c.format, c.multisample); bind(k_surface, c.proxy, r ? r.value.value : 0); break; }
    case Op::create_decl: { auto c = load<CreateDataCmd>(p); auto r = L.create_vertex_declaration(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p + sizeof c), c.bytes)); bind(k_decl, c.proxy, r ? r.value.value : 0); break; }
    case Op::create_vs:
    case Op::create_ps: {
        auto c = load<CreateDataCmd>(p);
        std::vector<uint32_t> words(c.bytes / 4);
        std::memcpy(words.data(), p + sizeof c, c.bytes);
        auto r = op == Op::create_vs ? L.create_vertex_shader(words) : L.create_pixel_shader(words);
        bind(k_shader, c.proxy, r ? r.value.value : 0);
        break;
    }
    case Op::texture_surface: { auto c = load<TextureSurfaceCmd>(p); auto r = L.texture_surface(TextureId{real(k_texture, c.texture)}, c.level, c.face); bind(k_surface, c.proxy, r ? r.value.value : 0); break; }
    case Op::destroy: {
        auto c = load<DestroyCmd>(p);
        const uint32_t id = real(Kind(c.kind), c.proxy);
        switch (Kind(c.kind)) {
        case k_texture: L.destroy(TextureId{id}); break;
        case k_buffer: L.destroy(BufferId{id}); break;
        case k_surface: L.destroy(SurfaceId{id}); break;
        case k_decl: L.destroy(DeclarationId{id}); break;
        case k_shader: L.destroy(ShaderId{id}); break;
        case k_block: L.destroy(StateBlockId{id}); break;
        default: break;
        }
        bind(Kind(c.kind), c.proxy, 0);
        break;
    }
    case Op::buffer_write: {
        auto c = load<BufferWriteCmd>(p);
        const std::byte* data = c.heap ? c.heap : p + sizeof c;
        const BufferId id{real(k_buffer, c.proxy)};
        auto lock = L.lock_buffer(id, c.offset, c.size, c.flags & ~uint32_t(lock_readonly));
        if (lock) { std::memcpy(lock.value.data, data, c.size); L.unlock_buffer(id); }
        delete[] c.heap;
        break;
    }
    case Op::texture_write: {
        auto c = load<TextureWriteCmd>(p);
        const std::byte* data = c.heap ? c.heap : p + sizeof c;
        const TextureId id{real(k_texture, c.proxy)};
        auto lock = L.lock_texture(id, c.level, c.face, c.has_rect ? c.rect : nullptr, c.flags & ~uint32_t(lock_readonly));
        if (lock) {
            if (lock.value.pitch == c.pitch) std::memcpy(lock.value.data, data, c.size);
            else {
                const uint32_t row = std::min(lock.value.pitch, c.pitch);
                for (uint32_t r = 0; r * size_t(c.pitch) + row <= c.size; ++r) std::memcpy(lock.value.data + size_t(r) * lock.value.pitch, data + size_t(r) * c.pitch, row);
            }
            L.unlock_texture(id, c.level, c.face);
        }
        delete[] c.heap;
        break;
    }
    case Op::texture_unlock_direct: { auto c = load<TextureUnlockCmd>(p); L.unlock_texture(TextureId{real(k_texture, c.proxy)}, c.level, c.face); break; }
    case Op::stretch_rect: { auto c = load<StretchCmd>(p); L.stretch_rect(SurfaceId{real(k_surface, c.from)}, c.has_from ? c.from_rect : nullptr, SurfaceId{real(k_surface, c.to)}, c.has_to ? c.to_rect : nullptr, c.filter); break; }
    case Op::update_texture: { auto c = load<PairCmd>(p); L.update_texture(TextureId{real(k_texture, c.a)}, TextureId{real(k_texture, c.b)}); break; }
    case Op::rs: { auto c = load<PairCmd>(p); L.set_render_state(c.a, c.b); break; }
    case Op::ss: { auto c = load<TripleCmd>(p); L.set_sampler_state(c.a, c.b, c.c); break; }
    case Op::tss: { auto c = load<TripleCmd>(p); L.set_texture_stage_state(c.a, c.b, c.c); break; }
    case Op::transform: { auto c = load<TransformCmd>(p); L.set_transform(c.type, c.m); break; }
    case Op::light_enable: { auto c = load<PairCmd>(p); L.light_enable(c.a, c.b != 0); break; }
    case Op::light: { auto c = load<LightCmd>(p); L.set_light(c.index, c.light); break; }
    case Op::material: { auto c = load<d3d9::MaterialDesc>(p); L.set_material(c); break; }
    case Op::viewport: { auto c = load<ViewportCmd>(p); L.set_viewport(c.r, c.min_z, c.max_z); break; }
    case Op::texture: { auto c = load<PairCmd>(p); L.set_texture(c.a, TextureId{real(k_texture, c.b)}); break; }
    case Op::render_target: { auto c = load<PairCmd>(p); L.set_render_target(c.a, SurfaceId{real(k_surface, c.b)}); break; }
    case Op::depth_stencil: { auto c = load<uint32_t>(p); L.set_depth_stencil(SurfaceId{real(k_surface, c)}); break; }
    case Op::decl: { auto c = load<uint32_t>(p); L.set_vertex_declaration(DeclarationId{real(k_decl, c)}); break; }
    case Op::fvf: { auto c = load<uint32_t>(p); L.set_fvf(c); break; }
    case Op::vs: { auto c = load<uint32_t>(p); L.set_vertex_shader(ShaderId{real(k_shader, c)}); break; }
    case Op::ps: { auto c = load<uint32_t>(p); L.set_pixel_shader(ShaderId{real(k_shader, c)}); break; }
    case Op::const_f: {
        auto c = load<ConstCmd>(p);
        const auto* v = reinterpret_cast<const d3d9::Float4*>(p + sizeof c);
        if (c.pixel) L.set_pixel_shader_constants_f(c.start, std::span<const d3d9::Float4>(v, c.count)); else L.set_vertex_shader_constants_f(c.start, std::span<const d3d9::Float4>(v, c.count));
        break;
    }
    case Op::const_i: {
        auto c = load<ConstCmd>(p);
        const auto* v = reinterpret_cast<const d3d9::Int4*>(p + sizeof c);
        if (c.pixel) L.set_pixel_shader_constants_i(c.start, std::span<const d3d9::Int4>(v, c.count)); else L.set_vertex_shader_constants_i(c.start, std::span<const d3d9::Int4>(v, c.count));
        break;
    }
    case Op::const_b: {
        auto c = load<ConstCmd>(p);
        const auto* v = reinterpret_cast<const uint32_t*>(p + sizeof c);
        if (c.pixel) L.set_pixel_shader_constants_b(c.start, std::span<const uint32_t>(v, c.count)); else L.set_vertex_shader_constants_b(c.start, std::span<const uint32_t>(v, c.count));
        break;
    }
    case Op::stream: { auto c = load<StreamCmd>(p); L.set_stream_source(c.stream, BufferId{real(k_buffer, c.proxy)}, c.offset, c.stride); break; }
    case Op::indices: { auto c = load<uint32_t>(p); L.set_indices(BufferId{real(k_buffer, c)}); break; }
    case Op::begin_block: { auto c = load<uint32_t>(p); bind(k_block, c, L.begin_state_block().value); break; }
    case Op::end_block: { auto c = load<uint32_t>(p); L.end_state_block(StateBlockId{real(k_block, c)}); break; }
    case Op::apply: { auto c = load<uint32_t>(p); L.apply(StateBlockId{real(k_block, c)}); break; }
    case Op::clear: { auto c = load<ClearCmd>(p); L.clear(std::span<const int32_t>(reinterpret_cast<const int32_t*>(p + sizeof c), c.count), c.flags, c.argb, c.z, c.stencil); break; }
    case Op::draw_indexed: { auto c = load<DrawIndexedCmd>(p); L.draw_indexed(c.primitive, c.base_vertex, c.min_vertex, c.vertex_count, c.start_index, c.primitive_count); break; }
    case Op::draw_up: {
        auto c = load<DrawUpCmd>(p);
        const std::byte* data = c.heap ? c.heap : p + sizeof c;
        L.draw_up(c.primitive, c.primitive_count, Bytes(data, c.bytes), c.stride);
        delete[] c.heap;
        break;
    }
    case Op::request_texture_report: L.request_texture_report(); break;
    case Op::mark_missing: L.set_mark_missing_textures(load<uint32_t>(p) != 0); break;
    case Op::reset_profile: L.reset_profile(); break;
    }
    return true;
}

// after each frame the worker leaves what the game thread reads without waiting: the counters, the texture report, the unsupported-features text
void ThreadedLayer::Impl::after_present() {
    ++worker_frames;
    std::string report_now = self.real_->take_texture_report();
    std::lock_guard<std::mutex> lock(snapshot_mutex);
    layer_stats = self.real_->stats();
    if (self.gpu_) gpu_stats = self.gpu_->stats();
    if (!report_now.empty()) { texture_report = std::move(report_now); texture_report_ready = true; }
    if (worker_frames % 60 == 1) report_text = self.real_->report();
}

void ThreadedLayer::Impl::run_worker() {
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    Block* b = read_block;
    uint32_t pos = 0;
    for (;;) {
        const uint32_t published = b->published.load(std::memory_order_acquire);
        if (pos < published) {
            LARGE_INTEGER start; QueryPerformanceCounter(&start);
            bool go = true;
            while (go && pos < published) {
                Header h; std::memcpy(&h, b->data.get() + pos, sizeof h);
                const std::byte* payload = b->data.get() + pos + sizeof h;
                pos += uint32_t(sizeof h) + h.size;
                try { go = execute(h.op, payload); }
                catch (const std::exception& e) { log("worker thread: a call failed with an exception: %s", e.what()); }
                catch (...) { log("worker thread: a call failed with an exception"); }
                if (h.op == Op::present) { presents_done.fetch_add(1, std::memory_order_release); SetEvent(done_event); }
            }
            LARGE_INTEGER end; QueryPerformanceCounter(&end);
            const uint64_t us = uint64_t((end.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
            self.busy_us_.fetch_add(us, std::memory_order_relaxed);
            g_worker_busy_total_us.fetch_add(us, std::memory_order_relaxed);
            if (!go) return;
            continue;
        }
        if (Block* next = b->next.load(std::memory_order_acquire)) {
            if (pos == b->published.load(std::memory_order_acquire)) {
                { std::lock_guard<std::mutex> lock(free_mutex); free_blocks.push_back(b); }
                b = next; pos = 0;
            }
            continue;
        }
        bool found = false;
        for (int i = 0; i < 256 && !found; ++i) { YieldProcessor(); found = b->published.load(std::memory_order_acquire) != pos || b->next.load(std::memory_order_acquire); }
        if (found) continue;
        worker_sleeping.store(true, std::memory_order_seq_cst);
        if (b->published.load(std::memory_order_seq_cst) == pos && !b->next.load(std::memory_order_seq_cst)) WaitForSingleObject(work_event, INFINITE);
        worker_sleeping.store(false, std::memory_order_seq_cst);
    }
}

// ---- ThreadedLayer -----------------------------------------------------------------------------------------------------------------------------

ThreadedLayer::ThreadedLayer(uint64_t copy_budget_bytes) : impl_(std::make_unique<Impl>(*this, copy_budget_bytes)) {
    impl_->write_block = impl_->new_block();
    impl_->read_block = impl_->write_block;
    impl_->worker = std::thread([this] { impl_->run_worker(); });
}

ThreadedLayer::~ThreadedLayer() {
    stop();
    CloseHandle(impl_->work_event);
    CloseHandle(impl_->done_event);
}

void ThreadedLayer::stop() {
    if (impl_->stopped) return;
    log("worker thread: %llu frames; the game thread waited for the worker outside Present %llu times (%llu buffer copies made for locks that read, %llu texture locks of levels that hold data)",
        (unsigned long long)impl_->presents_sent, (unsigned long long)impl_->syncs_sent, (unsigned long long)impl_->buffer_copies, (unsigned long long)impl_->texture_reads);
    impl_->stopped = true;
    impl_->begin(Op::stop, 0);
    impl_->commit(true);
    // the worker may still send a message to the game's window while it finishes: keep answering them
    HANDLE thread = impl_->worker.native_handle();
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjects(1, &thread, FALSE, 50, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) { MSG m; PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE); }
    }
    impl_->worker.join();
}

void ThreadedLayer::sync(const std::function<void()>& fn) {
    if (impl_->stopped) { fn(); return; }
    const uint64_t ticket = ++impl_->syncs_sent;
    impl_->put(Op::sync, SyncCmd{&fn, ticket}, true);
    timed_wait(impl_->done_event, [&] { return impl_->syncs_done.load(std::memory_order_acquire) >= ticket; });
}

void ThreadedLayer::post(std::function<void()> fn) {
    if (impl_->stopped) { fn(); return; }
    impl_->put(Op::fn, FnCmd{new std::function<void()>(std::move(fn))});
}

gpu::BgfxStats ThreadedLayer::gpu_stats() const { std::lock_guard<std::mutex> lock(impl_->snapshot_mutex); return impl_->gpu_stats; }
const d3d9::LayerStats& ThreadedLayer::stats() const { std::lock_guard<std::mutex> lock(impl_->snapshot_mutex); stats_view_ = impl_->layer_stats; return stats_view_; }
void ThreadedLayer::flush() { sync([this] { real_->flush(); }); }
std::string ThreadedLayer::report() const {
    { std::lock_guard<std::mutex> lock(impl_->snapshot_mutex); if (!impl_->report_text.empty()) return impl_->report_text; }
    std::string text;
    const_cast<ThreadedLayer*>(this)->sync([&] { text = real_->report(); });
    return text;
}
std::string ThreadedLayer::audit() const { std::string text; const_cast<ThreadedLayer*>(this)->sync([&] { text = real_->audit(); }); return text; }
void ThreadedLayer::request_texture_report() { impl_->begin(Op::request_texture_report, 0); impl_->commit(); }
std::string ThreadedLayer::take_texture_report() {
    std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
    if (!impl_->texture_report_ready) return {};
    impl_->texture_report_ready = false;
    return std::move(impl_->texture_report);
}
void ThreadedLayer::set_mark_missing_textures(bool on) { mark_missing_ = on; impl_->put(Op::mark_missing, uint32_t(on ? 1 : 0)); }
void ThreadedLayer::reset_profile() { impl_->begin(Op::reset_profile, 0); impl_->commit(); }
std::string ThreadedLayer::profile_report() const { std::string text; const_cast<ThreadedLayer*>(this)->sync([&] { text = real_->profile_report(); }); return text; }
std::string ThreadedLayer::recent_draws() const { std::string text; const_cast<ThreadedLayer*>(this)->sync([&] { text = real_->recent_draws(); }); return text; }

Status ThreadedLayer::reset(const d3d9::DeviceParams& params) {
    impl_->forget_states();
    impl_->recording = false;
    Status result = Status::ok;
    const uint64_t ticket = ++impl_->syncs_sent;
    impl_->put(Op::reset, ResetCmd{params, &result, ticket}, true);
    timed_wait(impl_->done_event, [&] { return impl_->syncs_done.load(std::memory_order_acquire) >= ticket; });
    return result;
}

Status ThreadedLayer::present() {
    impl_->begin(Op::present, 0);
    impl_->commit(true);
    const uint64_t sent = ++impl_->presents_sent;
    // at most one frame ahead: frame N is recorded while the worker draws frame N - 1
    wait_game_thread(impl_->done_event, [&] { return impl_->presents_done.load(std::memory_order_acquire) + 1 >= sent; });
    return Status::ok;
}

// ---- resources

Result<TextureId> ThreadedLayer::create_texture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) {
    const d3d9::FormatInfo& info = d3d9::format_info(format);
    if (!info.valid || !width || !height) {   // the layer refuses it (and notes it): ask it now so that the game gets its answer
        Status status = Status::unsupported;
        sync([&] { auto r = real_->create_texture(width, height, levels, usage, format, pool); status = r ? Status::unsupported : r.status; if (r) real_->destroy(r.value); });
        return {status, {}};
    }
    const uint32_t proxy = impl_->ids[k_texture].take();
    TextureInfo& t = slot(impl_->textures, proxy);
    t = TextureInfo{};
    t.alive = true; t.width = width; t.height = height; t.format = format; t.pool = pool; t.info = &info;
    t.render_target = (usage & (usage_rendertarget | usage_depthstencil)) != 0;
    t.levels = levels ? levels : d3d9::mip_count(width, height);
    t.has_data.assign(t.levels, t.render_target ? 1 : 0);
    t.locks.resize(t.levels);
    impl_->put(Op::create_texture, CreateTextureCmd{proxy, width, height, levels, usage, format, pool});
    return {Status::ok, TextureId{proxy}};
}

Result<TextureId> ThreadedLayer::create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) {
    const d3d9::FormatInfo& info = d3d9::format_info(format);
    if (!info.valid || !edge) {
        Status status = Status::unsupported;
        sync([&] { auto r = real_->create_cube_texture(edge, levels, usage, format, pool); status = r ? Status::unsupported : r.status; if (r) real_->destroy(r.value); });
        return {status, {}};
    }
    const uint32_t proxy = impl_->ids[k_texture].take();
    TextureInfo& t = slot(impl_->textures, proxy);
    t = TextureInfo{};
    t.alive = true; t.cube = true; t.width = t.height = edge; t.format = format; t.pool = pool; t.info = &info;
    t.render_target = (usage & usage_rendertarget) != 0;
    t.levels = levels ? levels : d3d9::mip_count(edge, edge);
    t.has_data.assign(size_t(t.levels) * 6, t.render_target ? 1 : 0);
    t.locks.resize(size_t(t.levels) * 6);
    impl_->put(Op::create_cube, CreateCubeCmd{proxy, edge, levels, usage, format, pool});
    return {Status::ok, TextureId{proxy}};
}

Result<BufferId> ThreadedLayer::create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) {
    const uint32_t proxy = impl_->ids[k_buffer].take();
    BufferInfo& b = slot(impl_->buffers, proxy);
    b = BufferInfo{}; b.alive = true; b.bytes = bytes;
    impl_->put(Op::create_vb, CreateBufferCmd{proxy, bytes, usage, fvf, pool});
    return {Status::ok, BufferId{proxy}};
}

Result<BufferId> ThreadedLayer::create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) {
    const uint32_t proxy = impl_->ids[k_buffer].take();
    BufferInfo& b = slot(impl_->buffers, proxy);
    b = BufferInfo{}; b.alive = true; b.bytes = bytes;
    impl_->put(Op::create_ib, CreateBufferCmd{proxy, bytes, usage, format, pool});
    return {Status::ok, BufferId{proxy}};
}

Result<SurfaceId> ThreadedLayer::create_render_target(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, bool lockable) {
    const uint32_t proxy = impl_->ids[k_surface].take();
    slot(impl_->surfaces, proxy) = SurfaceInfo{};
    impl_->put(Op::create_rt, CreateSurfaceCmd{proxy, width, height, format, multisample, lockable});
    return {Status::ok, SurfaceId{proxy}};
}

Result<SurfaceId> ThreadedLayer::create_depth_stencil(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample) {
    const uint32_t proxy = impl_->ids[k_surface].take();
    slot(impl_->surfaces, proxy) = SurfaceInfo{};
    impl_->put(Op::create_ds, CreateSurfaceCmd{proxy, width, height, format, multisample, false});
    return {Status::ok, SurfaceId{proxy}};
}

Result<DeclarationId> ThreadedLayer::create_vertex_declaration(std::span<const uint8_t> elements) {
    const uint32_t proxy = impl_->ids[k_decl].take();
    impl_->put_with(Op::create_decl, CreateDataCmd{proxy, uint32_t(elements.size())}, elements.data(), uint32_t(elements.size()));
    return {Status::ok, DeclarationId{proxy}};
}

Result<ShaderId> ThreadedLayer::create_vertex_shader(std::span<const uint32_t> bytecode) {
    const uint32_t proxy = impl_->ids[k_shader].take();
    impl_->put_with(Op::create_vs, CreateDataCmd{proxy, uint32_t(bytecode.size() * 4)}, bytecode.data(), uint32_t(bytecode.size() * 4));
    return {Status::ok, ShaderId{proxy}};
}

Result<ShaderId> ThreadedLayer::create_pixel_shader(std::span<const uint32_t> bytecode) {
    const uint32_t proxy = impl_->ids[k_shader].take();
    impl_->put_with(Op::create_ps, CreateDataCmd{proxy, uint32_t(bytecode.size() * 4)}, bytecode.data(), uint32_t(bytecode.size() * 4));
    return {Status::ok, ShaderId{proxy}};
}

void ThreadedLayer::destroy(TextureId id) {
    if (!id.value) return;
    if (id.value < impl_->textures.size()) impl_->textures[id.value] = TextureInfo{};
    impl_->put(Op::destroy, DestroyCmd{k_texture, id.value});
    impl_->ids[k_texture].give(id.value);
}
void ThreadedLayer::destroy(BufferId id) {
    if (!id.value) return;
    if (id.value < impl_->buffers.size()) { impl_->copies_bytes -= std::min<uint64_t>(impl_->copies_bytes, impl_->buffers[id.value].mirror.size()); impl_->buffers[id.value] = BufferInfo{}; }
    impl_->put(Op::destroy, DestroyCmd{k_buffer, id.value});
    impl_->ids[k_buffer].give(id.value);
}
void ThreadedLayer::destroy(SurfaceId id) {
    if (!id.value) return;
    impl_->put(Op::destroy, DestroyCmd{k_surface, id.value});
    impl_->ids[k_surface].give(id.value);
}
void ThreadedLayer::destroy(DeclarationId id) { if (!id.value) return; impl_->put(Op::destroy, DestroyCmd{k_decl, id.value}); impl_->ids[k_decl].give(id.value); }
void ThreadedLayer::destroy(ShaderId id) { if (!id.value) return; impl_->put(Op::destroy, DestroyCmd{k_shader, id.value}); impl_->ids[k_shader].give(id.value); }
void ThreadedLayer::destroy(StateBlockId id) { if (!id.value) return; impl_->put(Op::destroy, DestroyCmd{k_block, id.value}); impl_->ids[k_block].give(id.value); }

// ---- locks

Result<d3d9::LockedRegion> ThreadedLayer::lock_buffer(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags) {
    if (!id.value || id.value >= impl_->buffers.size() || !impl_->buffers[id.value].alive) return {Status::invalid_argument, {}};
    BufferInfo& b = impl_->buffers[id.value];
    if (b.locked || offset > b.bytes) return {Status::invalid_argument, {}};
    if (!bytes) bytes = b.bytes - offset;
    if (uint64_t(offset) + bytes > b.bytes) return {Status::invalid_argument, {}};
    const bool needs_contents = b.has_data && !(flags & lock_discard);
    if (b.mirror.empty() && needs_contents) {
        // what the buffer holds, as the worker has it once everything queued before is done; kept from now on (within the budget)
        std::vector<std::byte> copy(b.bytes);
        ++impl_->buffer_copies; ++g_worker_buffer_copies;
        sync([&] {
            const BufferId real_id{impl_->real(k_buffer, id.value)};
            auto lock = real_->lock_buffer(real_id, 0, 0, lock_readonly);
            if (lock) { std::memcpy(copy.data(), lock.value.data, copy.size()); real_->unlock_buffer(real_id); }
        });
        if (impl_->copies_bytes + copy.size() <= impl_->copy_budget) { impl_->copies_bytes += copy.size(); b.mirror = std::move(copy); }
        else { b.staging.reset(new std::byte[bytes]); std::memcpy(b.staging.get(), copy.data() + offset, bytes); }
    }
    b.locked = true; b.lock_offset = offset; b.lock_size = bytes; b.lock_flags = flags;
    std::byte* data;
    if (!b.mirror.empty()) data = b.mirror.data() + offset;
    else {
        if (!b.staging) { b.staging.reset(new std::byte[bytes]); if (!b.has_data) std::memset(b.staging.get(), 0, bytes); }
        data = b.staging.get();
    }
    return {Status::ok, d3d9::LockedRegion{data, bytes, 0}};
}

Status ThreadedLayer::unlock_buffer(BufferId id) {
    if (!id.value || id.value >= impl_->buffers.size()) return Status::invalid_argument;
    BufferInfo& b = impl_->buffers[id.value];
    if (!b.locked) return Status::invalid_argument;
    b.locked = false;
    if (b.lock_flags & lock_readonly) { b.staging.reset(); return Status::ok; }
    const std::byte* data = !b.mirror.empty() ? b.mirror.data() + b.lock_offset : b.staging.get();
    BufferWriteCmd c{id.value, b.lock_offset, b.lock_size, b.lock_flags, nullptr};
    if (b.lock_size > kInlineLimit) {
        if (b.mirror.empty()) c.heap = b.staging.release();
        else { c.heap = new std::byte[b.lock_size]; std::memcpy(c.heap, data, b.lock_size); }
        impl_->put(Op::buffer_write, c);
    } else impl_->put_with(Op::buffer_write, c, data, b.lock_size);
    b.staging.reset();
    b.has_data = true;
    return Status::ok;
}

Result<d3d9::LockedRegion> ThreadedLayer::lock_texture(TextureId id, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) {
    if (!id.value || id.value >= impl_->textures.size() || !impl_->textures[id.value].alive) return {Status::invalid_argument, {}};
    TextureInfo& t = impl_->textures[id.value];
    if (level >= t.levels || face >= (t.cube ? 6u : 1u)) return {Status::invalid_argument, {}};
    if (t.render_target) return {Status::unsupported, {}};
    const size_t index = size_t(face) * t.levels + level;
    TextureLock& lock = t.locks[index];
    if (lock.active) return {Status::invalid_argument, {}};
    if (t.has_data[index] && !(flags & lock_discard)) {
        // the level holds data the lock must show: lock it on the worker once the queue is drained; the game then writes into the layer's own staging
        Result<d3d9::LockedRegion> r{Status::invalid_argument, {}};
        ++impl_->texture_reads; ++g_worker_texture_reads;
        sync([&] { r = real_->lock_texture(TextureId{impl_->real(k_texture, id.value)}, level, face, rect4, flags); });
        if (!r) return r;
        lock = TextureLock{}; lock.active = true; lock.direct = true; lock.flags = flags;
        return r;
    }
    const uint32_t lw = d3d9::mip_dimension(t.width, level), lh = d3d9::mip_dimension(t.height, level);
    uint32_t x = 0, y = 0, w = lw, h = lh;
    if (rect4) {   // as the layer clamps it
        x = uint32_t(std::max(0, rect4[0])); y = uint32_t(std::max(0, rect4[1]));
        w = uint32_t(std::max(0, rect4[2] - rect4[0])); h = uint32_t(std::max(0, rect4[3] - rect4[1]));
        if (x + w > lw) w = lw - std::min(x, lw);
        if (y + h > lh) h = lh - std::min(y, lh);
    }
    lock = TextureLock{};
    lock.active = true; lock.flags = flags; lock.has_rect = rect4 != nullptr;
    if (rect4) std::memcpy(lock.rect, rect4, sizeof lock.rect);
    lock.pitch = d3d9::level_pitch(*t.info, w);
    lock.size = d3d9::level_bytes(*t.info, w, h);
    lock.staging.reset(new std::byte[lock.size]);
    std::memset(lock.staging.get(), 0, lock.size);
    return {Status::ok, d3d9::LockedRegion{lock.staging.get(), lock.pitch, 0}};
}

Status ThreadedLayer::unlock_texture(TextureId id, uint32_t level, uint32_t face) {
    if (!id.value || id.value >= impl_->textures.size()) return Status::invalid_argument;
    TextureInfo& t = impl_->textures[id.value];
    if (level >= t.levels || face >= (t.cube ? 6u : 1u)) return Status::invalid_argument;
    const size_t index = size_t(face) * t.levels + level;
    TextureLock& lock = t.locks[index];
    if (!lock.active) return Status::invalid_argument;
    lock.active = false;
    if (lock.direct) { impl_->put(Op::texture_unlock_direct, TextureUnlockCmd{id.value, level, face}); }
    else if (!(lock.flags & lock_readonly)) {
        TextureWriteCmd c{id.value, level, face, lock.flags, lock.pitch, lock.size, lock.has_rect, {}, nullptr};
        std::memcpy(c.rect, lock.rect, sizeof c.rect);
        if (lock.size > kInlineLimit) { c.heap = lock.staging.release(); impl_->put(Op::texture_write, c); }
        else impl_->put_with(Op::texture_write, c, lock.staging.get(), lock.size);
    }
    if (!(lock.flags & lock_readonly)) t.has_data[index] = 1;
    lock.staging.reset();
    return Status::ok;
}

Result<SurfaceId> ThreadedLayer::texture_surface(TextureId id, uint32_t level, uint32_t face) {
    const uint32_t proxy = impl_->ids[k_surface].take();
    slot(impl_->surfaces, proxy) = SurfaceInfo{id.value, level, face};
    impl_->put(Op::texture_surface, TextureSurfaceCmd{proxy, id.value, level, face});
    return {Status::ok, SurfaceId{proxy}};
}

Result<SurfaceId> ThreadedLayer::back_buffer(uint32_t swap_chain, uint32_t index) {
    Result<SurfaceId> r{Status::invalid_argument, {}};
    sync([&] { r = real_->back_buffer(swap_chain, index); });
    if (!r) return r;
    const uint32_t proxy = impl_->ids[k_surface].take();
    slot(impl_->surfaces, proxy) = SurfaceInfo{};
    const uint32_t real_id = r.value.value;
    post([this, proxy, real_id] { impl_->bind(k_surface, proxy, real_id); });
    return {Status::ok, SurfaceId{proxy}};
}

SurfaceId ThreadedLayer::auto_depth_stencil() {
    SurfaceId real_id;
    sync([&] { real_id = real_->auto_depth_stencil(); });
    if (!real_id.value) return {};
    const uint32_t proxy = impl_->ids[k_surface].take();
    slot(impl_->surfaces, proxy) = SurfaceInfo{};
    post([this, proxy, value = real_id.value] { impl_->bind(k_surface, proxy, value); });
    return SurfaceId{proxy};
}

Status ThreadedLayer::stretch_rect(SurfaceId from, const int32_t* from_rect4, SurfaceId to, const int32_t* to_rect4, uint32_t filter) {
    StretchCmd c{from.value, to.value, filter, from_rect4 != nullptr, to_rect4 != nullptr, {}, {}};
    if (from_rect4) std::memcpy(c.from_rect, from_rect4, sizeof c.from_rect);
    if (to_rect4) std::memcpy(c.to_rect, to_rect4, sizeof c.to_rect);
    impl_->put(Op::stretch_rect, c);
    if (to.value < impl_->surfaces.size()) {   // a texture level that is drawn into has data
        const SurfaceInfo& s = impl_->surfaces[to.value];
        if (s.texture && s.texture < impl_->textures.size()) { TextureInfo& t = impl_->textures[s.texture]; const size_t i = size_t(s.face) * t.levels + s.level; if (i < t.has_data.size()) t.has_data[i] = 1; }
    }
    return Status::ok;
}

Status ThreadedLayer::update_texture(TextureId source, TextureId destination) {
    impl_->put(Op::update_texture, PairCmd{source.value, destination.value});
    if (destination.value < impl_->textures.size()) for (auto& h : impl_->textures[destination.value].has_data) h = 1;
    return Status::ok;
}

// ---- state

void ThreadedLayer::set_render_state(uint32_t state, uint32_t value) {
    if (state < 256 && impl_->unchanged(impl_->rs_sent[state], value)) return;
    impl_->put(Op::rs, PairCmd{state, value});
}
void ThreadedLayer::set_sampler_state(uint32_t sampler, uint32_t type, uint32_t value) {
    if (sampler < 16 && type < 14 && impl_->unchanged(impl_->ss_sent[sampler][type], value)) return;
    impl_->put(Op::ss, TripleCmd{sampler, type, value});
}
void ThreadedLayer::set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value) {
    if (stage < 8 && type < 33 && impl_->unchanged(impl_->tss_sent[stage][type], value)) return;
    impl_->put(Op::tss, TripleCmd{stage, type, value});
}
void ThreadedLayer::set_transform(uint32_t type, const float* m) { TransformCmd c{type, {}}; std::memcpy(c.m, m, sizeof c.m); impl_->put(Op::transform, c); }
void ThreadedLayer::light_enable(uint32_t index, bool enable) { impl_->put(Op::light_enable, PairCmd{index, enable ? 1u : 0u}); }
void ThreadedLayer::set_light(uint32_t index, const d3d9::LightDesc& light) { impl_->put(Op::light, LightCmd{index, light}); }
void ThreadedLayer::set_material(const d3d9::MaterialDesc& m) { impl_->put(Op::material, m); }
bool ThreadedLayer::get_shader_constants_f(bool pixel, uint32_t start, d3d9::Float4* out, uint32_t count) const { bool ok = false; const_cast<ThreadedLayer*>(this)->sync([&] { ok = real_->get_shader_constants_f(pixel, start, out, count); }); return ok; }
bool ThreadedLayer::get_shader_constants_i(bool pixel, uint32_t start, d3d9::Int4* out, uint32_t count) const { bool ok = false; const_cast<ThreadedLayer*>(this)->sync([&] { ok = real_->get_shader_constants_i(pixel, start, out, count); }); return ok; }
bool ThreadedLayer::get_shader_constants_b(bool pixel, uint32_t start, uint32_t* out, uint32_t count) const { bool ok = false; const_cast<ThreadedLayer*>(this)->sync([&] { ok = real_->get_shader_constants_b(pixel, start, out, count); }); return ok; }
void ThreadedLayer::set_viewport(const uint32_t* r, float min_z, float max_z) { ViewportCmd c{{r[0], r[1], r[2], r[3]}, min_z, max_z}; impl_->put(Op::viewport, c); }
void ThreadedLayer::set_texture(uint32_t sampler, TextureId id) { impl_->put(Op::texture, PairCmd{sampler, id.value}); }
void ThreadedLayer::set_render_target(uint32_t index, SurfaceId id) { impl_->put(Op::render_target, PairCmd{index, id.value}); }
void ThreadedLayer::set_depth_stencil(SurfaceId id) { impl_->put(Op::depth_stencil, id.value); }
void ThreadedLayer::set_vertex_declaration(DeclarationId id) { impl_->put(Op::decl, id.value); }
void ThreadedLayer::set_fvf(uint32_t fvf) { impl_->put(Op::fvf, fvf); }
void ThreadedLayer::set_vertex_shader(ShaderId id) { impl_->put(Op::vs, id.value); }
void ThreadedLayer::set_pixel_shader(ShaderId id) { impl_->put(Op::ps, id.value); }
void ThreadedLayer::set_vertex_shader_constants_f(uint32_t start, std::span<const d3d9::Float4> v) { impl_->put_with(Op::const_f, ConstCmd{0, start, uint32_t(v.size())}, v.data(), uint32_t(v.size_bytes())); }
void ThreadedLayer::set_pixel_shader_constants_f(uint32_t start, std::span<const d3d9::Float4> v) { impl_->put_with(Op::const_f, ConstCmd{1, start, uint32_t(v.size())}, v.data(), uint32_t(v.size_bytes())); }
void ThreadedLayer::set_vertex_shader_constants_i(uint32_t start, std::span<const d3d9::Int4> v) { impl_->put_with(Op::const_i, ConstCmd{0, start, uint32_t(v.size())}, v.data(), uint32_t(v.size_bytes())); }
void ThreadedLayer::set_pixel_shader_constants_i(uint32_t start, std::span<const d3d9::Int4> v) { impl_->put_with(Op::const_i, ConstCmd{1, start, uint32_t(v.size())}, v.data(), uint32_t(v.size_bytes())); }
void ThreadedLayer::set_vertex_shader_constants_b(uint32_t start, std::span<const uint32_t> v) { impl_->put_with(Op::const_b, ConstCmd{0, start, uint32_t(v.size())}, v.data(), uint32_t(v.size_bytes())); }
void ThreadedLayer::set_pixel_shader_constants_b(uint32_t start, std::span<const uint32_t> v) { impl_->put_with(Op::const_b, ConstCmd{1, start, uint32_t(v.size())}, v.data(), uint32_t(v.size_bytes())); }
void ThreadedLayer::set_stream_source(uint32_t stream, BufferId id, uint32_t offset, uint32_t stride) { impl_->put(Op::stream, StreamCmd{stream, id.value, offset, stride}); }
void ThreadedLayer::set_indices(BufferId id) { impl_->put(Op::indices, id.value); }
StateBlockId ThreadedLayer::begin_state_block() { const uint32_t proxy = impl_->ids[k_block].take(); impl_->recording = true; impl_->put(Op::begin_block, proxy); return StateBlockId{proxy}; }
void ThreadedLayer::end_state_block(StateBlockId id) { impl_->recording = false; impl_->forget_states(); impl_->put(Op::end_block, id.value); }
void ThreadedLayer::apply(StateBlockId id) { impl_->forget_states(); impl_->put(Op::apply, id.value); }

// ---- draws

void ThreadedLayer::clear(std::span<const int32_t> rects4, uint32_t flags, uint32_t argb, float z, uint32_t stencil) {
    impl_->put_with(Op::clear, ClearCmd{uint32_t(rects4.size()), flags, argb, z, stencil}, rects4.data(), uint32_t(rects4.size_bytes()));
}
void ThreadedLayer::draw_indexed(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count) {
    impl_->put(Op::draw_indexed, DrawIndexedCmd{primitive, base_vertex, min_vertex, vertex_count, start_index, primitive_count});
}
void ThreadedLayer::draw_up(uint32_t primitive, uint32_t primitive_count, Bytes vertices, uint32_t stride) {
    DrawUpCmd c{primitive, primitive_count, stride, uint32_t(vertices.size()), nullptr};
    if (vertices.size() > kInlineLimit) { c.heap = new std::byte[vertices.size()]; std::memcpy(c.heap, vertices.data(), vertices.size()); impl_->put(Op::draw_up, c); }
    else impl_->put_with(Op::draw_up, c, vertices.data(), uint32_t(vertices.size()));
}

}  // namespace nue::facade
