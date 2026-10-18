// Layer: device lifecycle, resource creation and destruction, locks and unlocks.
#include "layer/layer.hpp"
#include <algorithm>
#include <cstring>

namespace nue::d3d9 {
using namespace k;

std::unique_ptr<Layer> create_layer(gpu::Gpu& gpu, const LayerOptions& options) { return std::make_unique<LayerImpl>(gpu, options); }

LayerImpl::LayerImpl(gpu::Gpu& gpu, const LayerOptions& options) : gpu_(gpu), options_(options) { init_state(state_); }

LayerImpl::~LayerImpl() {
    end_pass();
    for (auto& t : textures_.items) if (t.alive && t.gpu) gpu_.destroy(t.gpu);
    for (auto& b : buffers_.items) if (b.alive && b.gpu) gpu_.destroy(b.gpu);
    for (auto& s : surfaces_.items) if (s.alive && s.gpu) gpu_.destroy(s.gpu);
    if (dummy_2d_) gpu_.destroy(dummy_2d_);
    if (dummy_cube_) gpu_.destroy(dummy_cube_);
}

void LayerImpl::log(const std::string& message) { if (options_.log) options_.log(message.c_str()); }

void LayerImpl::note_unsupported(const char* what) {
    ++stats_.unsupported_calls;
    if (unsupported_[what]++ == 0 && options_.log_unsupported) log(std::string("unsupported: ") + what);
}

std::string LayerImpl::report() const {
    std::string text;
    for (const auto& [what, count] : unsupported_) text += "  " + what + ": " + std::to_string(count) + "\n";
    return text.empty() ? "  nothing unsupported was met\n" : text;
}

void LayerImpl::init_state(DeviceState& st) {
    st = DeviceState{};
    auto& rs = st.rs;
    rs[rs_zenable] = 1; rs[rs_fillmode] = fill_solid; rs[rs_shademode] = 2; rs[rs_zwriteenable] = 1; rs[rs_alphatestenable] = 0; rs[rs_lastpixel] = 1;
    rs[rs_srcblend] = blend_one; rs[rs_destblend] = blend_zero; rs[rs_cullmode] = cull_ccw; rs[rs_zfunc] = cmp_lessequal; rs[rs_alpharef] = 0; rs[rs_alphafunc] = cmp_always;
    rs[rs_ditherenable] = 0; rs[rs_alphablendenable] = 0; rs[rs_fogenable] = 0; rs[rs_specularenable] = 0; rs[rs_fogcolor] = 0; rs[rs_fogtablemode] = 0;
    rs[rs_fogstart] = 0; rs[rs_fogend] = 0x3F800000; rs[rs_fogdensity] = 0x3F800000; rs[rs_rangefogenable] = 0;
    rs[rs_stencilenable] = 0; rs[rs_stencilfail] = stencil_keep; rs[rs_stencilzfail] = stencil_keep; rs[rs_stencilpass] = stencil_keep; rs[rs_stencilfunc] = cmp_always;
    rs[rs_stencilref] = 0; rs[rs_stencilmask] = 0xFFFFFFFF; rs[rs_stencilwritemask] = 0xFFFFFFFF; rs[rs_texturefactor] = 0xFFFFFFFF;
    rs[rs_clipping] = 1; rs[rs_lighting] = 1; rs[rs_ambient] = 0; rs[rs_colorvertex] = 1; rs[rs_localviewer] = 1; rs[rs_normalizenormals] = 0;
    rs[rs_diffusematerialsource] = 1; rs[rs_specularmaterialsource] = 2; rs[rs_ambientmaterialsource] = 0; rs[rs_emissivematerialsource] = 0;
    rs[rs_colorwriteenable] = 0xF; rs[rs_colorwriteenable1] = rs[rs_colorwriteenable2] = rs[rs_colorwriteenable3] = 0xF;
    rs[rs_blendop] = blendop_add; rs[rs_blendfactor] = 0xFFFFFFFF; rs[rs_srgbwriteenable] = 0; rs[rs_depthbias] = 0; rs[rs_slopescaledepthbias] = 0;
    rs[rs_separatealphablendenable] = 0; rs[rs_srcblendalpha] = blend_one; rs[rs_destblendalpha] = blend_zero; rs[rs_blendopalpha] = blendop_add;
    rs[rs_scissortestenable] = 0; rs[rs_multisamplemask] = 0xFFFFFFFF; rs[rs_pointsize] = 0x3F800000; rs[rs_pointsize_min] = 0x3F800000; rs[rs_pointsize_max] = 0x42800000;
    rs[rs_pointscale_a] = 0x3F800000;
    for (auto& s : st.ss) { s[ss_addressu] = s[ss_addressv] = s[ss_addressw] = taddress_wrap; s[ss_magfilter] = s[ss_minfilter] = texf_point; s[ss_mipfilter] = texf_none; s[ss_maxanisotropy] = 1; }
    for (uint32_t stage = 0; stage < 8; ++stage) {
        auto& t = st.tss[stage];
        t[tss_colorop] = stage == 0 ? 4 : 1; t[tss_colorarg1] = 2; t[tss_colorarg2] = 1; t[tss_alphaop] = stage == 0 ? 2 : 1; t[tss_alphaarg1] = 2; t[tss_alphaarg2] = 1;
        t[tss_texcoordindex] = stage; t[tss_resultarg] = 1; t[tss_colorarg0] = t[tss_alphaarg0] = 1;
    }
    auto identity = [](float* m) { std::memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; };
    identity(st.view); identity(st.projection); identity(st.world);
    for (auto& m : st.texture) identity(m);
}

// ---- lifecycle ---------------------------------------------------------------------------------------------------------------------------

void LayerImpl::recreate_auto_depth(uint32_t width, uint32_t height) {
    if (auto_depth_surface_) {
        if (auto* s = surfaces_.get(auto_depth_surface_)) { if (s->gpu) gpu_.destroy(s->gpu); }
        surfaces_.release(auto_depth_surface_);
        auto_depth_surface_ = 0;
    }
    if (!params_.depth_format) return;
    const FormatInfo& info = format_info(params_.depth_format);
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::depth; s.width = width; s.height = height; s.format = params_.depth_format;
    auto r = gpu_.create_texture(width, height, 1, info.valid ? info.native : gpu::Format::d24s8, true, false);
    s.gpu = r.value;
    auto_depth_surface_ = surfaces_.add(std::move(s));
}

Status LayerImpl::reset(const DeviceParams& p) {
    end_pass();
    const bool resized = p.back_width != params_.back_width || p.back_height != params_.back_height;
    params_ = p;
    if (p.back_width && p.back_height) {
        gpu_.resize(p.back_width, p.back_height);
        if (!backbuffer_surface_) {
            SurfaceObject s; s.alive = true; s.kind = SurfaceObject::Kind::backbuffer; s.width = p.back_width; s.height = p.back_height; s.format = p.back_format;
            backbuffer_surface_ = surfaces_.add(std::move(s));
        } else if (auto* s = surfaces_.get(backbuffer_surface_)) { s->width = p.back_width; s->height = p.back_height; s->format = p.back_format; }
        if (resized || !auto_depth_surface_) recreate_auto_depth(p.back_width, p.back_height);
        state_.render_targets[0] = backbuffer_surface_;
        state_.depth_stencil = auto_depth_surface_;
        state_.viewport = Viewport{0, 0, p.back_width, p.back_height, 0.0f, 1.0f};
    }
    return Status::ok;
}

Status LayerImpl::present() {
    end_pass();
    gpu_.present(params_.present_interval);
    ++stats_.frames;
    ++frame_;
    transient_cache_.clear();
    for (auto& b : retired_buffers_) gpu_.destroy(b);
    retired_buffers_.clear();
    for (auto& t : pending_destroy_textures_) gpu_.destroy(t);
    pending_destroy_textures_.clear();
    return Status::ok;
}

// ---- textures and surfaces -----------------------------------------------------------------------------------------------------------------

static gpu::Format render_target_format(const FormatInfo& info) {
    if (!info.valid) return gpu::Format::bgra8;
    if (info.depth) return info.native;
    switch (info.d3d) {
    case fmt_a8r8g8b8: case fmt_x8r8g8b8: return gpu::Format::bgra8;
    case fmt_r5g6b5: return gpu::Format::r5g6b5;
    case fmt_a16b16g16r16f: return gpu::Format::rgba16f;
    case fmt_a32b32g32r32f: return gpu::Format::rgba32f;
    case fmt_r16f: return gpu::Format::r16f;
    case fmt_r32f: return gpu::Format::r32f;
    case fmt_g16r16f: return gpu::Format::rg16f;
    case fmt_g32r32f: return gpu::Format::rg32f;
    case fmt_a8: return gpu::Format::a8;
    default: return gpu::Format::bgra8;
    }
}

Result<TextureId> LayerImpl::create_texture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) {
    const FormatInfo& info = format_info(format);
    if (!info.valid || !width || !height) { note_unsupported(("create_texture format " + std::to_string(format)).c_str()); return {Status::unsupported, {}}; }
    TextureObject t;
    t.alive = true; t.width = width; t.height = height; t.usage = usage; t.format = format; t.pool = pool; t.info = &info;
    t.render_target = (usage & (usage_rendertarget | usage_depthstencil)) != 0;
    t.levels = levels ? levels : mip_count(width, height);
    if (usage & usage_autogenmipmap) t.levels = std::max(t.levels, 1u);
    if (pool == pool_systemmem || pool == pool_scratch) {
        t.locks.resize(t.levels);
        return {Status::ok, TextureId{textures_.add(std::move(t))}};
    }
    t.gpu_format = t.render_target ? render_target_format(info) : gpu_format_for(info, gpu_.caps());
    auto r = gpu_.create_texture(width, height, t.render_target ? 1 : t.levels, t.gpu_format, t.render_target, false);
    if (!r) return {r.status, {}};
    t.gpu = r.value;
    t.locks.resize(t.levels);
    return {Status::ok, TextureId{textures_.add(std::move(t))}};
}

Result<TextureId> LayerImpl::create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) {
    const FormatInfo& info = format_info(format);
    if (!info.valid || !edge) { note_unsupported(("create_cube_texture format " + std::to_string(format)).c_str()); return {Status::unsupported, {}}; }
    TextureObject t;
    t.alive = true; t.cube = true; t.width = t.height = edge; t.usage = usage; t.format = format; t.pool = pool; t.info = &info;
    t.render_target = (usage & usage_rendertarget) != 0;
    t.levels = levels ? levels : mip_count(edge, edge);
    t.gpu_format = t.render_target ? render_target_format(info) : gpu_format_for(info, gpu_.caps());
    auto r = gpu_.create_texture(edge, edge, t.levels, t.gpu_format, t.render_target, true);
    if (!r) return {r.status, {}};
    t.gpu = r.value;
    t.locks.resize(size_t(t.levels) * 6);
    return {Status::ok, TextureId{textures_.add(std::move(t))}};
}

Result<SurfaceId> LayerImpl::create_render_target(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, bool) {
    (void)multisample;
    const FormatInfo& info = format_info(format);
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::render_target; s.width = width; s.height = height; s.format = format;
    auto r = gpu_.create_texture(width, height, 1, render_target_format(info), true, false);
    if (!r) return {r.status, {}};
    s.gpu = r.value;
    return {Status::ok, SurfaceId{surfaces_.add(std::move(s))}};
}

Result<SurfaceId> LayerImpl::create_depth_stencil(uint32_t width, uint32_t height, uint32_t format, uint32_t) {
    const FormatInfo& info = format_info(format);
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::depth; s.width = width; s.height = height; s.format = format;
    auto r = gpu_.create_texture(width, height, 1, info.valid ? info.native : gpu::Format::d24s8, true, false);
    if (!r) return {r.status, {}};
    s.gpu = r.value;
    return {Status::ok, SurfaceId{surfaces_.add(std::move(s))}};
}

Result<SurfaceId> LayerImpl::texture_surface(TextureId id, uint32_t level, uint32_t face) {
    TextureObject* t = textures_.get(id.value);
    if (!t) { bad_handle("texture_surface"); return {Status::invalid_argument, {}}; }
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::texture_level; s.texture = id.value; s.level = level; s.face = face;
    s.width = mip_dimension(t->width, level); s.height = mip_dimension(t->height, level); s.format = t->format;
    return {Status::ok, SurfaceId{surfaces_.add(std::move(s))}};
}

Result<SurfaceId> LayerImpl::back_buffer(uint32_t, uint32_t) {
    if (!backbuffer_surface_) {
        SurfaceObject s; s.alive = true; s.kind = SurfaceObject::Kind::backbuffer; s.width = params_.back_width; s.height = params_.back_height; s.format = params_.back_format;
        backbuffer_surface_ = surfaces_.add(std::move(s));
    }
    return {Status::ok, SurfaceId{backbuffer_surface_}};
}

void LayerImpl::destroy(TextureId id) {
    TextureObject* t = textures_.get(id.value);
    if (!t) return;
    for (auto& r : state_.textures) if (r == id.value) r = 0;
    if (t->gpu) pending_destroy_textures_.push_back(t->gpu);
    textures_.release(id.value);
}

void LayerImpl::destroy(BufferId id) {
    BufferObject* b = buffers_.get(id.value);
    if (!b) return;
    for (auto& st : state_.streams) if (st.buffer == id.value) st = {};
    if (state_.index_buffer == id.value) state_.index_buffer = 0;
    if (b->gpu) retired_buffers_.push_back(b->gpu);
    buffers_.release(id.value);
}

void LayerImpl::destroy(SurfaceId id) {
    SurfaceObject* s = surfaces_.get(id.value);
    if (!s || id.value == backbuffer_surface_ || id.value == auto_depth_surface_) return;
    for (auto& r : state_.render_targets) if (r == id.value) r = 0;
    if (state_.depth_stencil == id.value) state_.depth_stencil = 0;
    if (s->gpu) pending_destroy_textures_.push_back(s->gpu);
    surfaces_.release(id.value);
}

void LayerImpl::destroy(DeclarationId id) {
    if (state_.declaration == id.value) state_.declaration = 0;
    declarations_.release(id.value);
}
void LayerImpl::destroy(ShaderId id) {
    if (state_.vertex_shader == id.value) state_.vertex_shader = 0;
    if (state_.pixel_shader == id.value) state_.pixel_shader = 0;
    shaders_.release(id.value);
}
void LayerImpl::destroy(StateBlockId id) { blocks_.release(id.value); }

// ---- buffers --------------------------------------------------------------------------------------------------------------------------------

Result<BufferId> LayerImpl::create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) {
    BufferObject b;
    b.alive = true; b.bytes = bytes; b.usage = usage; b.format_or_fvf = fvf; b.pool = pool;
    b.shadow.assign(bytes, std::byte{0});
    return {Status::ok, BufferId{buffers_.add(std::move(b))}};
}

Result<BufferId> LayerImpl::create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) {
    BufferObject b;
    b.alive = true; b.index = true; b.bytes = bytes; b.usage = usage; b.format_or_fvf = format; b.pool = pool;
    b.shadow.assign(bytes, std::byte{0});
    return {Status::ok, BufferId{buffers_.add(std::move(b))}};
}

Result<LockedRegion> LayerImpl::lock_buffer(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags) {
    BufferObject* b = buffers_.get(id.value);
    if (!b) { bad_handle("lock_buffer"); return {Status::invalid_argument, {}}; }
    if (offset > b->bytes) return {Status::invalid_argument, {}};
    if (!bytes) bytes = b->bytes - offset;
    if (uint64_t(offset) + bytes > b->bytes) return {Status::invalid_argument, {}};
    b->locked = true; b->lock_offset = offset; b->lock_bytes = bytes; b->lock_flags = flags;
    return {Status::ok, LockedRegion{b->shadow.data() + offset, bytes, 0}};
}

Status LayerImpl::unlock_buffer(BufferId id) {
    BufferObject* b = buffers_.get(id.value);
    if (!b) { bad_handle("unlock_buffer"); return Status::invalid_argument; }
    if (!b->locked) return Status::invalid_argument;
    b->locked = false;
    if (b->lock_flags & lock_readonly) return Status::ok;
    ++b->version;
    if (b->score_frame != frame_) { const uint32_t decay = (frame_ - b->score_frame) / 4; b->score = b->score > decay ? b->score - decay : 0; b->score_frame = frame_; }
    if (b->used_since_update) b->score = std::min(b->score + 2, 8u);   // rewritten after having been drawn from: it behaves as a dynamic buffer
    b->used_since_update = false;
    return Status::ok;
}

// ---- texture locks --------------------------------------------------------------------------------------------------------------------------

Result<LockedRegion> LayerImpl::lock_texture(TextureId id, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) {
    TextureObject* t = textures_.get(id.value);
    if (!t || level >= t->levels || face >= (t->cube ? 6u : 1u)) { bad_handle("lock_texture"); return {Status::invalid_argument, {}}; }
    if (t->render_target) { note_unsupported("lock of a render target texture"); return {Status::unsupported, {}}; }
    auto& lock = t->locks[size_t(face) * t->levels + level];
    if (lock.active) return {Status::invalid_argument, {}};
    const FormatInfo& info = *t->info;
    const uint32_t lw = mip_dimension(t->width, level), lh = mip_dimension(t->height, level);
    uint32_t x = 0, y = 0, w = lw, h = lh;
    if (rect4) {
        x = uint32_t(std::max(0, rect4[0])); y = uint32_t(std::max(0, rect4[1]));
        w = uint32_t(std::max(0, rect4[2] - rect4[0])); h = uint32_t(std::max(0, rect4[3] - rect4[1]));
        if (x + w > lw) w = lw - std::min(x, lw);
        if (y + h > lh) h = lh - std::min(y, lh);
    }
    lock.active = true; lock.flags = flags;
    lock.rect[0] = x; lock.rect[1] = y; lock.rect[2] = w; lock.rect[3] = h;
    lock.pitch = level_pitch(info, w);
    const size_t size = level_bytes(info, w, h);
    if (lock.staging.size() != size) lock.staging.assign(size, std::byte{0});
    return {Status::ok, LockedRegion{lock.staging.data(), lock.pitch, 0}};
}

Status LayerImpl::unlock_texture(TextureId id, uint32_t level, uint32_t face) {
    TextureObject* t = textures_.get(id.value);
    if (!t || level >= t->levels || face >= (t->cube ? 6u : 1u)) { bad_handle("unlock_texture"); return Status::invalid_argument; }
    auto& lock = t->locks[size_t(face) * t->levels + level];
    if (!lock.active) return Status::invalid_argument;
    lock.active = false;
    if ((lock.flags & lock_readonly) || !t->gpu) return Status::ok;   // system-memory textures keep their staging as the contents
    const FormatInfo& info = *t->info;
    const uint32_t w = lock.rect[2], h = lock.rect[3];
    if (!w || !h) { lock.staging.clear(); return Status::ok; }
    const uint32_t lw = mip_dimension(t->width, level), lh = mip_dimension(t->height, level);
    const bool whole = lock.rect[0] == 0 && lock.rect[1] == 0 && w == lw && h == lh;
    const uint32_t rect[4] = {lock.rect[0], lock.rect[1], w, h};
    const uint8_t* src = reinterpret_cast<const uint8_t*>(lock.staging.data());
    const bool same_layout = info.native == t->gpu_format && info.convert != Convert::x8_opaque;
    if (same_layout) {
        gpu_.update_texture(t->gpu, level, face, whole ? nullptr : rect, Bytes(lock.staging.data(), lock.staging.size()), lock.pitch);
        stats_.texture_upload_bytes += lock.staging.size();
    } else {
        uint32_t out_pitch = 0;
        convert_pixels(info, t->gpu_format, src, lock.pitch, w, h, scratch_, out_pitch);
        gpu_.update_texture(t->gpu, level, face, whole ? nullptr : rect, Bytes(scratch_.data(), scratch_.size()), out_pitch);
        stats_.texture_upload_bytes += scratch_.size();
        stats_.texture_converted_bytes += scratch_.size();
    }
    ++stats_.texture_uploads;
    if (!options_.keep_texture_copies) { lock.staging.clear(); lock.staging.shrink_to_fit(); }
    return Status::ok;
}

// ---- declarations and shaders ---------------------------------------------------------------------------------------------------------------

Result<DeclarationId> LayerImpl::create_vertex_declaration(std::span<const uint8_t> bytes) {
    Declaration d;
    for (size_t pos = 0; pos + 8 <= bytes.size(); pos += 8) {
        const uint8_t* e = bytes.data() + pos;
        uint16_t stream, offset;
        std::memcpy(&stream, e, 2); std::memcpy(&offset, e + 2, 2);
        if (stream == 0xFF) break;   // D3DDECL_END
        DeclElement el{uint8_t(stream), offset, e[4], e[6], e[7]};
        d.elements.push_back(el);
        d.attributes.push_back(gpu::VertexAttribute{el.stream, el.offset, el.type, el.usage, el.usage_index});
        d.stream_mask |= 1u << (el.stream & 15);
        if (el.type == dt_d3dcolor) d.bgra_attributes |= 1u << shader::bgfx_attribute_for(el.usage, el.usage_index);
    }
    return {Status::ok, DeclarationId{declarations_.add(std::move(d))}};
}

static uint64_t hash_words(std::span<const uint32_t> w) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t v : w) { h ^= v; h *= 0x100000001b3ull; }
    return h;
}

Result<ShaderId> LayerImpl::create_vertex_shader(std::span<const uint32_t> bytecode) {
    ShaderObject s;
    s.vertex = true; s.bytecode.assign(bytecode.begin(), bytecode.end()); s.hash = hash_words(bytecode);
    s.valid = shader::decode(bytecode, s.program, s.error) && s.program.stage == shader::Stage::vertex;
    if (!s.valid) { ++stats_.shader_failures; log("vertex shader rejected: " + s.error); }
    return {Status::ok, ShaderId{shaders_.add(std::move(s))}};
}

Result<ShaderId> LayerImpl::create_pixel_shader(std::span<const uint32_t> bytecode) {
    ShaderObject s;
    s.vertex = false; s.bytecode.assign(bytecode.begin(), bytecode.end()); s.hash = hash_words(bytecode);
    s.valid = shader::decode(bytecode, s.program, s.error) && s.program.stage == shader::Stage::pixel;
    if (!s.valid) { ++stats_.shader_failures; log("pixel shader rejected: " + s.error); }
    return {Status::ok, ShaderId{shaders_.add(std::move(s))}};
}

Status LayerImpl::stretch_rect(SurfaceId, const int32_t*, SurfaceId, const int32_t*, uint32_t) {
    ++stats_.stretch_rects;
    note_unsupported("stretch_rect");
    return Status::unsupported;
}

}  // namespace nue::d3d9
