#include "layer/fixed_function.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace nue::d3d9 {
using namespace shader;

namespace {

uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0xFF51AFD7ED558CCDull; }

struct Emitter {
    Program p;
    uint32_t constants = 0;

    static SrcParam src(RegType type, uint16_t index, const char* swizzle = "xyzw", SrcModifier modifier = SrcModifier::none) {
        SrcParam s;
        s.type = type; s.index = index; s.modifier = modifier;
        for (int i = 0; i < 4; ++i) s.swizzle[i] = uint8_t(swizzle[i] == 'x' ? 0 : swizzle[i] == 'y' ? 1 : swizzle[i] == 'z' ? 2 : 3);
        return s;
    }
    static DstParam dst(RegType type, uint16_t index, uint8_t mask = 0xF, bool saturate = false, int8_t shift = 0) {
        DstParam d;
        d.type = type; d.index = index; d.mask = mask; d.modifiers = saturate ? mod_saturate : 0; d.shift = shift;
        return d;
    }
    void emit(uint16_t opcode, const DstParam& d, std::initializer_list<SrcParam> sources) {
        Instruction in;
        in.opcode = opcode; in.has_dst = true; in.dst = d;
        for (const SrcParam& s : sources) {
            in.src[in.src_count++] = s;
            if (s.type == RegType::const_float) constants = std::max<uint32_t>(constants, s.index + 1u);
        }
        p.code.push_back(in);
    }
    void dcl(uint16_t reg, uint32_t usage, uint32_t index) {
        Instruction in;
        in.opcode = op_dcl; in.has_dst = true; in.dst.type = RegType::input; in.dst.index = reg; in.dcl_usage = usage; in.dcl_index = index;
        p.code.push_back(in);
    }
    void def(uint16_t reg, float x, float y, float z, float w) {
        Instruction in;
        in.opcode = op_def; in.has_dst = true; in.dst.type = RegType::const_float; in.dst.index = reg;
        in.immediate[0] = x; in.immediate[1] = y; in.immediate[2] = z; in.immediate[3] = w;
        p.code.push_back(in);
    }
    Program finish() { p.constant_registers_read = constants; return std::move(p); }
};

}  // namespace

uint64_t FfVertexKey::hash() const {
    uint64_t h = 0x1234;
    h = mix(h, uint64_t(in.position) | uint64_t(in.rhw) << 1 | uint64_t(in.normal) << 2 | uint64_t(in.diffuse) << 3 | uint64_t(in.specular) << 4 | uint64_t(in.texcoord_present_mask) << 8 | uint64_t(stages) << 16 | uint64_t(fog_mode) << 24);
    for (int i = 0; i < 8; ++i) h = mix(h, uint64_t(coord_index[i]) | uint64_t(transform_count[i]) << 8 | uint64_t(coord_gen[i]) << 16);
    h = mix(h, uint64_t(lighting) | uint64_t(specular_enable) << 1 | uint64_t(local_viewer) << 2 | uint64_t(light_count) << 8);
    for (int i = 0; i < 8; ++i) h = mix(h, light_type[i]);
    for (int i = 0; i < 4; ++i) h = mix(h, material_source[i]);
    return h;
}

uint64_t FfPixelKey::hash() const {
    uint64_t h = 0x5678;
    h = mix(h, uint64_t(stage_count) | uint64_t(specular) << 8);
    for (int i = 0; i < stage_count; ++i) {
        const FfPixelStage& s = stage[i];
        h = mix(h, uint64_t(s.color_op) | uint64_t(s.color_arg0) << 8 | uint64_t(s.color_arg1) << 16 | uint64_t(s.color_arg2) << 24 | uint64_t(s.alpha_op) << 32 | uint64_t(s.alpha_arg0) << 40 |
                       uint64_t(s.alpha_arg1) << 48 | uint64_t(s.alpha_arg2) << 56);
        h = mix(h, uint64_t(s.result_arg) | uint64_t(s.texture) << 8);
    }
    return h;
}

namespace {
// Vertex lighting. temps: 5 camera-space position, 6 unit normal (both prepared by the caller), 11 ambient, 12 diffuse, 13 specular sums, 14 light vector,
// 15 / 16 scratch, 17 attenuation, 18 vector to the eye.
void emit_lighting(Emitter& e, const FfVertexKey& key, uint16_t v_diffuse, uint16_t v_specular, uint16_t t_pos, uint16_t t_nrm) {
    using R = RegType;
    constexpr uint16_t t_amb = 11, t_dif = 12, t_spe = 13, t_l = 14, t_s0 = 15, t_s1 = 16, t_att = 17, t_eye = 18;
    constexpr uint16_t c_ambient = 48, c_mdiff = 49, c_mamb = 50, c_mspec = 51, c_memis = 52, c_power = 53, c_light = 56;
    auto K = [](uint16_t reg, const char* sw = "xyzw", SrcModifier m = SrcModifier::none) { return Emitter::src(R::const_float, reg, sw, m); };
    auto T = [](uint16_t reg, const char* sw = "xyzw", SrcModifier m = SrcModifier::none) { return Emitter::src(R::temp, reg, sw, m); };
    e.emit(op_mov, e.dst(R::temp, t_amb, 7), {K(c_ambient)});
    e.emit(op_mov, e.dst(R::temp, t_dif, 7), {K(14)});
    e.emit(op_mov, e.dst(R::temp, t_spe, 7), {K(14)});
    if (key.specular_enable) {
        if (key.local_viewer) {
            e.emit(op_dp3, e.dst(R::temp, t_eye, 8), {T(t_pos), T(t_pos)});
            e.emit(op_rsq, e.dst(R::temp, t_eye, 8), {T(t_eye, "wwww")});
            e.emit(op_mul, e.dst(R::temp, t_eye, 7), {T(t_pos, "xyzw", SrcModifier::negate), T(t_eye, "wwww")});
        } else e.emit(op_mov, e.dst(R::temp, t_eye, 7), {K(15, "xyzw", SrcModifier::negate)});
    }
    for (uint32_t i = 0; i < key.light_count; ++i) {
        const uint16_t b = uint16_t(c_light + 7 * i);
        const uint32_t type = key.light_type[i];
        if (type == 3) {   // directional: the light vector is against the direction, no attenuation
            e.emit(op_mov, e.dst(R::temp, t_l, 7), {K(uint16_t(b + 4), "xyzw", SrcModifier::negate)});
            e.emit(op_mov, e.dst(R::temp, t_att, 1), {K(15, "zzzz")});
        } else {
            e.emit(op_sub, e.dst(R::temp, t_l, 7), {K(uint16_t(b + 3)), T(t_pos)});
            e.emit(op_dp3, e.dst(R::temp, t_s0, 1), {T(t_l), T(t_l)});
            e.emit(op_rsq, e.dst(R::temp, t_s0, 2), {T(t_s0, "xxxx")});
            e.emit(op_mul, e.dst(R::temp, t_l, 7), {T(t_l), T(t_s0, "yyyy")});
            e.emit(op_mul, e.dst(R::temp, t_s0, 4), {T(t_s0, "xxxx"), T(t_s0, "yyyy")});   // d = |L| = |L|^2 * (1 / |L|)
            e.emit(op_mad, e.dst(R::temp, t_s1, 1), {K(uint16_t(b + 5), "zzzz"), T(t_s0, "zzzz"), K(uint16_t(b + 5), "yyyy")});
            e.emit(op_mad, e.dst(R::temp, t_s1, 1), {T(t_s1, "xxxx"), T(t_s0, "zzzz"), K(uint16_t(b + 5), "xxxx")});
            e.emit(op_rcp, e.dst(R::temp, t_att, 1), {T(t_s1, "xxxx")});
            e.emit(op_slt, e.dst(R::temp, t_s1, 1), {K(uint16_t(b + 3), "wwww"), T(t_s0, "zzzz")});   // beyond the range
            e.emit(op_mul, e.dst(R::temp, t_att, 1), {T(t_att, "xxxx"), T(t_s1, "xxxx", SrcModifier::complement)});
            if (type == 2) {   // spot cone
                e.emit(op_dp3, e.dst(R::temp, t_s1, 1), {T(t_l, "xyzw", SrcModifier::negate), K(uint16_t(b + 4))});
                e.emit(op_sub, e.dst(R::temp, t_s1, 2), {T(t_s1, "xxxx"), K(uint16_t(b + 5), "wwww")});
                e.emit(op_mul, e.dst(R::temp, t_s1, 2), {T(t_s1, "yyyy"), K(uint16_t(b + 6), "yyyy")});
                e.emit(op_max, e.dst(R::temp, t_s1, 2), {T(t_s1, "yyyy"), K(14, "xxxx")});   // not capped at 1 inside the inner cone: the real runtime does not either
                e.emit(op_pow, e.dst(R::temp, t_s1, 2), {T(t_s1, "yyyy"), K(uint16_t(b + 4), "wwww")});
                e.emit(op_mul, e.dst(R::temp, t_att, 1), {T(t_att, "xxxx"), T(t_s1, "yyyy")});
            }
        }
        e.emit(op_dp3, e.dst(R::temp, t_s0, 1), {T(t_nrm), T(t_l)});
        e.emit(op_max, e.dst(R::temp, t_s0, 1), {T(t_s0, "xxxx"), K(14, "xxxx")});
        e.emit(op_mul, e.dst(R::temp, t_s1, 1), {T(t_att, "xxxx"), T(t_s0, "xxxx")});
        e.emit(op_mad, e.dst(R::temp, t_dif, 7), {K(b), T(t_s1, "xxxx"), T(t_dif)});
        e.emit(op_mad, e.dst(R::temp, t_amb, 7), {K(uint16_t(b + 2)), T(t_att, "xxxx"), T(t_amb)});
        if (key.specular_enable) {
            e.emit(op_add, e.dst(R::temp, t_s1, 7), {T(t_l), T(t_eye)});
            e.emit(op_dp3, e.dst(R::temp, t_s1, 8), {T(t_s1), T(t_s1)});
            e.emit(op_rsq, e.dst(R::temp, t_s1, 8), {T(t_s1, "wwww")});
            e.emit(op_mul, e.dst(R::temp, t_s1, 7), {T(t_s1), T(t_s1, "wwww")});
            e.emit(op_dp3, e.dst(R::temp, t_s1, 8), {T(t_nrm), T(t_s1)});
            e.emit(op_max, e.dst(R::temp, t_s1, 8), {T(t_s1, "wwww"), K(14, "xxxx")});
            e.emit(op_pow, e.dst(R::temp, t_s1, 8), {T(t_s1, "wwww"), K(c_power, "xxxx")});
            e.emit(op_slt, e.dst(R::temp, t_s1, 2), {K(14, "xxxx"), T(t_s0, "xxxx")});   // only where the surface faces the light
            e.emit(op_mul, e.dst(R::temp, t_s1, 8), {T(t_s1, "wwww"), T(t_s1, "yyyy")});
            e.emit(op_mul, e.dst(R::temp, t_s1, 8), {T(t_s1, "wwww"), T(t_att, "xxxx")});
            e.emit(op_mad, e.dst(R::temp, t_spe, 7), {K(uint16_t(b + 1)), T(t_s1, "wwww"), T(t_spe)});
        }
    }
    auto alpha_of = [](SrcParam p) { for (int i = 0; i < 4; ++i) p.swizzle[i] = 3; return p; };
    auto source = [&](int which, uint16_t material_register) -> SrcParam {
        const uint32_t s = key.material_source[which];
        if (s == 1 && key.in.diffuse) return Emitter::src(R::input, v_diffuse);
        if (s == 2 && key.in.specular) return Emitter::src(R::input, v_specular);
        return K(material_register);   // no such colour in the vertex: the material colour is used (as the real runtime does)
    };
    e.emit(op_mul, e.dst(R::temp, t_s0, 7), {source(2, c_mamb), T(t_amb)});
    e.emit(op_mad, e.dst(R::temp, t_s0, 7), {source(0, c_mdiff), T(t_dif), T(t_s0)});
    e.emit(op_add, e.dst(R::attr_out, 0, 7, true), {T(t_s0), source(3, c_memis)});
    e.emit(op_mov, e.dst(R::attr_out, 0, 8, true), {alpha_of(source(0, c_mdiff))});
    if (key.specular_enable) {
        e.emit(op_mul, e.dst(R::attr_out, 1, 7, true), {source(1, c_mspec), T(t_spe)});
        e.emit(op_mov, e.dst(R::attr_out, 1, 8, true), {alpha_of(source(1, c_mspec))});
    }
}
}  // namespace

Program build_ff_vertex(const FfVertexKey& key) {
    Emitter e;
    e.p.stage = Stage::vertex; e.p.major = 1; e.p.minor = 1;
    using R = RegType;
    constexpr uint16_t v_pos = 0, v_normal = 1, v_diffuse = 2, v_specular = 3, v_tex = 4;
    e.dcl(v_pos, 0, 0);   // POSITION (POSITIONT maps to the same attribute)
    if (key.in.normal) e.dcl(v_normal, 3, 0);
    if (key.in.diffuse) e.dcl(v_diffuse, 10, 0);
    if (key.in.specular) e.dcl(v_specular, 10, 1);
    for (uint32_t k = 0; k < 8; ++k) if (key.in.texcoord_present_mask & (1u << k)) e.dcl(uint16_t(v_tex + k), 5, k);
    e.def(15, 0, 0, 1, 0);
    e.def(14, 0, 0, 0, 1);
    e.def(13, -1.4426950f, 0, 0, 0);
    e.def(12, 0.5f, 0.5f, 0, 1);

    // position
    if (key.in.rhw) {
        // pre-transformed vertices: pixel coordinates -> clip space of the viewport, w = 1 / rhw
        e.emit(op_rcp, e.dst(R::temp, 0, 8), {e.src(R::input, v_pos, "wwww")});
        e.emit(op_mul, e.dst(R::temp, 1, 3), {e.src(R::input, v_pos), e.src(R::const_float, 7)});
        e.emit(op_add, e.dst(R::temp, 1, 3), {e.src(R::temp, 1), e.src(R::const_float, 7, "zwzw")});
        e.emit(op_mul, e.dst(R::rast_out, 0, 3), {e.src(R::temp, 1), e.src(R::temp, 0, "wwww")});
        e.emit(op_mul, e.dst(R::rast_out, 0, 4), {e.src(R::input, v_pos, "zzzz"), e.src(R::temp, 0, "wwww")});
        e.emit(op_mov, e.dst(R::rast_out, 0, 8), {e.src(R::temp, 0, "wwww")});
    } else {
        for (uint16_t row = 0; row < 4; ++row) e.emit(op_dp4, e.dst(R::rast_out, 0, uint8_t(1u << row)), {e.src(R::input, v_pos), e.src(R::const_float, row)});
    }
    // colours
    if (!(key.lighting && !key.in.rhw)) {
        if (key.in.diffuse) e.emit(op_mov, e.dst(R::attr_out, 0), {e.src(R::input, v_diffuse)});
        else e.emit(op_mov, e.dst(R::attr_out, 0), {e.src(R::const_float, 15, "yyyy", SrcModifier::complement)});   // white
        if (key.in.specular) e.emit(op_mov, e.dst(R::attr_out, 1), {e.src(R::input, v_specular)});
    }
    // texture coordinates, one set per stage the pixel stage reads
    bool need_position = false, need_normal = false;
    const bool lit = key.lighting && !key.in.rhw;
    if (lit) {
        need_normal = true;
        for (uint32_t i = 0; i < key.light_count; ++i) if (key.light_type[i] != 3) need_position = true;
        if (key.specular_enable && key.local_viewer) need_position = true;
    }
    for (uint32_t s = 0; s < key.stages && !key.in.rhw; ++s) {
        const uint32_t g = key.coord_gen[s];
        if (g == 2 || g == 3 || g == 4) need_position = true;
        if (g == 1 || g == 3 || g == 4) need_normal = true;
    }
    constexpr uint16_t t_pos = 5, t_nrm = 6, t_inc = 7, t_dot = 8, t_aux = 9, t_gen = 10;
    if (need_position) {   // camera-space position
        for (uint16_t row = 0; row < 3; ++row) e.emit(op_dp4, e.dst(R::temp, t_pos, uint8_t(1u << row)), {e.src(R::input, v_pos), e.src(R::const_float, uint16_t(4 + row))});
        e.emit(op_mov, e.dst(R::temp, t_pos, 8), {e.src(R::const_float, 14, "wwww")});
    }
    if (need_normal) {     // camera-space unit normal
        if (key.in.normal) {
            for (uint16_t row = 0; row < 3; ++row) e.emit(op_dp3, e.dst(R::temp, t_nrm, uint8_t(1u << row)), {e.src(R::input, v_normal), e.src(R::const_float, uint16_t(9 + row))});
            e.emit(op_dp3, e.dst(R::temp, t_nrm, 8), {e.src(R::temp, t_nrm), e.src(R::temp, t_nrm)});
            e.emit(op_rsq, e.dst(R::temp, t_nrm, 8), {e.src(R::temp, t_nrm, "wwww")});
            e.emit(op_mul, e.dst(R::temp, t_nrm, 7), {e.src(R::temp, t_nrm), e.src(R::temp, t_nrm, "wwww")});
        } else e.emit(op_mov, e.dst(R::temp, t_nrm), {e.src(R::const_float, 14)});
    }
    if (lit) emit_lighting(e, key, v_diffuse, v_specular, t_pos, t_nrm);
    for (uint32_t s = 0; s < key.stages; ++s) {
        const uint32_t gen = key.in.rhw ? 0 : key.coord_gen[s];
        const uint32_t k = key.coord_index[s];
        SrcParam tc = e.src(R::input, uint16_t(v_tex + (k < 8 ? k : 0)));
        if (gen) {
            switch (gen) {
            case 1: e.emit(op_mov, e.dst(R::temp, t_gen, 7), {e.src(R::temp, t_nrm)}); break;
            case 2: e.emit(op_mov, e.dst(R::temp, t_gen, 7), {e.src(R::temp, t_pos)}); break;
            default: {   // reflection vector R = I - 2 (N.I) N, with I the unit vector from the eye to the vertex
                e.emit(op_dp3, e.dst(R::temp, t_inc, 8), {e.src(R::temp, t_pos), e.src(R::temp, t_pos)});
                e.emit(op_rsq, e.dst(R::temp, t_inc, 8), {e.src(R::temp, t_inc, "wwww")});
                e.emit(op_mul, e.dst(R::temp, t_inc, 7), {e.src(R::temp, t_pos), e.src(R::temp, t_inc, "wwww")});
                e.emit(op_dp3, e.dst(R::temp, t_dot, 1), {e.src(R::temp, t_nrm), e.src(R::temp, t_inc)});
                e.emit(op_add, e.dst(R::temp, t_dot, 1), {e.src(R::temp, t_dot, "xxxx"), e.src(R::temp, t_dot, "xxxx")});
                e.emit(op_mad, e.dst(R::temp, t_gen, 7), {e.src(R::temp, t_nrm), e.src(R::temp, t_dot, "xxxx", SrcModifier::negate), e.src(R::temp, t_inc)});
                if (gen == 4) {   // sphere map (matches the real runtime): u = Rx / m + 1/2, v = Ry / m + 1/2 with m = 2 sqrt(Rx^2 + Ry^2 + (Rz - 1)^2)
                    e.emit(op_add, e.dst(R::temp, t_aux, 7), {e.src(R::temp, t_gen), e.src(R::const_float, 15, "xyzw", SrcModifier::negate)});   // R - (0, 0, 1): Direct3D looks down +Z
                    e.emit(op_dp3, e.dst(R::temp, t_dot, 1), {e.src(R::temp, t_aux), e.src(R::temp, t_aux)});
                    e.emit(op_rsq, e.dst(R::temp, t_dot, 1), {e.src(R::temp, t_dot, "xxxx")});
                    e.emit(op_mul, e.dst(R::temp, t_aux, 3), {e.src(R::temp, t_gen), e.src(R::temp, t_dot, "xxxx")});
                    e.emit(op_mad, e.dst(R::temp, t_gen, 3), {e.src(R::temp, t_aux), e.src(R::const_float, 12, "xxxx"), e.src(R::const_float, 12, "xxxx")});
                    e.emit(op_mov, e.dst(R::temp, t_gen, 4), {e.src(R::const_float, 14, "xxxx")});
                }
            }
            }
            e.emit(op_mov, e.dst(R::temp, t_gen, 8), {e.src(R::const_float, 14, "wwww")});
            tc = e.src(R::temp, t_gen);
        } else if (!(k < 8 && (key.in.texcoord_present_mask & (1u << k)))) { e.emit(op_mov, e.dst(R::texcoord_out, uint16_t(s)), {e.src(R::const_float, 14)}); continue; }
        const uint32_t count = key.in.rhw ? 0 : key.transform_count[s];
        if (!count) { e.emit(op_mov, e.dst(R::texcoord_out, uint16_t(s)), {tc}); continue; }
        // the missing components of the coordinate are filled the way Direct3D 9 does (2D: z = 1, w = 0; 3D: w = 1)
        e.emit(op_mov, e.dst(R::temp, 2), {e.src(R::const_float, count == 2 ? 15 : 14)});
        e.emit(op_mov, e.dst(R::temp, 2, count == 2 ? 3 : count == 3 ? 7 : 15), {tc});
        for (uint16_t row = 0; row < 4; ++row)
            e.emit(op_dp4, e.dst(R::texcoord_out, uint16_t(s), uint8_t(1u << row)), {e.src(R::temp, 2), e.src(R::const_float, uint16_t(16 + 4 * s + row))});
    }
    // vertex fog from the eye-space depth
    if (key.fog_mode) {
        e.emit(op_dp4, e.dst(R::temp, 3, 1), {e.src(R::input, v_pos), e.src(R::const_float, 6)});
        e.emit(op_mov, e.dst(R::temp, 3, 1), {e.src(R::temp, 3, "xxxx", SrcModifier::abs)});
        if (key.fog_mode == 1) {
            e.emit(op_mad, e.dst(R::rast_out, 1, 1), {e.src(R::temp, 3, "xxxx"), e.src(R::const_float, 8, "xxxx"), e.src(R::const_float, 8, "yyyy")});
        } else {
            if (key.fog_mode == 3) {
                e.emit(op_mul, e.dst(R::temp, 3, 1), {e.src(R::temp, 3, "xxxx"), e.src(R::const_float, 8, "wwww")});
                e.emit(op_mul, e.dst(R::temp, 3, 1), {e.src(R::temp, 3, "xxxx"), e.src(R::temp, 3, "xxxx")});
                e.emit(op_mul, e.dst(R::temp, 3, 1), {e.src(R::temp, 3, "xxxx"), e.src(R::const_float, 13, "xxxx")});
            } else {
                e.emit(op_mul, e.dst(R::temp, 3, 1), {e.src(R::temp, 3, "xxxx"), e.src(R::const_float, 8, "zzzz")});
            }
            e.emit(op_expp, e.dst(R::temp, 4), {e.src(R::temp, 3, "xxxx")});
            e.emit(op_mov, e.dst(R::rast_out, 1, 1), {e.src(R::temp, 4, "zzzz")});
        }
    }
    return e.finish();
}

namespace {
// D3DTA_* argument -> source operand
SrcParam stage_argument(Emitter& e, uint32_t arg, uint32_t stage, bool alpha) {
    using R = RegType;
    const uint32_t base = arg & 7;
    SrcParam s;
    switch (base) {
    case 0: s = Emitter::src(R::input, 0); break;                              // DIFFUSE
    case 1: s = Emitter::src(R::temp, 0); break;                               // CURRENT
    case 2: s = Emitter::src(R::temp, 2); break;                               // TEXTURE
    case 3: s = Emitter::src(R::const_float, 0); break;                        // TFACTOR
    case 4: s = Emitter::src(R::input, 1); break;                              // SPECULAR
    case 5: s = Emitter::src(R::temp, 1); break;                               // TEMP
    default: s = Emitter::src(R::const_float, uint16_t(1 + stage)); break;     // CONSTANT
    }
    if ((arg & 0x20) || alpha) for (int i = 0; i < 4; ++i) s.swizzle[i] = 3;   // alpha replicate / alpha operations read .w
    if (arg & 0x10) s.modifier = SrcModifier::complement;
    if (e.p.stage == Stage::pixel && s.type == R::const_float) e.constants = std::max<uint32_t>(e.constants, s.index + 1u);
    return s;
}

void emit_operation(Emitter& e, uint32_t op, const DstParam& d, SrcParam a0, SrcParam a1, SrcParam a2, bool alpha, uint32_t stage, const FfPixelStage& st) {
    (void)stage; (void)st;
    using R = RegType;
    DstParam out = d;
    out.modifiers = mod_saturate;
    auto with_mod = [](SrcParam s, SrcModifier m) { s.modifier = m; return s; };
    auto alpha_of = [](SrcParam s) { for (int i = 0; i < 4; ++i) s.swizzle[i] = 3; return s; };
    switch (op) {
    case 2: e.emit(op_mov, out, {a1}); break;
    case 3: e.emit(op_mov, out, {a2}); break;
    case 4: e.emit(op_mul, out, {a1, a2}); break;
    case 5: out.shift = 1; e.emit(op_mul, out, {a1, a2}); break;
    case 6: out.shift = 2; e.emit(op_mul, out, {a1, a2}); break;
    case 7: e.emit(op_add, out, {a1, a2}); break;
    case 8: e.emit(op_add, out, {a1, with_mod(a2, SrcModifier::bias)}); break;
    case 9: out.shift = 1; e.emit(op_add, out, {a1, with_mod(a2, SrcModifier::bias)}); break;
    case 10: e.emit(op_sub, out, {a1, a2}); break;
    case 11: e.emit(op_mad, out, {a2, with_mod(a1, SrcModifier::complement), a1}); break;   // a1 + a2 * (1 - a1)
    case 12: e.emit(op_lrp, out, {Emitter::src(R::input, 0, "wwww"), a1, a2}); break;       // blend by diffuse alpha
    case 13: e.emit(op_lrp, out, {Emitter::src(R::temp, 2, "wwww"), a1, a2}); break;        // blend by texture alpha
    case 14: e.emit(op_lrp, out, {Emitter::src(R::const_float, 0, "wwww"), a1, a2}); break; // blend by factor alpha
    case 15: e.emit(op_mad, out, {a2, Emitter::src(R::temp, 2, "wwww", SrcModifier::complement), a1}); break;   // premultiplied texture alpha
    case 16: e.emit(op_lrp, out, {Emitter::src(R::temp, 0, "wwww"), a1, a2}); break;        // blend by current alpha
    case 18: e.emit(op_mad, out, {alpha_of(a1), a2, a1}); break;
    case 19: e.emit(op_mad, out, {a1, a2, alpha_of(a1)}); break;
    case 20: { SrcParam inv = alpha_of(a1); inv.modifier = SrcModifier::complement; e.emit(op_mad, out, {inv, a2, a1}); break; }
    case 21: e.emit(op_mad, out, {with_mod(a1, SrcModifier::complement), a2, alpha_of(a1)}); break;
    case 24: { DstParam rgba = out; rgba.mask = 0xF; rgba.shift = 2; e.emit(op_dp3, rgba, {with_mod(a1, SrcModifier::bias), with_mod(a2, SrcModifier::bias)}); break; }
    case 25: e.emit(op_mad, out, {a1, a2, a0}); break;
    case 26: e.emit(op_lrp, out, {a0, a1, a2}); break;
    default: e.emit(op_mov, out, {a1}); break;   // unsupported operations behave as SELECTARG1
    }
    (void)alpha;
}
}  // namespace

Program build_ff_pixel(const FfPixelKey& key) {
    Emitter e;
    e.p.stage = Stage::pixel; e.p.major = 2; e.p.minor = 0;
    using R = RegType;
    e.constants = 1;
    // current = diffuse colour; temp = 0
    e.emit(op_mov, e.dst(R::temp, 0), {e.src(R::input, 0)});
    e.emit(op_sub, e.dst(R::temp, 1), {e.src(R::temp, 0), e.src(R::temp, 0)});
    for (uint32_t s = 0; s < key.stage_count; ++s) {
        const FfPixelStage& st = key.stage[s];
        if (st.texture) {
            Instruction in;
            in.opcode = op_tex; in.has_dst = true; in.dst.type = R::temp; in.dst.index = 2; in.src_count = 2;
            in.src[0] = e.src(R::addr_texture, uint16_t(s)); in.src[1] = e.src(R::sampler, uint16_t(s));
            e.p.code.push_back(in);
        }
        const uint16_t result = st.result_arg == 5 ? 1 : 0;
        const DstParam color = e.dst(R::temp, result, 7), alpha = e.dst(R::temp, result, 8);
        // colour channels
        emit_operation(e, st.color_op, color, stage_argument(e, st.color_arg0, s, false), stage_argument(e, st.color_arg1, s, false), stage_argument(e, st.color_arg2, s, false), false, s, st);
        // alpha channel (a disabled alpha operation leaves the alpha of the result register alone)
        if (st.alpha_op != 1) emit_operation(e, st.alpha_op, alpha, stage_argument(e, st.alpha_arg0, s, true), stage_argument(e, st.alpha_arg1, s, true), stage_argument(e, st.alpha_arg2, s, true), true, s, st);
    }
    if (key.specular) e.emit(op_add, e.dst(R::temp, 0, 7, true), {e.src(R::temp, 0), e.src(R::input, 1)});
    e.emit(op_mov, e.dst(R::color_out, 0), {e.src(R::temp, 0)});
    return e.finish();
}

}  // namespace nue::d3d9
