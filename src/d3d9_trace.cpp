// Trace file writer, reader and headless replay. The payload of every op is a list of u32 words; the layout is the one the replay below reads:
//   reset                [back_w, back_h, back_format, depth_format (0 = none), multisample, buffers, windowed, present_interval]
//   op_device_created    [adapter, behaviour flags] + the 8 words of reset
//   create_texture       [obj, w, h, levels, usage, format, pool]          create_cube_texture [obj, edge, levels, usage, format, pool]
//   create_vertex_buffer [obj, bytes, usage, fvf, pool]                    create_index_buffer [obj, bytes, usage, format, pool]
//   create_render_target [obj, w, h, format, multisample, lockable]        create_depth_stencil [obj, w, h, format, multisample]
//   create_vertex_declaration / create_vertex_shader / create_pixel_shader [obj, blob(3 words)]
//   destroy_*            [obj]
//   lock_buffer          [obj, offset, bytes, flags]                       unlock_buffer [obj, blob]  (blob = the written range)
//   lock_texture         [obj, level, face, flags, has_rect, l, t, r, b]   unlock_texture [obj, level, face, pitch, blob]
//   texture_surface      [surface, texture, level, face]                   op_get_back_buffer [surface, swap_chain, index]
//   stretch_rect         [src, has_rect, l, t, r, b, dst, has_rect, l, t, r, b, filter]
//   set_render_state [state, value]  set_sampler_state [sampler, type, value]  set_texture_stage_state [stage, type, value]
//   light_enable [index, enable]  set_transform [type, 16 floats]  set_viewport [x, y, w, h, min_z, max_z]   set_texture [sampler, obj]  set_render_target [index, obj]
//   set_depth_stencil [obj]  set_vertex_declaration [obj]  set_fvf [fvf]  set_vertex_shader [obj]  set_pixel_shader [obj]
//   set_vertex_shader_constants_f / set_pixel_shader_constants_f [start, count, count*4 floats]
//   set_stream_source [stream, obj, offset, stride]  set_indices [obj]  end_state_block [obj]  apply_state_block [obj]
//   clear [count, flags, argb, z, stencil, count*4 rect ints]   draw_indexed [primitive, base_vertex, min_vertex, vertex_count, start_index, primitive_count]
//   draw_up [primitive, primitive_count, stride, blob]   op_frame_marker [frame, instructions_low, instructions_high]
//   op_reference_image [frame, w, h, format, blob]   op_unknown_call [slot, up to 8 argument words]
#include "nue/d3d9_trace.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace nue::d3d9 {
namespace {
// 64-bit hash of a data block, eight bytes at a time (the recorder hashes every lock of the run: it must stay cheap). Equal hash and size mean
// equal data for this purpose (a collision would need two different blocks of the same size with the same 64-bit hash).
uint64_t hash64(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(size) * 0xFF51AFD7ED558CCDull);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    uint64_t tail = 0;
    for (size_t k = 0; i + k < size; ++k) tail |= uint64_t(p[i + k]) << (8 * k);
    h = (h ^ tail) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    return h;
}
float as_float(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
}  // namespace

bool TraceWriter::open(const std::filesystem::path& directory, uint64_t max_blob_bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    trace_ = std::fopen((directory / "trace.d3d9").string().c_str(), "wb");
    blobs_ = std::fopen((directory / "blobs.bin").string().c_str(), "w+b");
    if (!trace_ || !blobs_) { error = "cannot create the trace files in " + directory.string(); close(); return false; }
    max_blob_bytes_ = max_blob_bytes;
    std::fwrite("NFD9TRC1", 1, 8, trace_);
    const uint32_t version = 2;
    std::fwrite(&version, 4, 1, trace_);
    return true;
}

void TraceWriter::close() {
    if (trace_) { std::fclose(trace_); trace_ = nullptr; }
    if (blobs_) { std::fclose(blobs_); blobs_ = nullptr; }
}

void TraceWriter::record(uint8_t op, const std::vector<uint32_t>& payload) {
    if (!trace_) return;
    const uint32_t size = uint32_t(payload.size() * 4);
    std::fwrite(&op, 1, 1, trace_);
    std::fwrite(&size, 4, 1, trace_);
    if (size) std::fwrite(payload.data(), 4, payload.size(), trace_);
    ++records_;
}

Blob TraceWriter::blob(const void* data, uint32_t size) {
    if (!blobs_ || !size) return {};
    const uint64_t h = hash64(data, size);
    auto found = index_.find(h);
    if (found != index_.end() && found->second.size == size) return found->second;
    if (max_blob_bytes_ && blob_end_ + size > max_blob_bytes_) {
        if (!truncated_) { truncated_ = true; record(op_truncated, {}); }
        return {};
    }
    std::fwrite(data, 1, size, blobs_);
    Blob b{blob_end_, size};
    blob_end_ += size;
    index_[h] = b;
    return b;
}

void TraceWriter::flush() {
    if (trace_) std::fflush(trace_);
    if (blobs_) std::fflush(blobs_);
}

bool TraceReader::open(const std::filesystem::path& directory, std::string& error) {
    trace_ = std::fopen((directory / "trace.d3d9").string().c_str(), "rb");
    blobs_ = std::fopen((directory / "blobs.bin").string().c_str(), "rb");
    char magic[8]; uint32_t version = 0;
    if (!trace_ || std::fread(magic, 1, 8, trace_) != 8 || std::memcmp(magic, "NFD9TRC1", 8) != 0 || std::fread(&version, 4, 1, trace_) != 1 || version != 2) {
        error = "not a Direct3D 9 trace: " + directory.string();
        close();
        return false;
    }
    return true;
}

void TraceReader::close() {
    if (trace_) { std::fclose(trace_); trace_ = nullptr; }
    if (blobs_) { std::fclose(blobs_); blobs_ = nullptr; }
}

bool TraceReader::next(Record& out) {
    if (!trace_) return false;
    uint8_t op; uint32_t size;
    if (std::fread(&op, 1, 1, trace_) != 1 || std::fread(&size, 4, 1, trace_) != 1) return false;
    out.op = op;
    out.words.assign(size / 4, 0);
    return size == 0 || std::fread(out.words.data(), 1, size, trace_) == size;
}

std::vector<uint8_t> TraceReader::read_blob(uint64_t offset, uint32_t size) const {
    std::vector<uint8_t> data(size);
    if (!blobs_ || !size) return {};
    _fseeki64(blobs_, static_cast<long long>(offset), SEEK_SET);
    if (std::fread(data.data(), 1, size, blobs_) != size) return {};
    return data;
}

namespace {
struct Replayer {
    TraceReader& reader;
    Device& device;
    ReplayStats& stats;
    const ReplayHooks* hooks = nullptr;
    enum class Kind : uint8_t { texture, buffer, surface, declaration, shader, state_block };
    struct Entry { Kind kind; uint32_t id; };
    std::unordered_map<uint32_t, Entry> objects;
    struct Lock { LockedRegion region; uint32_t bytes = 0; };

    template <class IdType> IdType find(uint32_t obj, Kind kind) {
        auto it = objects.find(obj);
        if (it == objects.end() || it->second.kind != kind) { if (obj) ++stats.bad_objects; return IdType{}; }
        return IdType{it->second.id};
    }
    template <class IdType> void remember(uint32_t obj, Kind kind, const Result<IdType>& r) { if (r) objects[obj] = {kind, r.value.value}; }
    void forget(uint32_t obj) { objects.erase(obj); }

    // Traces made before the recorder logged a BeginStateBlock outside a window: state calls outside a window can only come from a state block being
    // recorded, so the first of them opens an implicit recording.
    bool in_window = false;
    bool is_state_op(uint8_t op) const {
        switch (op) {
        case uint8_t(Method::set_render_state): case uint8_t(Method::set_sampler_state): case uint8_t(Method::set_texture_stage_state): case uint8_t(Method::set_transform):
        case uint8_t(Method::light_enable): case uint8_t(Method::set_viewport): case uint8_t(Method::set_texture): case uint8_t(Method::set_vertex_declaration):
        case uint8_t(Method::set_fvf): case uint8_t(Method::set_vertex_shader): case uint8_t(Method::set_pixel_shader): case uint8_t(Method::set_vertex_shader_constants_f):
        case uint8_t(Method::set_pixel_shader_constants_f): case uint8_t(Method::set_stream_source): case uint8_t(Method::set_indices): return true;
        default: return false;
        }
    }
    bool step(const Record& rec) {
        const auto& w = rec.words;
        if (!in_window && !recording_active && is_state_op(rec.op)) { recording_block = device.begin_state_block(); recording_active = true; }
        auto need = [&](size_t n) { return w.size() >= n; };
        ++stats.by_op[rec.op];
        ++stats.records;
        switch (rec.op) {
        case uint8_t(Method::reset): if (!need(8)) return false; { DeviceParams p; p.back_width = w[0]; p.back_height = w[1]; p.back_format = w[2]; p.depth_format = w[3]; p.multisample = w[4]; p.buffers = w[5]; p.windowed = w[6] != 0; p.present_interval = w[7]; device.reset(p); } break;
        case op_device_created: if (!need(10)) return false; { DeviceParams p; p.back_width = w[2]; p.back_height = w[3]; p.back_format = w[4]; p.depth_format = w[5]; p.multisample = w[6]; p.buffers = w[7]; p.windowed = w[8] != 0; p.present_interval = w[9]; device.reset(p); } break;
        case uint8_t(Method::present): device.present(); ++stats.frames; if (hooks && hooks->presented) hooks->presented(stats.frames); break;
        case op_frame_marker: if (hooks && hooks->frame_marker && need(1)) hooks->frame_marker(w[0]); break;
        case uint8_t(Method::create_texture): if (!need(7)) return false; remember(w[0], Kind::texture, device.create_texture(w[1], w[2], w[3], w[4], w[5], w[6])); break;
        case uint8_t(Method::create_cube_texture): if (!need(6)) return false; remember(w[0], Kind::texture, device.create_cube_texture(w[1], w[2], w[3], w[4], w[5])); break;
        case uint8_t(Method::create_vertex_buffer): if (!need(5)) return false; buffer_sizes[w[0]] = w[1]; remember(w[0], Kind::buffer, device.create_vertex_buffer(w[1], w[2], w[3], w[4])); break;
        case uint8_t(Method::create_index_buffer): if (!need(5)) return false; buffer_sizes[w[0]] = w[1]; remember(w[0], Kind::buffer, device.create_index_buffer(w[1], w[2], w[3], w[4])); break;
        case uint8_t(Method::create_render_target): if (!need(6)) return false; remember(w[0], Kind::surface, device.create_render_target(w[1], w[2], w[3], w[4], w[5] != 0)); break;
        case uint8_t(Method::create_depth_stencil): if (!need(5)) return false; remember(w[0], Kind::surface, device.create_depth_stencil(w[1], w[2], w[3], w[4])); break;
        case uint8_t(Method::create_vertex_declaration): case uint8_t(Method::create_vertex_shader): case uint8_t(Method::create_pixel_shader): {
            if (!need(4)) return false;
            const auto data = reader.read_blob(uint64_t(w[1]) | (uint64_t(w[2]) << 32), w[3]);
            if (rec.op == uint8_t(Method::create_vertex_declaration)) remember(w[0], Kind::declaration, device.create_vertex_declaration({data.data(), data.size()}));
            else {
                std::vector<uint32_t> words(data.size() / 4);
                std::memcpy(words.data(), data.data(), words.size() * 4);
                remember(w[0], Kind::shader, rec.op == uint8_t(Method::create_vertex_shader) ? device.create_vertex_shader(words) : device.create_pixel_shader(words));
            }
            break;
        }
        case uint8_t(Method::destroy_texture): if (!need(1)) return false; device.destroy(find<TextureId>(w[0], Kind::texture)); forget(w[0]); break;
        case uint8_t(Method::destroy_buffer): if (!need(1)) return false; device.destroy(find<BufferId>(w[0], Kind::buffer)); forget(w[0]); break;
        case uint8_t(Method::destroy_surface): if (!need(1)) return false; device.destroy(find<SurfaceId>(w[0], Kind::surface)); forget(w[0]); break;
        case uint8_t(Method::destroy_declaration): if (!need(1)) return false; device.destroy(find<DeclarationId>(w[0], Kind::declaration)); forget(w[0]); break;
        case uint8_t(Method::destroy_shader): if (!need(1)) return false; device.destroy(find<ShaderId>(w[0], Kind::shader)); forget(w[0]); break;
        case uint8_t(Method::destroy_state_block): if (!need(1)) return false; device.destroy(find<StateBlockId>(w[0], Kind::state_block)); forget(w[0]); break;
        case uint8_t(Method::lock_buffer): if (!need(4)) return false; {
            auto r = device.lock_buffer(find<BufferId>(w[0], Kind::buffer), w[1], w[2], w[3]);
            if (r) pending_buffer_locks[w[0]] = {r.value, w[2] ? w[2] : buffer_sizes[w[0]]};
        } break;
        case uint8_t(Method::unlock_buffer): {
            if (!need(4)) return false;
            // Replay the lock that preceded (it returned staging memory): write the data into it, then unlock.
            const BufferId id = find<BufferId>(w[0], Kind::buffer);
            const auto data = reader.read_blob(uint64_t(w[1]) | (uint64_t(w[2]) << 32), w[3]);
            auto it = pending_buffer_locks.find(w[0]);
            if (it != pending_buffer_locks.end() && it->second.region.data && !data.empty() && data.size() <= it->second.bytes) std::memcpy(it->second.region.data, data.data(), data.size());
            pending_buffer_locks.erase(w[0]);
            device.unlock_buffer(id);
            break;
        }
        case uint8_t(Method::lock_texture): if (!need(9)) return false; {
            const int32_t rect[4] = {int32_t(w[5]), int32_t(w[6]), int32_t(w[7]), int32_t(w[8])};
            auto r = device.lock_texture(find<TextureId>(w[0], Kind::texture), w[1], w[2], w[4] ? rect : nullptr, w[3]);
            if (r) pending_texture_locks[(uint64_t(w[0]) << 8) | ((w[1] & 15) << 4) | (w[2] & 15)] = r.value;
        } break;
        case uint8_t(Method::unlock_texture): {
            if (!need(7)) return false;
            // Write the recorded texels into the staging memory the layer handed out (row by row: the layer's pitch may differ from the recorded one).
            const auto data = reader.read_blob(uint64_t(w[4]) | (uint64_t(w[5]) << 32), w[6]);
            const uint64_t key = (uint64_t(w[0]) << 8) | ((w[1] & 15) << 4) | (w[2] & 15);
            auto it = pending_texture_locks.find(key);
            const uint32_t recorded_pitch = w[3];
            if (it != pending_texture_locks.end() && it->second.data && !data.empty() && recorded_pitch) {
                const uint32_t pitch = it->second.pitch ? it->second.pitch : recorded_pitch;
                const uint32_t rows = uint32_t(data.size() / recorded_pitch);
                const uint32_t row_bytes = std::min(pitch, recorded_pitch);
                for (uint32_t row = 0; row < rows; ++row) std::memcpy(it->second.data + size_t(row) * pitch, data.data() + size_t(row) * recorded_pitch, row_bytes);
            }
            pending_texture_locks.erase(key);
            device.unlock_texture(find<TextureId>(w[0], Kind::texture), w[1], w[2]);
        } break;
        case uint8_t(Method::texture_surface): if (!need(4)) return false; remember(w[0], Kind::surface, device.texture_surface(find<TextureId>(w[1], Kind::texture), w[2], w[3])); break;
        case op_get_back_buffer: if (!need(3)) return false; remember(w[0], Kind::surface, device.back_buffer(w[1], w[2])); break;
        case uint8_t(Method::stretch_rect): if (!need(13)) return false; {
            const int32_t src_rect[4] = {int32_t(w[2]), int32_t(w[3]), int32_t(w[4]), int32_t(w[5])}, dst_rect[4] = {int32_t(w[8]), int32_t(w[9]), int32_t(w[10]), int32_t(w[11])};
            device.stretch_rect(find<SurfaceId>(w[0], Kind::surface), w[1] ? src_rect : nullptr, find<SurfaceId>(w[6], Kind::surface), w[7] ? dst_rect : nullptr, w[12]);
        } break;
        case uint8_t(Method::set_render_state): if (!need(2)) return false; device.set_render_state(w[0], w[1]); break;
        case uint8_t(Method::set_sampler_state): if (!need(3)) return false; device.set_sampler_state(w[0], w[1], w[2]); break;
        case uint8_t(Method::set_texture_stage_state): if (!need(3)) return false; device.set_texture_stage_state(w[0], w[1], w[2]); break;
        case uint8_t(Method::set_transform): if (!need(17)) return false; { float m[16]; for (int i = 0; i < 16; ++i) m[i] = as_float(w[1 + i]); device.set_transform(w[0], m); } break;
        case uint8_t(Method::light_enable): if (!need(2)) return false; device.light_enable(w[0], w[1] != 0); break;
        case uint8_t(Method::set_viewport): if (!need(6)) return false; { const uint32_t r[4] = {w[0], w[1], w[2], w[3]}; device.set_viewport(r, as_float(w[4]), as_float(w[5])); } break;
        case uint8_t(Method::set_texture): if (!need(2)) return false; device.set_texture(w[0], find<TextureId>(w[1], Kind::texture)); break;
        case uint8_t(Method::set_render_target): if (!need(2)) return false; device.set_render_target(w[0], find<SurfaceId>(w[1], Kind::surface)); break;
        case uint8_t(Method::set_depth_stencil): if (!need(1)) return false; device.set_depth_stencil(find<SurfaceId>(w[0], Kind::surface)); break;
        case uint8_t(Method::set_vertex_declaration): if (!need(1)) return false; device.set_vertex_declaration(find<DeclarationId>(w[0], Kind::declaration)); break;
        case uint8_t(Method::set_fvf): if (!need(1)) return false; device.set_fvf(w[0]); break;
        case uint8_t(Method::set_vertex_shader): if (!need(1)) return false; device.set_vertex_shader(find<ShaderId>(w[0], Kind::shader)); break;
        case uint8_t(Method::set_pixel_shader): if (!need(1)) return false; device.set_pixel_shader(find<ShaderId>(w[0], Kind::shader)); break;
        case uint8_t(Method::set_vertex_shader_constants_f): case uint8_t(Method::set_pixel_shader_constants_f): {
            if (!need(2) || w.size() < 2 + size_t(w[1]) * 4) return false;
            const auto* floats = reinterpret_cast<const Float4*>(w.data() + 2);
            if (rec.op == uint8_t(Method::set_vertex_shader_constants_f)) device.set_vertex_shader_constants_f(w[0], {floats, w[1]});
            else device.set_pixel_shader_constants_f(w[0], {floats, w[1]});
            break;
        }
        case uint8_t(Method::set_stream_source): if (!need(4)) return false; device.set_stream_source(w[0], find<BufferId>(w[1], Kind::buffer), w[2], w[3]); break;
        case uint8_t(Method::set_indices): if (!need(1)) return false; device.set_indices(find<BufferId>(w[0], Kind::buffer)); break;
        case uint8_t(Method::begin_state_block): if (!recording_active) { recording_block = device.begin_state_block(); recording_active = true; } break;
        case uint8_t(Method::end_state_block):
            if (!need(1)) return false;
            if (!recording_active) { recording_block = device.begin_state_block(); }   // an empty block
            device.end_state_block(recording_block);
            recording_active = false;
            objects[w[0]] = {Kind::state_block, recording_block.value};
            break;
        case uint8_t(Method::apply_state_block): if (!need(1)) return false; device.apply(find<StateBlockId>(w[0], Kind::state_block)); break;
        case uint8_t(Method::begin_scene): device.begin_scene(); break;
        case uint8_t(Method::end_scene): device.end_scene(); break;
        case uint8_t(Method::clear): if (!need(5) || w.size() < 5 + size_t(w[0]) * 4) return false; device.clear({reinterpret_cast<const int32_t*>(w.data() + 5), w[0] * 4}, w[1], w[2], as_float(w[3]), w[4]); break;
        case uint8_t(Method::draw_indexed): if (!need(6)) return false; device.draw_indexed(w[0], int32_t(w[1]), w[2], w[3], w[4], w[5]); break;
        case uint8_t(Method::draw_up): {
            if (!need(6)) return false;
            const auto data = reader.read_blob(uint64_t(w[3]) | (uint64_t(w[4]) << 32), w[5]);
            device.draw_up(w[0], w[1], {reinterpret_cast<const std::byte*>(data.data()), data.size()}, w[2]);
            break;
        }
        case op_reference_image: if (hooks && hooks->reference_image && need(4)) hooks->reference_image(w[0], w[1], w[2], w[3]); break;
        case op_window_begin: in_window = true; if (hooks && hooks->window_begin && need(2)) hooks->window_begin(w[0], w[1]); break;
        case op_window_end: in_window = false; if (hooks && hooks->window_end && need(2)) hooks->window_end(w[0], w[1]); break;
        case op_truncated: case op_end: case op_checkpoint: break;
        case op_unknown_call: ++stats.unsupported; break;
        default: ++stats.unsupported; break;
        }
        return true;
    }
    std::unordered_map<uint32_t, Lock> pending_buffer_locks;
    std::unordered_map<uint32_t, uint32_t> buffer_sizes;
    StateBlockId recording_block;
    bool recording_active = false;
    std::unordered_map<uint64_t, LockedRegion> pending_texture_locks;
};
}  // namespace

bool replay(TraceReader& reader, Device& device, ReplayStats& stats, std::string& error, uint64_t max_frames, const ReplayHooks* hooks) {
    Replayer replayer{reader, device, stats, hooks};
    Record record;
    while (reader.next(record)) {
        if (!replayer.step(record)) { error = "malformed record, op " + std::to_string(record.op); return false; }
        if (max_frames && stats.frames >= max_frames) break;
    }
    return true;
}

}  // namespace nue::d3d9
