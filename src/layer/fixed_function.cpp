#include "layer/fixed_function.hpp"
#include <algorithm>
#include <cstring>

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
    for (int i = 0; i < 8; ++i) h = mix(h, uint64_t(coord_index[i]) | uint64_t(transform_count[i]) << 8);
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
    if (key.in.diffuse) e.emit(op_mov, e.dst(R::attr_out, 0), {e.src(R::input, v_diffuse)});
    else e.emit(op_mov, e.dst(R::attr_out, 0), {e.src(R::const_float, 15, "yyyy", SrcModifier::complement)});   // white
    if (key.in.specular) e.emit(op_mov, e.dst(R::attr_out, 1), {e.src(R::input, v_specular)});
    // texture coordinates, one set per stage the pixel stage reads
    for (uint32_t s = 0; s < key.stages; ++s) {
        const uint32_t k = key.coord_index[s];
        if (!(k < 8 && (key.in.texcoord_present_mask & (1u << k)))) { e.emit(op_mov, e.dst(R::texcoord_out, uint16_t(s)), {e.src(R::const_float, 14)}); continue; }
        const SrcParam tc = e.src(R::input, uint16_t(v_tex + k));
        const uint32_t count = key.transform_count[s];
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
