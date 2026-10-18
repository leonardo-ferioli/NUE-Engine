// Layer: draws. One gpu::DrawDesc is built per draw from the state shadow: program selection, vertex/index sources (static GPU buffers or transient
// copies of the ranges the draw reads), textures and samplers, constants, blend/depth/raster state.
#include "layer/layer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nue::d3d9 {
using namespace k;

namespace {
float as_float(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

gpu::Compare compare_of(uint32_t v) {
    switch (v) {
    case cmp_never: return gpu::Compare::never; case cmp_less: return gpu::Compare::less; case cmp_equal: return gpu::Compare::equal;
    case cmp_lessequal: return gpu::Compare::less_equal; case cmp_greater: return gpu::Compare::greater; case cmp_notequal: return gpu::Compare::not_equal;
    case cmp_greaterequal: return gpu::Compare::greater_equal; default: return gpu::Compare::always;
    }
}
gpu::StencilOp stencil_op_of(uint32_t v) {
    switch (v) {
    case stencil_zero: return gpu::StencilOp::zero; case stencil_replace: return gpu::StencilOp::replace; case stencil_incrsat: return gpu::StencilOp::incr_sat;
    case stencil_decrsat: return gpu::StencilOp::decr_sat; case stencil_invert: return gpu::StencilOp::invert; case stencil_incr: return gpu::StencilOp::incr;
    case stencil_decr: return gpu::StencilOp::decr; default: return gpu::StencilOp::keep;
    }
}
gpu::BlendFactor blend_factor_of(uint32_t v, bool alpha_pair_dst, bool& both) {
    both = false;
    switch (v) {
    case blend_zero: return gpu::BlendFactor::zero; case blend_one: return gpu::BlendFactor::one; case blend_srccolor: return gpu::BlendFactor::src_color;
    case blend_invsrccolor: return gpu::BlendFactor::inv_src_color; case blend_srcalpha: return gpu::BlendFactor::src_alpha; case blend_invsrcalpha: return gpu::BlendFactor::inv_src_alpha;
    case blend_destalpha: return gpu::BlendFactor::dst_alpha; case blend_invdestalpha: return gpu::BlendFactor::inv_dst_alpha; case blend_destcolor: return gpu::BlendFactor::dst_color;
    case blend_invdestcolor: return gpu::BlendFactor::inv_dst_color; case blend_srcalphasat: return gpu::BlendFactor::src_alpha_sat;
    case blend_bothsrcalpha: both = true; return alpha_pair_dst ? gpu::BlendFactor::inv_src_alpha : gpu::BlendFactor::src_alpha;
    case blend_bothinvsrcalpha: both = true; return alpha_pair_dst ? gpu::BlendFactor::src_alpha : gpu::BlendFactor::inv_src_alpha;
    case blend_blendfactor: return gpu::BlendFactor::constant; case blend_invblendfactor: return gpu::BlendFactor::inv_constant;
    default: return gpu::BlendFactor::one;
    }
}
gpu::BlendOp blend_op_of(uint32_t v) {
    switch (v) { case blendop_subtract: return gpu::BlendOp::subtract; case blendop_revsubtract: return gpu::BlendOp::reverse_subtract; case blendop_min: return gpu::BlendOp::min;
                 case blendop_max: return gpu::BlendOp::max; default: return gpu::BlendOp::add; }
}
uint8_t address_of(uint32_t v) {
    switch (v) { case taddress_mirror: return 1; case taddress_clamp: return 2; case taddress_border: return 3; case taddress_mirroronce: return 4; default: return 0; }
}
uint32_t index_count_for(uint32_t primitive, uint32_t n) {
    switch (primitive) {
    case pt_pointlist: return n; case pt_linelist: return n * 2; case pt_linestrip: return n + 1; case pt_trianglelist: return n * 3;
    case pt_trianglestrip: case pt_trianglefan: return n + 2; default: return 0;
    }
}
gpu::Topology topology_of(uint32_t primitive) {
    switch (primitive) {
    case pt_pointlist: return gpu::Topology::points; case pt_linelist: return gpu::Topology::lines; case pt_linestrip: return gpu::Topology::line_strip;
    case pt_trianglestrip: return gpu::Topology::triangle_strip; default: return gpu::Topology::triangles;
    }
}

// The declaration Direct3D 9 builds implicitly for a fixed vertex format (D3DFVF_*).
Declaration declaration_from_fvf(uint32_t fvf) {
    Declaration d;
    uint16_t offset = 0;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t index, uint16_t size) {
        DeclElement e{0, offset, type, usage, index};
        d.elements.push_back(e);
        d.attributes.push_back(gpu::VertexAttribute{0, offset, type, usage, index});
        if (type == dt_d3dcolor) d.bgra_attributes |= 1u << shader::bgfx_attribute_for(usage, index);
        if (type == dt_ubyte4) d.uint_attributes |= 1u << shader::bgfx_attribute_for(usage, index);
        offset = uint16_t(offset + size);
    };
    const uint32_t pos = fvf & fvf_posmask;
    uint32_t blend = 0;
    if (pos == fvf_xyzrhw) add(dt_float4, du_positiont, 0, 16);
    else if (pos == fvf_xyzw) add(dt_float4, du_position, 0, 16);
    else if (pos >= fvf_xyz) { add(dt_float3, du_position, 0, 12); if (pos >= fvf_xyzb1) blend = ((pos - fvf_xyz) >> 1); }
    if (blend) { const bool ubyte = (fvf & fvf_lastbeta_ubyte4) != 0; const uint32_t floats = ubyte ? blend - 1 : blend;
                 if (floats) add(uint8_t(dt_float1 + floats - 1), du_blendweight, 0, uint16_t(floats * 4));
                 if (ubyte) add(dt_ubyte4, du_blendindices, 0, 4); }
    if (fvf & fvf_normal) add(dt_float3, du_normal, 0, 12);
    if (fvf & fvf_psize) add(dt_float1, du_psize, 0, 4);
    if (fvf & fvf_diffuse) add(dt_d3dcolor, du_color, 0, 4);
    if (fvf & fvf_specular) add(dt_d3dcolor, du_color, 1, 4);
    const uint32_t tex_count = (fvf & fvf_texcountmask) >> fvf_texcountshift;
    for (uint32_t i = 0; i < tex_count; ++i) {
        const uint32_t size_code = (fvf >> (16 + 2 * i)) & 3;   // 0: 2D, 1: 3D, 2: 4D, 3: 1D
        const uint8_t type = size_code == 0 ? dt_float2 : size_code == 1 ? dt_float3 : size_code == 2 ? dt_float4 : dt_float1;
        add(type, du_texcoord, uint8_t(i), uint16_t((type + 1) * 4));
    }
    d.stream_mask = 1;
    return d;
}
}  // namespace

// ---- buffers --------------------------------------------------------------------------------------------------------------------------------

bool LayerImpl::buffer_is_dynamic(BufferObject& b) {
    if (b.usage & usage_dynamic) return true;
    if (b.score_frame != frame_) { const uint32_t decay = (frame_ - b.score_frame) / 4; b.score = b.score > decay ? b.score - decay : 0; b.score_frame = frame_; }
    return b.score >= 2;
}

gpu::Buffer LayerImpl::static_gpu_buffer(BufferObject& b) {
    if (b.gpu && b.gpu_version == b.version) { b.used_since_update = true; return b.gpu; }
    if (b.gpu) retired_buffers_.push_back(b.gpu);
    auto r = gpu_.create_static_buffer(Bytes(b.shadow.data(), b.shadow.size()), !b.index ? gpu::BufferKind::vertex : b.format_or_fvf == fmt_index32 ? gpu::BufferKind::index32 : gpu::BufferKind::index16);
    b.gpu = r ? r.value : gpu::Buffer{};
    b.gpu_version = b.version;
    b.used_since_update = true;
    ++stats_.static_buffer_uploads;
    stats_.static_buffer_bytes += b.shadow.size();
    return b.gpu;
}

gpu::Buffer LayerImpl::transient_copy(BufferObject& source, uint32_t byte_offset, uint32_t bytes, uint32_t stride, uint32_t buffer_id) {
    source.used_since_update = true;
    const TransientKey key{buffer_id, source.version, byte_offset, bytes};
    auto cached = transient_cache_.find(key);
    if (cached != transient_cache_.end()) { ++stats_.transient_dedupe_hits; return cached->second; }
    MutableBytes out;
    auto r = gpu_.allocate_transient(bytes, stride, false, out);
    if (!r) return {};
    const uint64_t available = byte_offset < source.shadow.size() ? source.shadow.size() - byte_offset : 0;
    const size_t n = size_t(std::min<uint64_t>(available, bytes));
    std::memcpy(out.data(), source.shadow.data() + byte_offset, n);
    if (n < bytes) std::memset(out.data() + n, 0, bytes - n);
    ++stats_.transient_copies;
    stats_.transient_bytes += bytes;
    transient_cache_[key] = r.value;
    return r.value;
}

// ---- programs -------------------------------------------------------------------------------------------------------------------------------

const shader::TranslateResult* LayerImpl::translate(ShaderObject& s, uint32_t, const shader::TranslateOptions& options, uint64_t options_key) {
    const TranslationKey key{s.hash, options_key};
    auto it = translations_.find(key);
    if (it != translations_.end()) return it->second.get();
    auto result = std::make_unique<shader::TranslateResult>();
    ++stats_.shader_translations;
    if (!shader::translate_to_spirv(s.program, options, *result)) {
        ++stats_.shader_failures;
        log(std::string(s.vertex ? "vertex" : "pixel") + " shader translation failed: " + result->error);
        result.reset();
    }
    const shader::TranslateResult* raw = result.get();
    translations_[key] = std::move(result);
    return raw;
}

LayerImpl::DrawProgram LayerImpl::select_program(const Declaration* declaration, const DeviceState& st, ShaderObject* vs, ShaderObject* ps, bool vertex_fog_from_fixed_function) {
    DrawProgram out;
    if (!vs || !ps || !vs->valid || !ps->valid) return out;
    shader::TranslateOptions vo;
    vo.bgra_attributes = declaration ? declaration->bgra_attributes : 0;
    vo.uint_attributes = declaration ? declaration->uint_attributes : 0;
    vo.sint_attributes = declaration ? declaration->sint_attributes : 0;
    shader::TranslateOptions po;
    uint64_t po_key = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const TextureObject* t = textures_.get(st.textures[i]);
        po.sampler_dim[i] = t && t->cube ? 3 : 1;
        po_key |= uint64_t(po.sampler_dim[i] == 3 ? 1 : 0) << i;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t flags = st.tss[i][tss_texturetransformflags];
        // D3DTTFF_PROJECTED divides by the last coordinate: the third when three coordinates are passed, otherwise the fourth
        po.projected[i] = (flags & 0x100) ? ((flags & 0xFF) == 3 ? 3 : 4) : 0;
        po_key |= uint64_t(po.projected[i] == 4 ? 2 : po.projected[i] == 3 ? 1 : 0) << (16 + 2 * i);
    }
    if (st.rs[rs_alphatestenable] && st.rs[rs_alphafunc] != cmp_always) po.alpha_func = uint8_t(st.rs[rs_alphafunc]);
    if (st.rs[rs_fogenable]) {
        if (st.rs[rs_fogtablemode] == 0 || vertex_fog_from_fixed_function) po.fog_mode = 1;
        else note_unsupported("table fog with a pixel shader");
    }
    po_key |= uint64_t(po.alpha_func) << 32 | uint64_t(po.fog_mode) << 36;
    out.vertex = translate(*vs, st.vertex_shader, vo, (uint64_t(vo.bgra_attributes) + 1) * 0x9E3779B97F4A7C15ull ^ (uint64_t(vo.uint_attributes) + 3) * 0xC2B2AE3D27D4EB4Full ^ (uint64_t(vo.sint_attributes) + 7) * 0x165667B19E3779F9ull);
    out.pixel = translate(*ps, st.pixel_shader, po, po_key | (1ull << 41));
    if (!out.vertex || !out.pixel) return out;
    const uint64_t key = (vs->hash * 0x9E3779B97F4A7C15ull) ^ (ps->hash + 0x7F4A7C15ull + (vs->hash << 6)) ^ (uint64_t(vo.bgra_attributes) << 7) ^ (uint64_t(vo.uint_attributes) * 0x2545F4914F6CDD1Dull) ^ (uint64_t(vo.sint_attributes) * 0x9E3779B97F4A7C15ull) ^ (po_key * 0xC2B2AE3D27D4EB4Full);
    auto found = programs_.find(key);
    if (found == programs_.end()) {
        gpu::ProgramDesc desc{key, out.vertex, out.pixel};
        auto r = gpu_.create_program(desc);
        if (!r) return out;
        found = programs_.emplace(key, r.value).first;
        ++stats_.programs;
    }
    out.program = found->second;
    out.ok = true;
    return out;
}

void LayerImpl::fill_vertex_fixup(float* fix, const Attachment& a) const {
    const Viewport& v = state_.viewport;
    const float W = float(std::max(a.width, 1u)), H = float(std::max(a.height, 1u));
    float vx = float(v.x), vy = float(v.y), vw = float(v.width), vh = float(v.height);
    if (!v.width || !v.height) { vx = vy = 0; vw = W; vh = H; }
    fix[0] = vw / W;                                  // scale x
    fix[1] = vh / H;                                  // scale y
    fix[2] = (2.0f * vx + vw) / W - 1.0f + 1.0f / W;   // bias x, plus the half-pixel shift of Direct3D 9
    fix[3] = 1.0f - (2.0f * vy + vh) / H - 1.0f / H;   // bias y
    fix[4] = v.max_z - v.min_z;                       // scale z
    fix[5] = v.min_z;                                 // bias z
    fix[6] = fix[7] = 0.0f;
}

void LayerImpl::fill_pixel_fixup(float* fix) const {
    const auto& rs = state_.rs;
    fix[0] = float(rs[rs_alpharef] & 0xFF);
    fix[1] = as_float(rs[rs_fogstart]); fix[2] = as_float(rs[rs_fogend]); fix[3] = as_float(rs[rs_fogdensity]);
    const uint32_t c = rs[rs_fogcolor];
    fix[4] = float((c >> 16) & 0xFF) / 255.0f; fix[5] = float((c >> 8) & 0xFF) / 255.0f; fix[6] = float(c & 0xFF) / 255.0f; fix[7] = float(c >> 24) / 255.0f;
}

void LayerImpl::collect_constants(std::vector<std::byte>& out, const Float4* regs, uint32_t available, const shader::TranslateResult& tr, const float* fix) {
    const uint32_t count = tr.constant_registers;
    out.resize(size_t(2 + count) * 16);
    std::memcpy(out.data(), fix, 32);
    const uint32_t n = std::min(count, available);
    std::memcpy(out.data() + 32, regs, size_t(n) * 16);
    if (n < count) std::memset(out.data() + 32 + size_t(n) * 16, 0, size_t(count - n) * 16);
    stats_.constants_bytes += out.size();
}

// ---- state translation ----------------------------------------------------------------------------------------------------------------------

gpu::SamplerDesc LayerImpl::sampler_desc(uint32_t slot) const {
    const auto& s = state_.ss[slot];
    gpu::SamplerDesc d;
    const uint32_t minf = s[ss_minfilter], magf = s[ss_magfilter], mipf = s[ss_mipfilter];
    d.min = minf == texf_anisotropic ? 2 : minf == texf_point || minf == texf_none ? 0 : 1;
    d.mag = (magf == texf_point || magf == texf_none) ? 0 : 1;
    d.mip = mipf == texf_linear ? 1 : 0;
    d.use_mips = mipf != texf_none;
    d.address_u = address_of(s[ss_addressu]); d.address_v = address_of(s[ss_addressv]); d.address_w = address_of(s[ss_addressw]);
    d.max_aniso = uint8_t(std::clamp<uint32_t>(s[ss_maxanisotropy], 1, 16));
    d.lod_bias = as_float(s[ss_mipmaplodbias]);
    d.border_argb = s[ss_bordercolor];
    return d;
}

void LayerImpl::fill_states(gpu::DrawDesc& d) const {
    const auto& rs = state_.rs;
    d.blend.enable = rs[rs_alphablendenable] != 0;
    bool both = false;
    d.blend.src = blend_factor_of(rs[rs_srcblend], false, both);
    d.blend.dst = blend_factor_of(rs[rs_destblend], true, both);
    d.blend.op = blend_op_of(rs[rs_blendop]);
    if (rs[rs_srcblend] == blend_bothsrcalpha) { d.blend.src = gpu::BlendFactor::src_alpha; d.blend.dst = gpu::BlendFactor::inv_src_alpha; }
    if (rs[rs_srcblend] == blend_bothinvsrcalpha) { d.blend.src = gpu::BlendFactor::inv_src_alpha; d.blend.dst = gpu::BlendFactor::src_alpha; }
    if (rs[rs_separatealphablendenable]) {
        d.blend.src_alpha = blend_factor_of(rs[rs_srcblendalpha], false, both);
        d.blend.dst_alpha = blend_factor_of(rs[rs_destblendalpha], true, both);
        d.blend.op_alpha = blend_op_of(rs[rs_blendopalpha]);
    } else {
        d.blend.src_alpha = d.blend.src; d.blend.dst_alpha = d.blend.dst; d.blend.op_alpha = d.blend.op;
    }
    d.blend.write_mask = uint8_t(rs[rs_colorwriteenable] & 0xF);
    d.blend.constant_argb = rs[rs_blendfactor];
    const bool has_depth = bool(pass_.depth_stencil);
    d.depth_stencil.depth_test = rs[rs_zenable] != 0 && has_depth;
    d.depth_stencil.depth_write = rs[rs_zwriteenable] != 0 && has_depth;
    d.depth_stencil.depth_func = compare_of(rs[rs_zfunc]);
    d.depth_stencil.stencil = rs[rs_stencilenable] != 0 && has_depth;
    d.depth_stencil.stencil_func = compare_of(rs[rs_stencilfunc]);
    d.depth_stencil.stencil_fail = stencil_op_of(rs[rs_stencilfail]);
    d.depth_stencil.stencil_depth_fail = stencil_op_of(rs[rs_stencilzfail]);
    d.depth_stencil.stencil_pass = stencil_op_of(rs[rs_stencilpass]);
    d.depth_stencil.stencil_ref = uint8_t(rs[rs_stencilref]);
    d.depth_stencil.stencil_read_mask = uint8_t(rs[rs_stencilmask]);
    d.depth_stencil.stencil_write_mask = uint8_t(rs[rs_stencilwritemask]);
    d.raster.cull = rs[rs_cullmode] == cull_none ? gpu::Cull::none : rs[rs_cullmode] == cull_cw ? gpu::Cull::cw : gpu::Cull::ccw;
    d.raster.wireframe = rs[rs_fillmode] == fill_wireframe;
    d.raster.depth_bias = as_float(rs[rs_depthbias]);
    d.raster.slope_scale_bias = as_float(rs[rs_slopescaledepthbias]);
}

gpu::Texture LayerImpl::dummy_texture(bool cube) {
    gpu::Texture& slot = cube ? dummy_cube_ : dummy_2d_;
    if (slot) return slot;
    auto r = gpu_.create_texture(1, 1, 1, gpu::Format::rgba8, false, cube);
    if (!r) return {};
    slot = r.value;
    const uint8_t texel[4] = {0, 0, 0, 255};
    for (uint32_t face = 0; face < (cube ? 6u : 1u); ++face) gpu_.update_texture(slot, 0, face, nullptr, Bytes(reinterpret_cast<const std::byte*>(texel), 4), 4);
    return slot;
}

// ---- the draw -------------------------------------------------------------------------------------------------------------------------------

void LayerImpl::draw_indexed(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count) {
    do_draw(primitive, base_vertex, min_vertex, vertex_count, start_index, primitive_count, nullptr, 0, 0);
}

void LayerImpl::draw_up(uint32_t primitive, uint32_t primitive_count, Bytes vertices, uint32_t stride) {
    ++stats_.draw_up;
    do_draw(primitive, 0, 0, 0, 0, primitive_count, reinterpret_cast<const uint8_t*>(vertices.data()), uint32_t(vertices.size()), stride);
}

void LayerImpl::do_draw(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count,
                        const uint8_t* up_vertices, uint32_t up_bytes, uint32_t up_stride) {
    ++stats_.draws;
    if (stats_.draws - 1 < options_.debug_draw_first || stats_.draws - 1 > options_.debug_draw_last) return;
    if (primitive < 8) ++stats_.draw_by_primitive[primitive];
    if (options_.debug_draw_first == options_.debug_draw_last && options_.log) {
        char text[512];
        std::snprintf(text, sizeof text, "DRAW %llu: prim %u base %d min %u count %u start %u primcount %u up %d stride %u vs %u ps %u decl %u fvf %x ib %u streams: %u/%u/%u %u/%u/%u",
                      (unsigned long long)(stats_.draws - 1), primitive, base_vertex, min_vertex, vertex_count, start_index, primitive_count, up_vertices ? 1 : 0, up_stride, state_.vertex_shader, state_.pixel_shader,
                      state_.declaration, state_.fvf, state_.index_buffer, state_.streams[0].buffer, state_.streams[0].offset, state_.streams[0].stride, state_.streams[1].buffer, state_.streams[1].offset, state_.streams[1].stride);
        log(text);
        std::snprintf(text, sizeof text, "  states: z %u zw %u zfunc %u cull %u ablend %u src %u dst %u atest %u afunc %u aref %u cw %u stencil %u clipplane %u fog %u depthbias %x rt0 %u ds %u vp %u,%u %ux%u %f-%f",
                      state_.rs[rs_zenable], state_.rs[rs_zwriteenable], state_.rs[rs_zfunc], state_.rs[rs_cullmode], state_.rs[rs_alphablendenable], state_.rs[rs_srcblend], state_.rs[rs_destblend], state_.rs[rs_alphatestenable],
                      state_.rs[rs_alphafunc], state_.rs[rs_alpharef], state_.rs[rs_colorwriteenable], state_.rs[rs_stencilenable], state_.rs[rs_clipplaneenable], state_.rs[rs_fogenable], state_.rs[rs_depthbias],
                      state_.render_targets[0], state_.depth_stencil, state_.viewport.x, state_.viewport.y, state_.viewport.width, state_.viewport.height, state_.viewport.min_z, state_.viewport.max_z);
        log(text);
        if (const Declaration* dd = declarations_.get(state_.declaration)) for (const auto& e : dd->elements) {
            std::snprintf(text, sizeof text, "  element stream %u offset %u type %u usage %u/%u", e.stream, e.offset, e.type, e.usage, e.usage_index);
            log(text);
        }
        for (uint32_t i = 0; i < 4; ++i) if (const TextureObject* t = textures_.get(state_.textures[i])) {
            std::snprintf(text, sizeof text, "  tex%u: id %u %ux%u levels %u usage %u fmt %u cube %d | filter min %u mag %u mip %u addr %u/%u | tss flags %x tci %x", i, state_.textures[i], t->width, t->height, t->levels, t->usage, t->format, t->cube ? 1 : 0,
                          state_.ss[i][ss_minfilter], state_.ss[i][ss_magfilter], state_.ss[i][ss_mipfilter], state_.ss[i][ss_addressu], state_.ss[i][ss_addressv], state_.tss[i][tss_texturetransformflags], state_.tss[i][tss_texcoordindex]);
            log(text);
        }
        if (auto* v = shaders_.get(state_.vertex_shader)) log(std::string("VS:") + char(10) + shader::disassemble(v->program));
        if (auto* p = shaders_.get(state_.pixel_shader)) log(std::string("PS:") + char(10) + shader::disassemble(p->program));
    }
    const bool up = up_vertices != nullptr;
    if (!primitive_count || primitive < 1 || primitive > 6) return;

    ShaderObject* vs = shaders_.get(state_.vertex_shader);
    ShaderObject* ps = shaders_.get(state_.pixel_shader);
    const bool ff_vertex = vs == nullptr, ff_pixel = ps == nullptr;
    ensure_pass();
    if (!pass_open_) { ++stats_.draws_skipped; return; }

    Declaration fvf_decl;
    const Declaration* decl = declarations_.get(state_.declaration);
    if (!decl && state_.fvf) {
        auto cached = fvf_declarations_.find(state_.fvf);
        if (cached == fvf_declarations_.end()) cached = fvf_declarations_.emplace(state_.fvf, declaration_from_fvf(state_.fvf)).first;
        decl = &cached->second;
    }
    if (!decl) { ++stats_.draws_skipped; return; }
    if (ff_vertex || ff_pixel) {
        ++stats_.draws_fixed_function;
        if (options_.log && stats_.draws_fixed_function <= 4) { char t[96]; std::snprintf(t, sizeof t, "fixed-function draw number %llu (draw index %llu)", (unsigned long long)stats_.draws_fixed_function, (unsigned long long)(stats_.draws - 1)); log(t); }
        if (state_.rs[rs_lighting] && ff_vertex) note_unsupported("fixed-function lighting (drawn unlit with the vertex colours)");
        if (ff_vertex) vs = fixed_function_vertex(*decl);
        if (ff_pixel) ps = fixed_function_pixel();
    }

    DrawProgram program = select_program(decl, state_, vs, ps, ff_vertex);
    if (!program.ok) { ++stats_.draws_skipped; return; }

    gpu::DrawDesc d;
    d.program = program.program;
    d.topology = topology_of(primitive);
    d.layout = std::span<const gpu::VertexAttribute>(decl->attributes);
    const bool fan = primitive == pt_trianglefan;
    const uint32_t index_count = index_count_for(primitive, primitive_count);

    // ---- vertex and index sources
    if (up) {
        if (!up_stride) { ++stats_.draws_skipped; return; }
        const uint32_t vertices = std::min(up_bytes / up_stride, std::max(index_count, 1u));
        const uint32_t bytes = std::max(vertices * up_stride, 1u);
        MutableBytes out;
        auto r = gpu_.allocate_transient(bytes, up_stride, false, out);
        if (!r) { ++stats_.draws_skipped; return; }
        std::memcpy(out.data(), up_vertices, std::min<uint32_t>(bytes, up_bytes));
        ++stats_.transient_copies; stats_.transient_bytes += bytes;
        d.vertex_buffer[0] = r.value; d.vertex_stride[0] = up_stride;
        d.vertex_count = vertices;
        if (fan) {   // triangle fan -> triangle list through a generated index buffer
            ++stats_.fan_conversions;
            const uint32_t triangles = primitive_count;
            const bool wide = vertices > 65535;
            const uint32_t isz = wide ? 4 : 2;
            MutableBytes iout;
            auto ir = gpu_.allocate_transient(triangles * 3 * isz, isz, true, iout);
            if (!ir) { ++stats_.draws_skipped; return; }
            for (uint32_t t = 0; t < triangles; ++t) {
                const uint32_t tri[3] = {0, t + 1, t + 2};
                for (int c = 0; c < 3; ++c) {
                    if (wide) { const uint32_t v = tri[c]; std::memcpy(iout.data() + size_t(t * 3 + c) * 4, &v, 4); }
                    else { const uint16_t v = uint16_t(tri[c]); std::memcpy(iout.data() + size_t(t * 3 + c) * 2, &v, 2); }
                }
            }
            d.index_buffer = ir.value; d.index32 = wide; d.first_index = 0; d.index_count = triangles * 3;
            d.topology = gpu::Topology::triangles;
        }
    } else {
        BufferObject* ib = buffers_.get(state_.index_buffer);
        if (!ib) { ++stats_.draws_skipped; return; }
        const bool index32 = ib->format_or_fvf == fmt_index32;
        const uint32_t isz = index32 ? 4 : 2;
        // which streams the declaration reads, and whether any must be copied
        bool any_copy = false;
        const int64_t base = base_vertex;
        struct StreamPlan { uint32_t stream; BufferObject* buffer; uint32_t buffer_id; uint32_t stride; uint32_t offset; bool copy; };
        StreamPlan plans[4];
        uint32_t plan_count = 0;
        for (uint32_t s = 0; s < 16; ++s) {
            if (!(decl->stream_mask & (1u << s))) continue;
            if (plan_count >= 4) { note_unsupported("more than four vertex streams"); break; }
            const StreamBinding& sb = state_.streams[s];
            BufferObject* b = buffers_.get(sb.buffer);
            if (!b || !sb.stride) { ++stats_.draws_skipped; return; }
            const bool copy = buffer_is_dynamic(*b) || (sb.offset % sb.stride) != 0 || base + int64_t(sb.offset / sb.stride) < 0;
            plans[plan_count++] = StreamPlan{s, b, sb.buffer, sb.stride, sb.offset, copy};
            any_copy |= copy;
        }
        const bool ib_dynamic = buffer_is_dynamic(*ib);
        // Indices are rewritten (made relative to the first vertex read) when a vertex stream is copied and the draw does not start at its vertex 0.
        const int64_t lo = (any_copy && (min_vertex > 0 || base < 0)) ? int64_t(min_vertex) : 0;
        const bool rewrite = lo != 0;
        const int64_t first_vertex = base + lo;            // first vertex read, relative to the start of each stream
        if (first_vertex < 0) { ++stats_.draws_skipped; return; }
        const uint32_t span_vertices = uint32_t(int64_t(min_vertex) + vertex_count - lo);
        for (uint32_t i = 0; i < plan_count; ++i) {
            const StreamPlan& p = plans[i];
            const uint32_t slot = i;   // gpu stream index: the order of the used streams (the layout's stream numbers are remapped below)
            if (p.copy) {
                const uint32_t byte_offset = p.offset + uint32_t(first_vertex) * p.stride;
                d.vertex_buffer[slot] = transient_copy(*p.buffer, byte_offset, span_vertices * p.stride, p.stride, p.buffer_id);
                d.vertex_offset[slot] = 0;
            } else {
                d.vertex_buffer[slot] = static_gpu_buffer(*p.buffer);
                d.vertex_offset[slot] = p.offset + uint32_t(first_vertex) * p.stride;
            }
            d.vertex_stride[slot] = p.stride;
            if (!d.vertex_buffer[slot]) { ++stats_.draws_skipped; return; }
        }
        d.vertex_count = span_vertices;
        remap_streams_.clear();
        for (uint32_t i = 0; i < plan_count; ++i) remap_streams_[plans[i].stream] = i;
        layout_scratch_ = decl->attributes;
        for (auto& a : layout_scratch_) a.stream = uint8_t(remap_streams_[a.stream]);
        d.layout = std::span<const gpu::VertexAttribute>(layout_scratch_);

        const uint64_t index_bytes = uint64_t(index_count) * isz;
        const uint64_t index_start_bytes = uint64_t(start_index) * isz;
        if (index_start_bytes + index_bytes > ib->shadow.size()) { ++stats_.draws_skipped; return; }
        if (!ib_dynamic && !rewrite && !fan) {
            d.index_buffer = static_gpu_buffer(*ib);
            d.index32 = index32;
            d.first_index = start_index;
            d.index_count = index_count;
        } else {
            // copy (and convert) the range this draw reads
            const std::byte* src = ib->shadow.data() + index_start_bytes;
            const uint32_t out_count = fan ? (primitive_count * 3) : index_count;
            const bool wide = index32;
            const uint32_t out_isz = wide ? 4 : 2;
            MutableBytes iout;
            auto ir = gpu_.allocate_transient(out_count * out_isz, out_isz, true, iout);
            if (!ir) { ++stats_.draws_skipped; return; }
            auto fetch = [&](uint32_t i) -> uint32_t {
                uint32_t v;
                if (wide) std::memcpy(&v, src + size_t(i) * 4, 4); else { uint16_t w; std::memcpy(&w, src + size_t(i) * 2, 2); v = w; }
                const int64_t r = int64_t(v) - lo;
                return r < 0 ? 0u : uint32_t(r);
            };
            auto store = [&](uint32_t at, uint32_t v) {
                if (wide) std::memcpy(iout.data() + size_t(at) * 4, &v, 4); else { const uint16_t w = uint16_t(v); std::memcpy(iout.data() + size_t(at) * 2, &w, 2); }
            };
            if (fan) {
                ++stats_.fan_conversions;
                for (uint32_t t = 0; t < primitive_count; ++t) { store(t * 3, fetch(0)); store(t * 3 + 1, fetch(t + 1)); store(t * 3 + 2, fetch(t + 2)); }
                d.topology = gpu::Topology::triangles;
            } else {
                for (uint32_t i = 0; i < out_count; ++i) store(i, fetch(i));
                if (rewrite) ++stats_.index_rewrites;
            }
            ++stats_.transient_copies; stats_.transient_bytes += out_count * out_isz;
            d.index_buffer = ir.value; d.index32 = wide; d.first_index = 0; d.index_count = out_count;
            if (ib_dynamic) ib->used_since_update = true;
        }
    }
    if (up) { layout_scratch_ = decl->attributes; for (auto& a : layout_scratch_) a.stream = 0; d.layout = std::span<const gpu::VertexAttribute>(layout_scratch_); }

    // ---- textures
    for (uint32_t i = 0; i < 16; ++i) {
        if (!(program.pixel->sampler_mask & (1u << i))) continue;
        TextureObject* t = textures_.get(state_.textures[i]);
        const bool cube_expected = false;
        d.textures[i] = t && t->gpu ? t->gpu : dummy_texture(t ? t->cube : cube_expected);
        d.samplers[i] = sampler_desc(i);
        if (t && t->levels <= 1) d.samplers[i].use_mips = false;
    }

    // ---- constants
    float vfix[8], pfix[8];
    fill_vertex_fixup(vfix, pass_attachments_);
    const bool robust_equal = ff_vertex && state_.rs[rs_zfunc] == cmp_equal;
    if (robust_equal) vfix[5] -= 1e-5f;   // see below: a fixed-function redraw cannot reproduce the depth of a shader pass bit for bit
    fill_pixel_fixup(pfix);
    if (ff_vertex) { fill_ff_vertex_constants(ff_constants_); collect_constants(vs_constants_scratch_, ff_constants_.data(), uint32_t(ff_constants_.size()), *program.vertex, vfix); }
    else collect_constants(vs_constants_scratch_, state_.vs_constants.data(), uint32_t(state_.vs_constants.size()), *program.vertex, vfix);
    if (ff_pixel) { fill_ff_pixel_constants(ff_constants_); collect_constants(ps_constants_scratch_, ff_constants_.data(), uint32_t(ff_constants_.size()), *program.pixel, pfix); }
    else collect_constants(ps_constants_scratch_, state_.ps_constants.data(), uint32_t(state_.ps_constants.size()), *program.pixel, pfix);
    d.vertex_constants = Bytes(vs_constants_scratch_.data(), vs_constants_scratch_.size());
    d.pixel_constants = Bytes(ps_constants_scratch_.data(), ps_constants_scratch_.size());

    fill_states(d);
    // D3DCMP_EQUAL redraws through the fixed-function path (the game redraws geometry with its own transform): the transform of the first pass cannot be
    // reproduced bit for bit, so the test becomes "less or equal" and the redraw is pulled a hair towards the camera.
    if (robust_equal) d.depth_stencil.depth_func = gpu::Compare::less_equal;
    gpu_.draw(d);
}

}  // namespace nue::d3d9
