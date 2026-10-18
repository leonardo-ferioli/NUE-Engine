#include "nue/shader/d3d9_to_spirv.hpp"
#include "shader/spirv_builder.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace nue::shader {

// bgfx::Attrib::Enum (vendored bgfx, include/bgfx/defines.h order): Position 0, Normal 1, Tangent 2, Bitangent 3, Color0..3 = 4..7, Indices 8,
// Weight 9, TexCoord0..13 = 10..23. The point-size usage has no attribute of its own and travels as the last texture coordinate.
uint32_t bgfx_attribute_for(uint32_t usage, uint32_t usage_index) {
    switch (usage) {
    case 0: return 0;                                        // position
    case 1: return 9;                                        // blend weight
    case 2: return 8;                                        // blend indices
    case 3: return 1;                                        // normal
    case 5: return 10 + std::min<uint32_t>(usage_index, 13); // texcoord
    case 6: return 2;                                        // tangent
    case 7: return 3;                                        // binormal
    case 10: return 4 + std::min<uint32_t>(usage_index, 3);  // colour
    default: return 23;
    }
}

const char* bgfx_attribute_name(uint32_t attribute) {
    static const char* const names[] = {"a_position", "a_normal", "a_tangent", "a_bitangent", "a_color0", "a_color1", "a_color2", "a_color3", "a_indices", "a_weight",
        "a_texcoord0", "a_texcoord1", "a_texcoord2", "a_texcoord3", "a_texcoord4", "a_texcoord5", "a_texcoord6", "a_texcoord7", "a_texcoord8", "a_texcoord9",
        "a_texcoord10", "a_texcoord11", "a_texcoord12", "a_texcoord13"};
    return attribute < 24 ? names[attribute] : "a_texcoord13";
}

namespace {

using namespace spv;

struct Lowering {
    const Program& p;
    const TranslateOptions& o;
    TranslateResult& out;
    SpirvBuilder b;
    bool ok = true;

    uint32_t f32 = 0, vec4 = 0, vec3 = 0, vec2 = 0, boolean = 0, bvec4 = 0, fn_vec4_ptr = 0;
    uint32_t ubo_var = 0, fog_input = 0;
    uint32_t constants = 0;
    std::vector<uint32_t> interface_ids;

    // register files (Function-storage vec4 variables)
    std::map<uint32_t, uint32_t> temps, texregs, outputs;   // outputs keyed by (type<<8|index)
    std::map<uint32_t, uint32_t> inputs_global;              // v# / interpolants, keyed by (type<<8|index): Input variable
    std::map<uint32_t, std::array<float, 4>> defs;           // def c#
    std::map<uint32_t, VertexInput> vs_inputs;               // by register
    uint32_t attribute_of_register[16] = {};
    uint32_t image_var[16] = {}, sampler_var[16] = {};
    uint32_t image_dim[16] = {};
    uint32_t addr_var = 0;

    Lowering(const Program& program, const TranslateOptions& options, TranslateResult& result) : p(program), o(options), out(result) {}

    void fail(const std::string& why) { if (ok) out.error = why; ok = false; }
    bool ps() const { return p.stage == Stage::pixel; }
    bool sm1() const { return p.major < 2; }

    uint32_t vec(uint32_t n) { return b.type_vec(f32, n); }
    uint32_t splat(float v) { return b.const_vec4(v, v, v, v); }

    uint32_t temp(uint16_t index) {
        auto it = temps.find(index);
        if (it != temps.end()) return it->second;
        return temps[index] = b.local_variable(vec4, "r" + std::to_string(index));
    }
    uint32_t texreg(uint16_t index) {
        auto it = texregs.find(index);
        if (it != texregs.end()) return it->second;
        return texregs[index] = b.local_variable(vec4, "t" + std::to_string(index));
    }
    uint32_t output(RegType type, uint16_t index) {
        const uint32_t key = (uint32_t(type) << 8) | index;
        auto it = outputs.find(key);
        if (it != outputs.end()) return it->second;
        return outputs[key] = b.local_variable(vec4, "o" + std::to_string(unsigned(type)) + "_" + std::to_string(index));
    }
    uint32_t interpolant(RegType type, uint16_t index) {   // pixel shader inputs: v0/v1 colours (locations 0, 1), t# texcoords (locations 2 + n)
        const uint32_t key = (uint32_t(type) << 8) | index;
        auto it = inputs_global.find(key);
        if (it != inputs_global.end()) return it->second;
        const uint32_t location = type == RegType::input ? index : 2 + index;
        const uint32_t var = b.global_variable(vec4, Input, "vary" + std::to_string(location));   // stage-to-stage matching is by name in OpenGL ES 3.0
        b.decorate(var, Location, {location});
        interface_ids.push_back(var);
        out.interpolants_read |= 1u << (type == RegType::input ? index : 2 + index);
        return inputs_global[key] = var;
    }
    uint32_t vertex_input(uint16_t reg) {
        const uint32_t key = (uint32_t(RegType::input) << 8) | reg;
        auto it = inputs_global.find(key);
        if (it != inputs_global.end()) return it->second;
        VertexInput in{reg, 0, 0, 0};
        auto d = vs_inputs.find(reg);
        if (d != vs_inputs.end()) in = d->second;
        else {   // no dcl (model 1 assembled without one): the fixed register order of Direct3D 9 vertex declarations
            static const uint32_t usage_of[16] = {0, 1, 2, 3, 4, 10, 10, 5, 5, 5, 5, 5, 5, 5, 5, 0};
            static const uint32_t index_of[16] = {0, 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 1};
            in.usage = usage_of[reg & 15]; in.usage_index = index_of[reg & 15]; in.attribute = bgfx_attribute_for(in.usage, in.usage_index);
        }
        const uint32_t var = b.global_variable(vec4, Input, bgfx_attribute_name(in.attribute));
        b.decorate(var, Location, {in.attribute});
        interface_ids.push_back(var);
        out.inputs.push_back(in);
        attribute_of_register[reg & 15] = in.attribute;
        return inputs_global[key] = var;
    }

    uint32_t sampler_image(uint32_t stage, uint32_t dim) {
        if (!image_var[stage]) {
            image_dim[stage] = dim;
            const uint32_t image_type = b.type_image(dim);
            image_var[stage] = b.global_variable(image_type, UniformConstant, "s_tex" + std::to_string(stage));
            b.decorate(image_var[stage], DescriptorSet, {0});
            b.decorate(image_var[stage], Binding, {2 + stage});
            sampler_var[stage] = b.global_variable(b.type_sampler(), UniformConstant, "s_samp" + std::to_string(stage));
            b.decorate(sampler_var[stage], DescriptorSet, {0});
            b.decorate(sampler_var[stage], Binding, {18 + stage});
            out.sampler_mask |= 1u << stage;
        }
        return image_var[stage];
    }

    // ---- source operands ----
    uint32_t constant_value(uint16_t index) {
        auto d = defs.find(index);
        if (d != defs.end()) {
            std::array<float, 4> v = d->second;
            if (ps() && sm1() && o.clamp_ps1_constants) for (auto& x : v) x = std::clamp(x, -1.0f, 1.0f);
            return b.const_vec4(v[0], v[1], v[2], v[3]);
        }
        if (index >= constants) { fail("constant register beyond the uniform block"); return b.const_vec4(0, 0, 0, 0); }
        const uint32_t pointer = b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(2), b.const_uint(index)});
        uint32_t value = b.load(vec4, pointer);
        if (ps() && sm1() && o.clamp_ps1_constants) value = b.ext(vec4, FClamp, {value, splat(-1.0f), splat(1.0f)});
        return value;
    }

    uint32_t raw_source(const SrcParam& s) {
        if (s.relative) {
            if (ps() || s.type != RegType::const_float || !addr_var) { fail("unsupported relative addressing"); return b.const_vec4(0, 0, 0, 0); }
            const uint32_t a0x = component(b.load(vec4, addr_var), 0);
            const uint32_t index = b.emit(IAdd, b.type_int(), {b.emit(ConvertFToS, b.type_int(), {a0x}), b.const_int(int32_t(s.index))});
            const uint32_t clamped = b.ext(b.type_int(), SClamp, {index, b.const_int(0), b.const_int(int32_t(constants) - 1)});
            const uint32_t pointer = b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(2), clamped});
            return b.load(vec4, pointer);
        }
        switch (s.type) {
        case RegType::temp: return b.load(vec4, temp(s.index));
        case RegType::const_float: return constant_value(s.index);
        case RegType::input: {
            if (ps()) return b.load(vec4, interpolant(RegType::input, s.index));
            uint32_t value = b.load(vec4, vertex_input(s.index));
            const uint32_t attribute = attribute_of_register[s.index];
            if (attribute < 32 && ((o.bgra_attributes >> attribute) & 1)) value = b.emit(VectorShuffle, vec4, {value, value, 2u, 1u, 0u, 3u});
            return value;
        }
        case RegType::addr_texture:
            if (!ps()) { fail("a0 as a source is not supported yet"); return b.const_vec4(0, 0, 0, 0); }
            return b.load(vec4, texreg(s.index));
        default: fail("unsupported source register type " + std::to_string(unsigned(s.type))); return b.const_vec4(0, 0, 0, 0);
        }
    }

    uint32_t swizzle(uint32_t value, const uint8_t sw[4]) {
        if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2 && sw[3] == 3) return value;
        return b.emit(VectorShuffle, vec4, {value, value, sw[0], sw[1], sw[2], sw[3]});
    }

    uint32_t source(const SrcParam& s) {
        uint32_t v = swizzle(raw_source(s), s.swizzle);
        const uint32_t half = splat(0.5f), one = splat(1.0f), two = splat(2.0f);
        switch (s.modifier) {
        case SrcModifier::none: break;
        case SrcModifier::negate: v = b.emit(FNegate, vec4, {v}); break;
        case SrcModifier::bias: v = b.emit(FSub, vec4, {v, half}); break;
        case SrcModifier::bias_negate: v = b.emit(FNegate, vec4, {b.emit(FSub, vec4, {v, half})}); break;
        case SrcModifier::sign: v = b.emit(FMul, vec4, {b.emit(FSub, vec4, {v, half}), two}); break;
        case SrcModifier::sign_negate: v = b.emit(FNegate, vec4, {b.emit(FMul, vec4, {b.emit(FSub, vec4, {v, half}), two})}); break;
        case SrcModifier::complement: v = b.emit(FSub, vec4, {one, v}); break;
        case SrcModifier::x2: v = b.emit(FMul, vec4, {v, two}); break;
        case SrcModifier::x2_negate: v = b.emit(FNegate, vec4, {b.emit(FMul, vec4, {v, two})}); break;
        case SrcModifier::abs: v = b.ext(vec4, FAbs, {v}); break;
        case SrcModifier::abs_negate: v = b.emit(FNegate, vec4, {b.ext(vec4, FAbs, {v})}); break;
        case SrcModifier::dz: case SrcModifier::dw: {   // pixel shader 1.4: x/z (or x/w) and y/z (or y/w) for the coordinates
            const uint32_t divisor = b.emit(CompositeExtract, f32, {v, s.modifier == SrcModifier::dz ? 2u : 3u});
            const uint32_t d4 = b.emit(CompositeConstruct, vec4, {divisor, divisor, divisor, divisor});
            v = b.emit(FDiv, vec4, {v, d4});
            break;
        }
        default: fail("unsupported source modifier"); break;
        }
        return v;
    }

    // ---- destination operands ----
    uint32_t dest_variable(const DstParam& d) {
        switch (d.type) {
        case RegType::temp: return temp(d.index);
        case RegType::addr_texture:
            if (ps()) return texreg(d.index);
            if (!addr_var) addr_var = b.local_variable(vec4, "a0");
            return addr_var;
        case RegType::rast_out: case RegType::attr_out: case RegType::texcoord_out: case RegType::color_out: return output(d.type, d.index);
        default: fail("unsupported destination register type " + std::to_string(unsigned(d.type))); return temp(0);
        }
    }

    void write(const DstParam& d, uint32_t value) {
        if (d.shift) value = b.emit(FMul, vec4, {value, splat(d.shift > 0 ? float(1 << d.shift) : 1.0f / float(1 << -d.shift))});
        if (d.modifiers & mod_saturate) value = b.ext(vec4, FClamp, {value, splat(0.0f), splat(1.0f)});
        const uint32_t var = dest_variable(d);
        if (d.mask != 0xF) {
            const uint32_t old = b.load(vec4, var);
            value = b.emit(VectorShuffle, vec4, {old, value, (d.mask & 1) ? 4u : 0u, (d.mask & 2) ? 5u : 1u, (d.mask & 4) ? 6u : 2u, (d.mask & 8) ? 7u : 3u});
        }
        b.store(var, value);
    }

    uint32_t replicate(uint32_t scalar) { return b.emit(CompositeConstruct, vec4, {scalar, scalar, scalar, scalar}); }
    uint32_t component(uint32_t v, uint32_t i) { return b.emit(CompositeExtract, f32, {v, i}); }
    uint32_t head(uint32_t v, uint32_t n) {   // first n components as a vector
        if (n == 4) return v;
        if (n == 3) return b.emit(VectorShuffle, vec3, {v, v, 0u, 1u, 2u});
        return b.emit(VectorShuffle, vec2, {v, v, 0u, 1u});
    }
    uint32_t dot_n(uint32_t a, uint32_t c, uint32_t n) { return b.emit(Dot, f32, {head(a, n), head(c, n)}); }
    uint32_t compare_select(uint32_t cond_type_op, uint32_t a, uint32_t c, uint32_t if_true, uint32_t if_false) {
        const uint32_t cond = b.emit(Op(cond_type_op), bvec4, {a, c});
        return b.emit(Select, vec4, {cond, if_true, if_false});
    }

    uint32_t sample(uint32_t stage, uint32_t coords_value) {
        const uint32_t dim = ps() && image_dim[stage] ? image_dim[stage] : o.sampler_dim[stage];
        const uint32_t image_type = b.type_image(dim);
        sampler_image(stage, dim);
        const uint32_t image = b.load(image_type, image_var[stage]);
        const uint32_t sampler = b.load(b.type_sampler(), sampler_var[stage]);
        const uint32_t combined = b.emit(SampledImage, b.type_sampled_image(image_type), {image, sampler});
        const unsigned projected = o.projected[stage];
        if (dim == 1) {
            if (projected) {
                const uint32_t q = component(coords_value, projected == 4 ? 3u : 2u);
                const uint32_t coords = b.emit(CompositeConstruct, vec3, {component(coords_value, 0), component(coords_value, 1), q});
                return b.emit(ImageSampleProjImplicitLod, vec4, {combined, coords});
            }
            return b.emit(ImageSampleImplicitLod, vec4, {combined, head(coords_value, 2)});
        }
        if (projected) return b.emit(ImageSampleProjImplicitLod, vec4, {combined, coords_value});
        return b.emit(ImageSampleImplicitLod, vec4, {combined, head(coords_value, 3)});
    }

    void instruction(const Instruction& in) {
        switch (in.opcode) {
        case op_nop: case op_phase: case op_dcl: case op_def: return;
        case op_mov: {
            uint32_t value = source(in.src[0]);
            if (!ps() && in.dst.type == RegType::addr_texture) value = b.ext(vec4, Floor, {value});   // vs 1.x: mov to a0 floors
            write(in.dst, value);
            return;
        }
        case op_mova: write(in.dst, b.ext(vec4, Floor, {b.emit(FAdd, vec4, {source(in.src[0]), splat(0.5f)})})); return;
        case op_add: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, b.emit(FAdd, vec4, {a, c})); return; }
        case op_sub: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, b.emit(FSub, vec4, {a, c})); return; }
        case op_mul: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, b.emit(FMul, vec4, {a, c})); return; }
        case op_mad: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]); write(in.dst, b.emit(FAdd, vec4, {b.emit(FMul, vec4, {a, c}), d})); return; }
        case op_dp3: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, replicate(dot_n(a, c, 3))); return; }
        case op_dp4: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, replicate(dot_n(a, c, 4))); return; }
        case op_dp2add: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]);
                          write(in.dst, replicate(b.emit(FAdd, f32, {dot_n(a, c, 2), component(d, 0)}))); return; }
        case op_min: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, b.ext(vec4, FMin, {a, c})); return; }
        case op_max: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, b.ext(vec4, FMax, {a, c})); return; }
        case op_rcp: { auto a = component(source(in.src[0]), 0); write(in.dst, replicate(b.emit(FDiv, f32, {b.const_float(1.0f), a}))); return; }
        case op_rsq: { auto a = b.ext(f32, FAbs, {component(source(in.src[0]), 0)}); write(in.dst, replicate(b.ext(f32, InverseSqrt, {a}))); return; }
        case op_exp: case op_expp: { auto a = component(source(in.src[0]), 0); write(in.dst, replicate(b.ext(f32, Exp2, {a}))); return; }
        case op_log: case op_logp: { auto a = b.ext(f32, FAbs, {component(source(in.src[0]), 0)}); write(in.dst, replicate(b.ext(f32, Log2, {a}))); return; }
        case op_frc: write(in.dst, b.ext(vec4, Fract, {source(in.src[0])})); return;
        case op_slt: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, compare_select(FOrdLessThan, a, c, splat(1.0f), splat(0.0f))); return; }
        case op_sge: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, compare_select(FOrdGreaterThanEqual, a, c, splat(1.0f), splat(0.0f))); return; }
        case op_lrp: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]); write(in.dst, b.ext(vec4, FMix, {d, c, a})); return; }
        case op_cmp: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]); write(in.dst, compare_select(FOrdGreaterThanEqual, a, splat(0.0f), c, d)); return; }
        case op_cnd: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]); write(in.dst, compare_select(FOrdGreaterThan, a, splat(0.5f), c, d)); return; }
        case op_m4x4: case op_m4x3: case op_m3x4: case op_m3x3: case op_m3x2: {
            const unsigned rows = in.opcode == op_m4x4 ? 4 : in.opcode == op_m4x3 ? 3 : in.opcode == op_m3x4 ? 4 : in.opcode == op_m3x3 ? 3 : 2;
            const unsigned width = (in.opcode == op_m4x4 || in.opcode == op_m4x3) ? 4 : 3;
            const uint32_t a = source(in.src[0]);
            uint32_t row[4] = {};
            for (unsigned r = 0; r < 4; ++r) {
                if (r >= rows) { row[r] = b.const_float(0.0f); continue; }
                SrcParam matrix_row = in.src[1];
                matrix_row.index = uint16_t(matrix_row.index + r);
                row[r] = dot_n(a, source(matrix_row), width);
            }
            write(in.dst, b.emit(CompositeConstruct, vec4, {row[0], row[1], row[2], row[3]}));
            return;
        }
        // ---- pixel shader texture instructions
        case op_texcoord:
            if (p.minor == 4 && in.src_count == 1) {   // texcrd rN, tM
                const uint32_t coords = source(in.src[0]);
                write(in.dst, b.emit(VectorShuffle, vec4, {coords, b.const_vec4(0, 0, 0, 1), 0u, 1u, 2u, 7u}));
            } else {                                   // texcoord tN: the coordinates become a colour, clamped, alpha 1
                const uint32_t coords = b.load(vec4, texreg(in.dst.index));
                const uint32_t clamped = b.ext(vec4, FClamp, {coords, splat(0.0f), splat(1.0f)});
                write(in.dst, b.emit(VectorShuffle, vec4, {clamped, b.const_vec4(0, 0, 0, 1), 0u, 1u, 2u, 7u}));
            }
            return;
        case op_tex:
            if (p.major >= 2) {                        // texld dst, coords, sampler
                const uint32_t coords = source(in.src[0]);
                write(in.dst, sample(in.src[1].index, coords));
            } else if (p.minor == 4 && in.src_count == 1) {   // texld rN, tM: sample stage N at the coordinates of tM
                write(in.dst, sample(in.dst.index, source(in.src[0])));
            } else {                                   // tex tN
                write(in.dst, sample(in.dst.index, b.load(vec4, texreg(in.dst.index))));
            }
            return;
        case op_texreg2ar: case op_texreg2gb: {        // dependent read: coordinates from two components of an earlier texture register
            const uint32_t from = b.load(vec4, texreg(in.src_count ? in.src[0].index : 0));
            const uint32_t coords = in.opcode == op_texreg2ar
                ? b.emit(VectorShuffle, vec4, {from, from, 3u, 0u, 3u, 0u})
                : b.emit(VectorShuffle, vec4, {from, from, 1u, 2u, 1u, 2u});
            write(in.dst, sample(in.dst.index, coords));
            return;
        }
        default:
            fail(std::string("unsupported instruction ") + opcode_name(in.opcode));
            return;
        }
    }

    bool run() {
        out = TranslateResult{};
        out.stage = p.stage;
        f32 = b.type_float(); vec2 = vec(2); vec3 = vec(3); vec4 = b.type_vec4();
        boolean = b.type_bool(); bvec4 = b.type_vec(boolean, 4);
        constants = p.uses_relative_constants ? 256 : std::max<uint32_t>(p.constant_registers_read, 1);
        out.constant_registers = constants;

        // pre-scan: defs and dcls
        std::set<uint32_t> declared_samplers;
        for (const Instruction& in : p.code) {
            if (in.opcode == op_def && in.dst.type == RegType::const_float) defs[in.dst.index] = {in.immediate[0], in.immediate[1], in.immediate[2], in.immediate[3]};
            if (in.opcode == op_dcl) {
                if (in.dst.type == RegType::input && !ps()) {
                    VertexInput vi{in.dst.index, in.dcl_usage, in.dcl_index, bgfx_attribute_for(in.dcl_usage, in.dcl_index)};
                    vs_inputs[in.dst.index] = vi;
                } else if (in.dst.type == RegType::sampler) {
                    image_dim[in.dst.index] = in.dcl_texture_type == 3 ? 3 : in.dcl_texture_type == 4 ? 2 : 1;
                }
            }
        }
        // constants: a uniform block { vec4 u_fix0; vec4 u_fix1; vec4 c[constants]; } (the OpenGL backends flatten it into plain uniforms of these names)
        const uint32_t c_array = b.type_array(vec4, constants);
        b.decorate(c_array, ArrayStride, {16});
        const uint32_t block = b.type_struct({vec4, vec4, c_array}, "ubo" + std::to_string(constants));
        b.decorate(block, Block);
        b.decorate_member(block, 0, Offset, {0});
        b.decorate_member(block, 1, Offset, {16});
        b.decorate_member(block, 2, Offset, {32});
        b.set_member_name(block, 0, "u_fix0");
        b.set_member_name(block, 1, "u_fix1");
        b.set_member_name(block, 2, ps() ? "u_psc" : "u_vsc");
        b.set_name(block, ps() ? "PixelConstants" : "VertexConstants");
        ubo_var = b.global_variable(block, Uniform, ps() ? "ps_constants" : "vs_constants");
        b.decorate(ubo_var, DescriptorSet, {0});
        b.decorate(ubo_var, Binding, {ps() ? 1u : 0u});

        const uint32_t entry_label = b.begin_block();
        (void)entry_label;
        // zero the registers the shader touches (undefined in Direct3D 9; zero keeps results deterministic)
        std::set<uint32_t> used_temps, used_tex;
        std::set<std::pair<RegType, uint16_t>> used_outputs;
        auto note_dst = [&](const DstParam& d) {
            if (d.type == RegType::temp) used_temps.insert(d.index);
            if (d.type == RegType::addr_texture && ps()) used_tex.insert(d.index);
            if (d.type == RegType::rast_out || d.type == RegType::attr_out || d.type == RegType::texcoord_out || d.type == RegType::color_out) used_outputs.insert({d.type, d.index});
        };
        for (const Instruction& in : p.code) {
            if (in.has_dst && in.opcode != op_dcl && in.opcode != op_def) note_dst(in.dst);
            for (int k = 0; k < in.src_count; ++k) {
                if (in.src[k].type == RegType::temp) used_temps.insert(in.src[k].index);
                if (in.src[k].type == RegType::addr_texture && ps()) used_tex.insert(in.src[k].index);
            }
            // implicit texture-register reads of the ps 1.x texture instructions
            if (ps() && (in.opcode == op_tex || in.opcode == op_texcoord) && p.minor != 4) used_tex.insert(in.dst.index);
        }
        for (uint32_t r : used_temps) b.store(temp(uint16_t(r)), b.const_vec4(0, 0, 0, 0));
        if (!ps()) {   // fixed interface: oD0, oD1, oT0..oT7 and oFog always exist, so a pixel shader's inputs line up register for register
            for (uint16_t i = 0; i < 2; ++i) used_outputs.insert({RegType::attr_out, i});
            for (uint16_t i = 0; i < 8; ++i) used_outputs.insert({RegType::texcoord_out, i});
            used_outputs.insert({RegType::rast_out, 1});
        }
        // Unwritten vertex outputs: the diffuse colour defaults to white (measured on the real Direct3D 9 device), everything else to zero.
        for (auto& [type, index] : used_outputs) b.store(output(type, index), type == RegType::attr_out && index == 0 && !ps() ? b.const_vec4(1, 1, 1, 1) : b.const_vec4(0, 0, 0, 0));
        if (!ps()) for (const Instruction& in : p.code) if (in.has_dst && in.opcode != op_dcl && in.dst.type == RegType::addr_texture) { if (!addr_var) { addr_var = b.local_variable(vec4, "a0"); b.store(addr_var, b.const_vec4(0, 0, 0, 0)); } }
        for (uint32_t t : used_tex) {   // texture registers start as the interpolated coordinates
            if (ps()) b.store(texreg(uint16_t(t)), b.load(vec4, interpolant(RegType::addr_texture, uint16_t(t))));
        }

        for (const Instruction& in : p.code) {
            instruction(in);
            if (!ok) return false;
        }

        if (ps()) {
            const uint32_t sink = b.local_variable(vec4, "interface_sink");
            for (uint16_t i = 0; i < 2; ++i) b.store(sink, b.load(vec4, interpolant(RegType::input, i)));
            for (uint16_t i = 0; i < 8; ++i) b.store(sink, b.load(vec4, interpolant(RegType::addr_texture, i)));
            fog_input = b.global_variable(f32, Input, "vary10");
            b.decorate(fog_input, Location, {10});
            interface_ids.push_back(fog_input);
            b.store(sink, b.emit(CompositeConstruct, vec4, {b.load(f32, fog_input), b.const_float(0.0f), b.const_float(0.0f), b.const_float(0.0f)}));
        }
        // ---- epilogue
        uint32_t color_out = 0, pos_out = 0;
        if (ps()) {
            const uint32_t var = b.global_variable(vec4, Output, "oC0");
            b.decorate(var, Location, {0});
            interface_ids.push_back(var);
            uint32_t value;
            if (sm1()) value = b.load(vec4, temp(0));   // pixel shaders 1.x return r0
            else if (outputs.count((uint32_t(RegType::color_out) << 8) | 0)) value = b.load(vec4, output(RegType::color_out, 0));
            else value = b.const_vec4(0, 0, 0, 0);
            if (o.fog_mode) {   // the interpolated fog factor of the vertex stage is the blend factor (1 = no fog)
                const uint32_t f = b.load(f32, fog_input);
                const uint32_t f4 = b.emit(CompositeConstruct, vec4, {f, f, f, b.const_float(1.0f)});
                const uint32_t fog_color = b.load(vec4, b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(1)}));
                value = b.ext(vec4, FMix, {fog_color, value, f4});
            }
            if (o.alpha_func && o.alpha_func != 8) {   // Direct3D 9 compares the alpha quantised to 8 bits with the reference (0..255)
                const uint32_t ref = component(b.load(vec4, b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(0)})), 0);
                const uint32_t alpha = b.ext(f32, Floor, {b.emit(FAdd, f32, {b.emit(FMul, f32, {component(value, 3), b.const_float(255.0f)}), b.const_float(0.5f)})});
                uint32_t pass;
                switch (o.alpha_func) {
                case 1: pass = b.emit(FOrdLessThan, boolean, {b.const_float(1.0f), b.const_float(0.0f)}); break;   // never
                case 2: pass = b.emit(FOrdLessThan, boolean, {alpha, ref}); break;
                case 3: pass = b.emit(FOrdEqual, boolean, {alpha, ref}); break;
                case 4: pass = b.emit(FOrdLessThanEqual, boolean, {alpha, ref}); break;
                case 5: pass = b.emit(FOrdGreaterThan, boolean, {alpha, ref}); break;
                case 6: pass = b.emit(FOrdNotEqual, boolean, {alpha, ref}); break;
                default: pass = b.emit(FOrdGreaterThanEqual, boolean, {alpha, ref}); break;
                }
                const uint32_t keep = b.new_id(), discard = b.new_id();
                b.emit_void(SelectionMerge, {keep, 0});
                b.emit_void(BranchConditional, {pass, keep, discard});
                b.label(discard);
                b.emit_void(Kill, {});
                b.label(keep);
            }
            b.store(var, value);
            color_out = var;
        } else {
            pos_out = b.global_variable(vec4, Output, "gl_Position");
            b.decorate(pos_out, BuiltIn, {Position});
            interface_ids.push_back(pos_out);
            uint32_t pos = outputs.count((uint32_t(RegType::rast_out) << 8) | 0) ? b.load(vec4, output(RegType::rast_out, 0)) : b.const_vec4(0, 0, 0, 1);
            if (o.viewport_fixup) {
                const uint32_t fix0 = b.load(vec4, b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(0)}));
                const uint32_t fix1 = b.load(vec4, b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(1)}));
                const uint32_t w = component(pos, 3);
                auto scaled = [&](uint32_t value, uint32_t scale, uint32_t bias) { return b.emit(FAdd, f32, {b.emit(FMul, f32, {value, scale}), b.emit(FMul, f32, {w, bias})}); };
                const uint32_t x = scaled(component(pos, 0), component(fix0, 0), component(fix0, 2));
                const uint32_t y = scaled(component(pos, 1), component(fix0, 1), component(fix0, 3));
                const uint32_t z = scaled(component(pos, 2), component(fix1, 0), component(fix1, 1));
                pos = b.emit(CompositeConstruct, vec4, {x, y, z, w});
            }
            b.store(pos_out, pos);
            for (auto& [key, local] : outputs) {
                const RegType type = RegType(key >> 8);
                const uint16_t index = uint16_t(key & 0xFF);
                if (type == RegType::rast_out && index == 0) continue;
                if (type == RegType::rast_out && index == 2) {   // oPts
                    const uint32_t var = b.global_variable(f32, Output, "gl_PointSize");
                    b.decorate(var, BuiltIn, {PointSize});
                    interface_ids.push_back(var);
                    b.store(var, component(b.load(vec4, local), 0));
                    continue;
                }
                uint32_t location, value = b.load(vec4, local);
                uint32_t type_id = vec4;
                if (type == RegType::attr_out) { location = index; value = b.ext(vec4, FClamp, {value, splat(0.0f), splat(1.0f)}); }
                else if (type == RegType::texcoord_out) location = 2 + index;
                else if (type == RegType::rast_out && index == 1) {   // oFog: a scalar, clamped
                    location = 10; type_id = f32;
                    value = b.ext(f32, FClamp, {component(value, 0), b.const_float(0.0f), b.const_float(1.0f)});
                } else { fail("unsupported vertex output"); return false; }
                const uint32_t var = b.global_variable(type_id, Output, "vary" + std::to_string(location));
                b.decorate(var, Location, {location});
                interface_ids.push_back(var);
                b.store(var, value);
            }
        }
        (void)color_out;
        b.emit_void(Return, {});
        out.spirv = b.finish(ps() ? Fragment : Vertex, "main", interface_ids, ps());
        return true;
    }
};

}  // namespace

bool translate_to_spirv(const Program& program, const TranslateOptions& options, TranslateResult& out) {
    Lowering lowering(program, options, out);
    const bool ok = lowering.run();
    if (!ok && out.error.empty()) out.error = "translation failed";
    return ok;
}

}  // namespace nue::shader
