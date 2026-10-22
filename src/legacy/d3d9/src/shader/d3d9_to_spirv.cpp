// Lowering of decoded Direct3D 9 shaders (models 1.0 - 3.0) to SPIR-V. Instruction semantics follow the Direct3D 9 documentation and were checked against
// DXVK's dxso compiler (zlib licence; the structured control-flow shapes and several instruction definitions are adapted from it).
#include "nue/shader/d3d9_to_spirv.hpp"
#include "shader/spirv_builder.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>

namespace nue::shader {

// bgfx::Attrib::Enum (vendored bgfx, include/bgfx/defines.h order): Position 0, Normal 1, Tangent 2, Bitangent 3, Color0..3 = 4..7, Indices 8,
// Weight 9, TexCoord0..15 = 10..25. The point-size usage has no attribute of its own and travels as the last texture coordinate.
uint32_t bgfx_attribute_for(uint32_t usage, uint32_t usage_index) {
    switch (usage) {
    case 0: return 0;                                        // position
    case 1: return 9;                                        // blend weight
    case 2: return 8;                                        // blend indices
    case 3: return 1;                                        // normal
    case 9: return 0;                                        // position (pre-transformed)
    case 5: return 10 + std::min<uint32_t>(usage_index, 15); // texcoord
    case 6: return 2;                                        // tangent
    case 7: return 3;                                        // binormal
    case 10: return 4 + std::min<uint32_t>(usage_index, 3);  // colour
    default: return 25;
    }
}

const char* bgfx_attribute_name(uint32_t attribute) {
    static const char* const names[] = {"a_position", "a_normal", "a_tangent", "a_bitangent", "a_color0", "a_color1", "a_color2", "a_color3", "a_indices", "a_weight",
        "a_texcoord0", "a_texcoord1", "a_texcoord2", "a_texcoord3", "a_texcoord4", "a_texcoord5", "a_texcoord6", "a_texcoord7", "a_texcoord8", "a_texcoord9",
        "a_texcoord10", "a_texcoord11", "a_texcoord12", "a_texcoord13", "a_texcoord14", "a_texcoord15"};
    return attribute < 26 ? names[attribute] : "a_texcoord15";
}

namespace {

using namespace spv;

// Interface between the stages: one location per Direct3D semantic, so a pixel shader finds what the vertex shader wrote whatever the model:
// colours 0, 1; texture coordinates 2..11; fog 12; colours 2, 3 at 13, 14; everything else from 16.
constexpr uint32_t fog_location = 12;
uint32_t varying_location(uint32_t usage, uint32_t index) {
    switch (usage) {
    case 10: return index < 2 ? index : 13 + (index - 2) % 2;
    case 5: return 2 + std::min<uint32_t>(index, 9);
    case 11: return fog_location;
    default: return 16 + std::min<uint32_t>(usage, 13);
    }
}

struct CfgBlock {
    enum Type { If, Loop } type = If;
    uint32_t test = 0, label_if = 0, label_else = 0, label_end = 0;   // if
    size_t header_position = 0;
    uint32_t label_header = 0, label_continue = 0, label_break = 0, counter = 0, stride = 0, saved_loop = 0;   // loop
};

struct Lowering {
    const Program& p;
    const TranslateOptions& o;
    TranslateResult& out;
    SpirvBuilder b;
    bool ok = true;

    uint32_t f32 = 0, i32 = 0, vec4 = 0, vec3 = 0, vec2 = 0, ivec4 = 0, boolean = 0, bvec4 = 0;
    uint32_t ubo_var = 0, fog_input = 0;
    uint32_t constants = 0;
    bool has_ib = false;                 // the uniform block carries the integer and boolean constant registers
    uint32_t ib_member = 0;
    std::vector<uint32_t> interface_ids;

    // register files (Function-storage variables)
    std::map<uint32_t, uint32_t> temps, texregs, outputs;   // outputs keyed by (type<<8|index)
    std::map<uint32_t, uint32_t> inputs_global;              // v# / interpolants, keyed by (type<<8|index): Input variable
    std::map<uint32_t, std::array<float, 4>> defs;           // def c#
    std::map<uint32_t, std::array<int32_t, 4>> defs_i;       // defi i#
    std::map<uint32_t, bool> defs_b;                         // defb b#
    std::map<uint32_t, VertexInput> vs_inputs;               // vertex shaders: by register
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> semantic_of_input, semantic_of_output;   // model 3: register -> (usage, index)
    uint32_t attribute_of_register[16] = {};
    uint32_t integer_input[32] = {};   // 0 float, 1 unsigned integer, 2 signed integer (by bgfx attribute)
    uint32_t image_var[16] = {}, sampler_var[16] = {};
    uint32_t image_dim[16] = {};
    uint32_t addr_var = 0, loop_var = 0, pred_var = 0, vpos_var = 0, vface_var = 0, depth_out_var = 0;
    std::map<uint32_t, uint32_t> color_out_vars;
    std::vector<CfgBlock> cfg;
    std::map<uint32_t, std::pair<size_t, size_t>> subroutines;   // label -> [first instruction, ret]
    int call_depth = 0;
    const Instruction* current = nullptr;

    Lowering(const Program& program, const TranslateOptions& options, TranslateResult& result) : p(program), o(options), out(result) {}

    void fail(const std::string& why) { if (ok) out.error = why; ok = false; }
    bool ps() const { return p.stage == Stage::pixel; }
    bool sm1() const { return p.major < 2; }
    bool sm3() const { return p.major >= 3; }

    uint32_t vec(uint32_t n) { return b.type_vec(f32, n); }
    uint32_t splat(float v) { return b.const_vec4(v, v, v, v); }
    uint32_t component(uint32_t v, uint32_t i) { return b.emit(CompositeExtract, f32, {v, i}); }
    uint32_t replicate(uint32_t scalar) { return b.emit(CompositeConstruct, vec4, {scalar, scalar, scalar, scalar}); }
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
    uint32_t address_var() { if (!addr_var) { addr_var = b.local_variable(vec4, "a0"); } return addr_var; }
    uint32_t loop_counter() { if (!loop_var) { loop_var = b.local_variable(i32, "aL"); } return loop_var; }
    uint32_t predicate_var() { if (!pred_var) { pred_var = b.local_variable(bvec4, "p0"); } return pred_var; }

    // ---- stage interface -------------------------------------------------------------------------------------------------------------------
    uint32_t interpolant_at(uint32_t location, uint32_t mask_bit) {
        const uint32_t key = (0x100u << 8) | location;
        auto it = inputs_global.find(key);
        if (it != inputs_global.end()) return it->second;
        const uint32_t var = b.global_variable(vec4, Input, "vary" + std::to_string(location));   // stage-to-stage matching is by name in OpenGL ES 3.0
        b.decorate(var, Location, {location});
        interface_ids.push_back(var);
        out.interpolants_read |= mask_bit;
        return inputs_global[key] = var;
    }
    uint32_t interpolant(RegType type, uint16_t index) {   // pixel shader inputs
        if (sm3() && type == RegType::input) {              // model 3: by the semantic the register was declared with
            auto d = semantic_of_input.find(index);
            if (d != semantic_of_input.end()) {
                const uint32_t location = varying_location(d->second.first, d->second.second);
                return interpolant_at(location, location < 32 ? 1u << location : 0);
            }
        }
        const uint32_t location = type == RegType::input ? index : 2 + index;
        return interpolant_at(location, 1u << location);
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
        const bool is_uint = in.attribute < 32 && ((o.uint_attributes >> in.attribute) & 1), is_sint = in.attribute < 32 && ((o.sint_attributes >> in.attribute) & 1);
        const uint32_t input_type = is_uint ? b.type_vec(b.type_uint(), 4) : is_sint ? b.type_vec(b.type_int(), 4) : vec4;
        integer_input[in.attribute < 32 ? in.attribute : 0] = is_uint ? 1 : is_sint ? 2 : 0;
        const uint32_t var = b.global_variable(input_type, Input, bgfx_attribute_name(in.attribute));
        b.decorate(var, Location, {in.attribute});
        interface_ids.push_back(var);
        out.inputs.push_back(in);
        attribute_of_register[reg & 15] = in.attribute;
        return inputs_global[key] = var;
    }
    uint32_t builtin_input(uint32_t builtin, uint32_t type, const char* name) {
        const uint32_t var = b.global_variable(type, Input, name);
        b.decorate(var, BuiltIn, {builtin});
        interface_ids.push_back(var);
        return var;
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
            out.sampler_dim[stage] = uint8_t(dim);
        }
        return image_var[stage];
    }

    // ---- constants -------------------------------------------------------------------------------------------------------------------------
    uint32_t constant_pointer(uint32_t index_id) {
        return b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(2), index_id});
    }
    uint32_t constant_value(uint16_t index) {
        auto d = defs.find(index);
        if (d != defs.end()) {
            std::array<float, 4> v = d->second;
            if (ps() && sm1() && o.clamp_ps1_constants) for (auto& x : v) x = std::clamp(x, -1.0f, 1.0f);
            return b.const_vec4(v[0], v[1], v[2], v[3]);
        }
        if (index >= constants) { fail("constant register beyond the uniform block"); return b.const_vec4(0, 0, 0, 0); }
        uint32_t value = b.load(vec4, constant_pointer(b.const_uint(index)));
        if (ps() && sm1() && o.clamp_ps1_constants) value = b.ext(vec4, FClamp, {value, splat(-1.0f), splat(1.0f)});
        return value;
    }
    uint32_t int_constant(uint16_t index) {   // ivec4
        auto d = defs_i.find(index);
        if (d != defs_i.end()) {
            const uint32_t x = b.const_int(d->second[0]), y = b.const_int(d->second[1]), z = b.const_int(d->second[2]), w = b.const_int(d->second[3]);
            return b.emit(CompositeConstruct, ivec4, {x, y, z, w});
        }
        if (!has_ib || index >= 16) return b.emit(CompositeConstruct, ivec4, {b.const_int(0), b.const_int(0), b.const_int(0), b.const_int(0)});
        const uint32_t ptr = b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(ib_member), b.const_uint(index)});
        return b.emit(ConvertFToS, ivec4, {b.load(vec4, ptr)});
    }
    uint32_t bool_constant(uint16_t index) {   // scalar bool
        auto d = defs_b.find(index);
        if (d != defs_b.end()) return d->second ? b.emit(FOrdLessThan, boolean, {b.const_float(0.0f), b.const_float(1.0f)}) : b.emit(FOrdLessThan, boolean, {b.const_float(1.0f), b.const_float(0.0f)});
        if (!has_ib || index >= 16) return b.emit(FOrdLessThan, boolean, {b.const_float(1.0f), b.const_float(0.0f)});
        const uint32_t ptr = b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(ib_member), b.const_uint(16u + index)});
        return b.emit(FOrdNotEqual, boolean, {component(b.load(vec4, ptr), 0), b.const_float(0.0f)});
    }

    // ---- source operands -------------------------------------------------------------------------------------------------------------------
    uint32_t relative_index(const SrcParam& s) {
        uint32_t base;
        if (sm1() || (!s.relative_loop && true)) {
            if (sm1()) base = b.emit(ConvertFToS, i32, {component(b.load(vec4, address_var()), 0)});
            else base = b.emit(ConvertFToS, i32, {component(b.load(vec4, address_var()), s.relative_component)});
        } else base = b.load(i32, loop_counter());
        return b.emit(IAdd, i32, {base, b.const_int(int32_t(s.index))});
    }

    uint32_t raw_source(const SrcParam& s) {
        if (s.relative) {
            if (s.type != RegType::const_float) { fail("relative addressing of a register file other than constants"); return b.const_vec4(0, 0, 0, 0); }
            if (ps() && !sm3()) { fail("relative addressing in a pixel shader below model 3"); return b.const_vec4(0, 0, 0, 0); }
            uint32_t index = relative_index(s);
            index = b.ext(i32, SClamp, {index, b.const_int(0), b.const_int(int32_t(constants) - 1)});
            return b.load(vec4, constant_pointer(index));
        }
        switch (s.type) {
        case RegType::temp: return b.load(vec4, temp(s.index));
        case RegType::const_float: return constant_value(s.index);
        case RegType::input: {
            if (ps()) return b.load(vec4, interpolant(RegType::input, s.index));
            const uint32_t var = vertex_input(s.index);
            const uint32_t attribute = attribute_of_register[s.index];
            const uint32_t kind = attribute < 32 ? integer_input[attribute] : 0;
            uint32_t value;
            if (kind == 1) value = b.emit(ConvertUToF, vec4, {b.load(b.type_vec(b.type_uint(), 4), var)});
            else if (kind == 2) value = b.emit(ConvertSToF, vec4, {b.load(b.type_vec(b.type_int(), 4), var)});
            else value = b.load(vec4, var);
            if (attribute < 32 && ((o.bgra_attributes >> attribute) & 1)) value = b.emit(VectorShuffle, vec4, {value, value, 2u, 1u, 0u, 3u});
            return value;
        }
        case RegType::addr_texture:
            return ps() ? b.load(vec4, texreg(s.index)) : b.load(vec4, address_var());
        case RegType::const_int: return b.emit(ConvertSToF, vec4, {int_constant(s.index)});
        case RegType::const_bool: { const uint32_t v = b.emit(Select, f32, {bool_constant(s.index), b.const_float(1.0f), b.const_float(0.0f)}); return replicate(v); }
        case RegType::loop: { const uint32_t v = b.emit(ConvertSToF, f32, {b.load(i32, loop_counter())}); return replicate(v); }
        case RegType::misc:
            if (s.index == 0) {   // vPos: the pixel coordinates (Direct3D 9 pixel centres are integers)
                if (!vpos_var) vpos_var = builtin_input(FragCoord, vec4, "gl_FragCoord");
                const uint32_t frag = b.load(vec4, vpos_var);
                return b.emit(FSub, vec4, {frag, b.const_vec4(0.5f, 0.5f, 0.0f, 0.0f)});
            } else {              // vFace: +1 front, -1 back
                if (!vface_var) vface_var = builtin_input(FrontFacing, boolean, "gl_FrontFacing");
                const uint32_t v = b.emit(Select, f32, {b.load(boolean, vface_var), b.const_float(1.0f), b.const_float(-1.0f)});
                return replicate(v);
            }
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
        case SrcModifier::not_: break;   // only meaningful for predicates (handled by the consumers)
        default: fail("unsupported source modifier"); break;
        }
        return v;
    }

    // ---- destination operands --------------------------------------------------------------------------------------------------------------
    uint32_t dest_variable(const DstParam& d) {
        switch (d.type) {
        case RegType::temp: return temp(d.index);
        case RegType::addr_texture: return ps() ? texreg(d.index) : address_var();
        case RegType::rast_out: case RegType::attr_out: case RegType::texcoord_out: return output(d.type, d.index);
        case RegType::color_out: {
            if (!ps()) return output(d.type, d.index);
            auto it = color_out_vars.find(d.index);
            if (it != color_out_vars.end()) return it->second;
            return color_out_vars[d.index] = b.local_variable(vec4, "oC" + std::to_string(d.index));
        }
        case RegType::depth_out: if (!depth_out_var) depth_out_var = b.local_variable(vec4, "oDepth"); return depth_out_var;
        case RegType::predicate: return predicate_var();
        default: fail("unsupported destination register type " + std::to_string(unsigned(d.type))); return temp(0);
        }
    }

    uint32_t predicate_vector() {   // the instruction's predicate as a bvec4 in destination order
        uint32_t v = b.load(bvec4, predicate_var());
        const SrcParam& s = current->predicate;
        v = b.emit(VectorShuffle, bvec4, {v, v, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]});
        if (s.modifier == SrcModifier::not_) v = b.emit(LogicalNot, bvec4, {v});
        return v;
    }

    void write(const DstParam& d, uint32_t value) {
        if (d.shift) value = b.emit(FMul, vec4, {value, splat(d.shift > 0 ? float(1 << d.shift) : 1.0f / float(1 << -d.shift))});
        if (d.modifiers & mod_saturate) value = b.ext(vec4, FClamp, {value, splat(0.0f), splat(1.0f)});
        const uint32_t var = dest_variable(d);
        const bool predicated = current && current->predicated;
        if (d.mask != 0xF || predicated) {
            const uint32_t old = b.load(vec4, var);
            if (predicated) value = b.emit(Select, vec4, {predicate_vector(), value, old});
            if (d.mask != 0xF) value = b.emit(VectorShuffle, vec4, {old, value, (d.mask & 1) ? 4u : 0u, (d.mask & 2) ? 5u : 1u, (d.mask & 4) ? 6u : 2u, (d.mask & 8) ? 7u : 3u});
        }
        b.store(var, value);
    }

    // ---- texture sampling ------------------------------------------------------------------------------------------------------------------
    uint32_t sample(uint32_t stage, uint32_t coords_value, uint32_t mode = 0, uint32_t operand = 0) {   // mode: 0 implicit, 1 projected, 2 bias, 3 explicit lod
        const uint32_t dim = ps() && image_dim[stage] ? image_dim[stage] : o.sampler_dim[stage];
        const uint32_t image_type = b.type_image(dim);
        sampler_image(stage, dim);
        const uint32_t image = b.load(image_type, image_var[stage]);
        const uint32_t sampler = b.load(b.type_sampler(), sampler_var[stage]);
        const uint32_t combined = b.emit(SampledImage, b.type_sampled_image(image_type), {image, sampler});
        unsigned projected = sm1() ? o.projected[stage] : 0;
        if (mode == 1) projected = 4;
        const uint32_t coord_n = dim == 1 ? 2 : 3;
        auto coords = [&](bool with_divisor, uint32_t divisor_index) -> uint32_t {
            if (!with_divisor) return head(coords_value, coord_n);
            const uint32_t q = component(coords_value, divisor_index);
            if (dim == 1) return b.emit(CompositeConstruct, vec3, {component(coords_value, 0), component(coords_value, 1), q});
            return b.emit(CompositeConstruct, vec4, {component(coords_value, 0), component(coords_value, 1), component(coords_value, 2), q});
        };
        if (mode == 3) {
            if (projected) return b.emit(ImageSampleProjExplicitLod, vec4, {combined, coords(true, projected == 4 ? 3u : 2u), Lod, operand});
            return b.emit(ImageSampleExplicitLod, vec4, {combined, head(coords_value, coord_n), Lod, operand});
        }
        if (projected) return b.emit(ImageSampleProjImplicitLod, vec4, {combined, coords(true, projected == 4 ? 3u : 2u)});
        if (mode == 2) return b.emit(ImageSampleImplicitLod, vec4, {combined, head(coords_value, coord_n), Bias, operand});
        return b.emit(ImageSampleImplicitLod, vec4, {combined, head(coords_value, coord_n)});
    }

    // ---- structured control flow -----------------------------------------------------------------------------------------------------------
    uint32_t compare_scalar(uint32_t control, uint32_t a, uint32_t c) {   // D3DSPC: 1 gt, 2 eq, 3 ge, 4 lt, 5 ne, 6 le
        switch (control) {
        case 1: return b.emit(FOrdGreaterThan, boolean, {a, c});
        case 2: return b.emit(FOrdEqual, boolean, {a, c});
        case 3: return b.emit(FOrdGreaterThanEqual, boolean, {a, c});
        case 4: return b.emit(FOrdLessThan, boolean, {a, c});
        case 5: return b.emit(FOrdNotEqual, boolean, {a, c});
        default: return b.emit(FOrdLessThanEqual, boolean, {a, c});
        }
    }
    uint32_t compare_vector(uint32_t control, uint32_t a, uint32_t c) {
        switch (control) {
        case 1: return b.emit(FOrdGreaterThan, bvec4, {a, c});
        case 2: return b.emit(FOrdEqual, bvec4, {a, c});
        case 3: return b.emit(FOrdGreaterThanEqual, bvec4, {a, c});
        case 4: return b.emit(FOrdLessThan, bvec4, {a, c});
        case 5: return b.emit(FOrdNotEqual, bvec4, {a, c});
        default: return b.emit(FOrdLessThanEqual, bvec4, {a, c});
        }
    }
    uint32_t scalar_condition(const SrcParam& s) {   // the .x of a boolean-ish source (b#, p0 component, or a float compared with zero)
        if (s.type == RegType::const_bool && !s.relative) { uint32_t v = bool_constant(s.index); if (s.modifier == SrcModifier::not_) v = b.emit(LogicalNot, boolean, {v}); return v; }
        if (s.type == RegType::predicate) {
            const uint32_t pv = b.load(bvec4, predicate_var());
            uint32_t v = b.emit(CompositeExtract, boolean, {pv, uint32_t(s.swizzle[0])});
            if (s.modifier == SrcModifier::not_) v = b.emit(LogicalNot, boolean, {v});
            return v;
        }
        return b.emit(FOrdNotEqual, boolean, {component(source(s), 0), b.const_float(0.0f)});
    }

    void begin_if(uint32_t condition) {
        CfgBlock blk;
        blk.type = CfgBlock::If;
        blk.test = condition; blk.label_if = b.new_id(); blk.label_end = b.new_id();
        blk.header_position = b.body_position();
        cfg.push_back(blk);
        b.label(blk.label_if);
    }
    void do_else() {
        if (cfg.empty() || cfg.back().type != CfgBlock::If || cfg.back().label_else) { fail("else without if"); return; }
        CfgBlock& blk = cfg.back();
        blk.label_else = b.new_id();
        b.emit_void(Branch, {blk.label_end});
        b.label(blk.label_else);
    }
    void end_if() {
        if (cfg.empty() || cfg.back().type != CfgBlock::If) { fail("endif without if"); return; }
        const CfgBlock blk = cfg.back();
        cfg.pop_back();
        b.insert_selection_header(blk.header_position, blk.label_end, blk.test, blk.label_if, blk.label_else ? blk.label_else : blk.label_end);
        b.emit_void(Branch, {blk.label_end});
        b.label(blk.label_end);
    }
    void begin_loop(uint32_t count, bool with_counter, uint32_t start, uint32_t stride) {
        CfgBlock blk;
        blk.type = CfgBlock::Loop;
        blk.counter = b.local_variable(i32, "loop_count");
        b.store(blk.counter, count);
        blk.label_header = b.new_id(); blk.label_continue = b.new_id(); blk.label_break = b.new_id();
        const uint32_t label_cond = b.new_id(), label_body = b.new_id();
        if (with_counter) {   // `loop aL, iN`: aL runs start, start + stride ...; the previous value is restored when the loop ends
            blk.saved_loop = b.load(i32, loop_counter());
            blk.stride = stride;
            b.store(loop_counter(), start);
        }
        b.emit_void(Branch, {blk.label_header});
        b.label(blk.label_header);
        b.emit_void(LoopMerge, {blk.label_break, blk.label_continue, 0});
        b.emit_void(Branch, {label_cond});
        b.label(label_cond);
        const uint32_t remaining = b.load(i32, blk.counter);
        b.emit_void(BranchConditional, {b.emit(SGreaterThan, boolean, {remaining, b.const_int(0)}), label_body, blk.label_break});
        b.label(label_body);
        cfg.push_back(blk);
    }
    void end_loop() {
        if (cfg.empty() || cfg.back().type != CfgBlock::Loop) { fail("endloop without loop"); return; }
        const CfgBlock blk = cfg.back();
        cfg.pop_back();
        b.emit_void(Branch, {blk.label_continue});
        b.label(blk.label_continue);
        if (blk.stride) b.store(loop_counter(), b.emit(IAdd, i32, {b.load(i32, loop_counter()), blk.stride}));
        b.store(blk.counter, b.emit(ISub, i32, {b.load(i32, blk.counter), b.const_int(1)}));
        b.emit_void(Branch, {blk.label_header});
        b.label(blk.label_break);
        if (blk.saved_loop) b.store(loop_counter(), blk.saved_loop);
    }
    CfgBlock* innermost_loop() { for (auto it = cfg.rbegin(); it != cfg.rend(); ++it) if (it->type == CfgBlock::Loop) return &*it; return nullptr; }
    void do_break(uint32_t condition) {   // condition 0: unconditional
        CfgBlock* loop = innermost_loop();
        if (!loop) { fail("break outside a loop"); return; }
        if (!condition) {
            b.emit_void(Branch, {loop->label_break});
            b.label(b.new_id());          // dead block: the following instructions need an open block
            return;
        }
        const uint32_t label_break = b.new_id(), label_merge = b.new_id();
        b.emit_void(SelectionMerge, {label_merge, 0});
        b.emit_void(BranchConditional, {condition, label_break, label_merge});
        b.label(label_break);
        b.emit_void(Branch, {loop->label_break});
        b.label(label_merge);
    }
    void conditional_kill(uint32_t kill_condition) {
        const uint32_t label_kill = b.new_id(), label_merge = b.new_id();
        b.emit_void(SelectionMerge, {label_merge, 0});
        b.emit_void(BranchConditional, {kill_condition, label_kill, label_merge});
        b.label(label_kill);
        b.emit_void(Kill, {});
        b.label(label_merge);
    }

    // ---- instructions ----------------------------------------------------------------------------------------------------------------------
    void run_range(size_t first, size_t last) {
        for (size_t i = first; i < last && ok; ++i) {
            const Instruction& in = p.code[i];
            if (in.opcode == op_label) {   // a subroutine definition: skipped in the main flow (it is inlined at its calls)
                while (i < last && p.code[i].opcode != op_ret) ++i;
                continue;
            }
            if (in.opcode == op_ret) return;
            instruction(in);
        }
    }

    void instruction(const Instruction& in) {
        current = &in;
        switch (in.opcode) {
        case op_nop: case op_phase: case op_dcl: case op_def: case op_defi: case op_defb: return;
        case op_mov: {
            uint32_t value = source(in.src[0]);
            if (!ps() && sm1() && in.dst.type == RegType::addr_texture) value = b.ext(vec4, Floor, {value});   // vs 1.x: mov to a0 floors
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
        case op_exp: case op_expp: {
            auto a = component(source(in.src[0]), 0);
            if (in.opcode == op_expp && !ps() && sm1()) {   // expp of vertex shader 1.x: the vector form (2^floor(x), x - floor(x), 2^x, 1); exp itself is scalar
                const uint32_t fl = b.ext(f32, Floor, {a});
                write(in.dst, b.emit(CompositeConstruct, vec4, {b.ext(f32, Exp2, {fl}), b.emit(FSub, f32, {a, fl}), b.ext(f32, Exp2, {a}), b.const_float(1.0f)}));
            } else write(in.dst, replicate(b.ext(f32, Exp2, {a})));
            return;
        }
        case op_log: case op_logp: { auto a = b.ext(f32, FAbs, {component(source(in.src[0]), 0)}); write(in.dst, replicate(b.ext(f32, Log2, {a}))); return; }
        case op_pow: {   // |x|^y (Direct3D 9 takes the absolute value of the base); 0^0 is 1
            const uint32_t base = b.ext(f32, FAbs, {component(source(in.src[0]), 0)});
            const uint32_t exponent = component(source(in.src[1]), 0);
            write(in.dst, replicate(b.ext(f32, Pow, {base, exponent})));
            return;
        }
        case op_frc: write(in.dst, b.ext(vec4, Fract, {source(in.src[0])})); return;
        case op_abs: write(in.dst, b.ext(vec4, FAbs, {source(in.src[0])})); return;
        case op_sgn: write(in.dst, b.ext(vec4, FSign, {source(in.src[0])})); return;
        case op_nrm: {   // r * rsq(r . r) over the first three components
            const uint32_t v = source(in.src[0]);
            const uint32_t d = dot_n(v, v, 3);
            write(in.dst, b.emit(FMul, vec4, {v, replicate(b.ext(f32, InverseSqrt, {d}))}));
            return;
        }
        case op_crs: {
            const uint32_t a = head(source(in.src[0]), 3), c = head(source(in.src[1]), 3);
            const uint32_t cr = b.ext(vec3, Cross, {a, c});
            write(in.dst, b.emit(CompositeConstruct, vec4, {b.emit(CompositeExtract, f32, {cr, 0u}), b.emit(CompositeExtract, f32, {cr, 1u}), b.emit(CompositeExtract, f32, {cr, 2u}), b.const_float(0.0f)}));
            return;
        }
        case op_sincos: {   // x = cos(src), y = sin(src), z = w = 0
            const uint32_t a = component(source(in.src[0]), 0);
            write(in.dst, b.emit(CompositeConstruct, vec4, {b.ext(f32, Cos, {a}), b.ext(f32, Sin, {a}), b.const_float(0.0f), b.const_float(0.0f)}));
            return;
        }
        case op_lit: {   // x = 1, y = max(src.x, 0), z = src.x > 0 ? max(src.y, 0)^clamp(src.w) : 0, w = 1
            const uint32_t v = source(in.src[0]);
            const uint32_t x = component(v, 0), y = component(v, 1), w = component(v, 3);
            const uint32_t power = b.ext(f32, FClamp, {w, b.const_float(-127.9961f), b.const_float(127.9961f)});
            const uint32_t zero = b.const_float(0.0f);
            const uint32_t yv = b.ext(f32, FMax, {x, zero});
            const uint32_t pw = b.ext(f32, Pow, {b.ext(f32, FMax, {y, zero}), power});
            const uint32_t both = b.emit(LogicalAnd, boolean, {b.emit(FOrdGreaterThanEqual, boolean, {x, zero}), b.emit(FOrdGreaterThanEqual, boolean, {y, zero})});
            const uint32_t zv = b.emit(Select, f32, {both, pw, zero});
            write(in.dst, b.emit(CompositeConstruct, vec4, {b.const_float(1.0f), yv, zv, b.const_float(1.0f)}));
            return;
        }
        case op_dst: {   // x = 1, y = src0.y * src1.y, z = src0.z, w = src1.w
            const uint32_t a = source(in.src[0]), c = source(in.src[1]);
            write(in.dst, b.emit(CompositeConstruct, vec4, {b.const_float(1.0f), b.emit(FMul, f32, {component(a, 1), component(c, 1)}), component(a, 2), component(c, 3)}));
            return;
        }
        case op_slt: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, compare_select(FOrdLessThan, a, c, splat(1.0f), splat(0.0f))); return; }
        case op_sge: { auto a = source(in.src[0]), c = source(in.src[1]); write(in.dst, compare_select(FOrdGreaterThanEqual, a, c, splat(1.0f), splat(0.0f))); return; }
        case op_lrp: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]); write(in.dst, b.ext(vec4, FMix, {d, c, a})); return; }
        case op_cmp: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]); write(in.dst, compare_select(FOrdGreaterThanEqual, a, splat(0.0f), c, d)); return; }
        case op_cnd: { auto a = source(in.src[0]), c = source(in.src[1]), d = source(in.src[2]); write(in.dst, compare_select(FOrdGreaterThan, a, splat(0.5f), c, d)); return; }
        case op_dsx: write(in.dst, b.emit(DPdx, vec4, {source(in.src[0])})); return;
        case op_dsy: write(in.dst, b.emit(DPdy, vec4, {source(in.src[0])})); return;
        case op_setp: {
            const uint32_t a = source(in.src[0]), c = source(in.src[1]);
            const uint32_t result = compare_vector(in.control, a, c);
            const uint32_t var = predicate_var();
            if (in.dst.mask == 0xF) { b.store(var, result); return; }
            const uint32_t old = b.load(bvec4, var);
            b.store(var, b.emit(VectorShuffle, bvec4, {old, result, (in.dst.mask & 1) ? 4u : 0u, (in.dst.mask & 2) ? 5u : 1u, (in.dst.mask & 4) ? 6u : 2u, (in.dst.mask & 8) ? 7u : 3u}));
            return;
        }
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
        // ---- control flow
        case op_if: begin_if(scalar_condition(in.src[0])); return;
        case op_ifc: begin_if(compare_scalar(in.control, component(source(in.src[0]), 0), component(source(in.src[1]), 0))); return;
        case op_else: do_else(); return;
        case op_endif: end_if(); return;
        case op_rep: {
            const uint32_t count = b.emit(CompositeExtract, i32, {int_constant(in.src[0].index), 0u});
            begin_loop(count, false, 0, 0);
            return;
        }
        case op_loop: {   // loop aL, iN: count x, start y, step z
            const uint32_t iv = int_constant(in.src[1].index);
            begin_loop(b.emit(CompositeExtract, i32, {iv, 0u}), true, b.emit(CompositeExtract, i32, {iv, 1u}), b.emit(CompositeExtract, i32, {iv, 2u}));
            return;
        }
        case op_endrep: case op_endloop: end_loop(); return;
        case op_break: do_break(0); return;
        case op_breakc: do_break(compare_scalar(in.control, component(source(in.src[0]), 0), component(source(in.src[1]), 0))); return;
        case op_breakp: do_break(scalar_condition(in.src[0])); return;
        case op_call: case op_callnz: {
            auto it = subroutines.find(in.src[0].index);
            if (it == subroutines.end() || call_depth > 8) { fail("call of an unknown or too deeply nested label"); return; }
            const bool conditional = in.opcode == op_callnz;
            if (conditional) begin_if(scalar_condition(in.src[1]));
            ++call_depth;
            run_range(it->second.first + 1, it->second.second);
            --call_depth;
            if (conditional) end_if();
            return;
        }
        // ---- texture instructions
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
            if (p.major >= 2) {                        // texld dst, coords, sampler (control: 1 projected, 2 bias)
                const uint32_t coords = source(in.src[0]);
                const uint32_t mode = in.control == 1 ? 1 : in.control == 2 ? 2 : 0;
                write(in.dst, sample(in.src[1].index, coords, mode, mode == 2 ? component(coords, 3) : 0));
            } else if (p.minor == 4 && in.src_count == 1) {   // texld rN, tM: sample stage N at the coordinates of tM
                write(in.dst, sample(in.dst.index, source(in.src[0])));
            } else {                                   // tex tN
                write(in.dst, sample(in.dst.index, b.load(vec4, texreg(in.dst.index))));
            }
            return;
        case op_texldl: {   // explicit level of detail in coords.w
            const uint32_t coords = source(in.src[0]);
            write(in.dst, sample(in.src[1].index, coords, 3, component(coords, 3)));
            return;
        }
        case op_texkill: {
            uint32_t coords;
            if (ps() && in.src_count == 0) {
                if (p.major < 2) coords = b.load(vec4, texreg(in.dst.index));   // texkill tN: the texture register
                else { SrcParam r; r.type = in.dst.type; r.index = in.dst.index; coords = raw_source(r); }   // texkill r0: the register named as the destination
            } else coords = source(in.src[0]);
            const uint32_t negative = b.emit(FOrdLessThan, bvec4, {coords, splat(0.0f)});
            const uint32_t any_xyz = b.emit(LogicalOr, boolean, {b.emit(LogicalOr, boolean, {b.emit(CompositeExtract, boolean, {negative, 0u}), b.emit(CompositeExtract, boolean, {negative, 1u})}), b.emit(CompositeExtract, boolean, {negative, 2u})});
            conditional_kill(any_xyz);
            return;
        }
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

    // ---- program ---------------------------------------------------------------------------------------------------------------------------
    void scan() {
        for (size_t i = 0; i < p.code.size(); ++i) {
            const Instruction& in = p.code[i];
            if (in.opcode == op_def && in.dst.type == RegType::const_float) defs[in.dst.index] = {in.immediate[0], in.immediate[1], in.immediate[2], in.immediate[3]};
            else if (in.opcode == op_defi) defs_i[in.dst.index] = {int32_t(in.immediate[0]), int32_t(in.immediate[1]), int32_t(in.immediate[2]), int32_t(in.immediate[3])};
            else if (in.opcode == op_defb) defs_b[in.dst.index] = in.immediate[0] != 0.0f;
            else if (in.opcode == op_dcl) {
                if (in.dst.type == RegType::input && !ps()) {
                    VertexInput vi{in.dst.index, in.dcl_usage, in.dcl_index, bgfx_attribute_for(in.dcl_usage, in.dcl_index)};
                    vs_inputs[in.dst.index] = vi;
                } else if (in.dst.type == RegType::input && ps()) {
                    semantic_of_input[in.dst.index] = {in.dcl_usage, in.dcl_index};
                } else if (in.dst.type == RegType::texcoord_out && !ps() && sm3()) {
                    semantic_of_output[in.dst.index] = {in.dcl_usage, in.dcl_index};
                } else if (in.dst.type == RegType::sampler) {
                    image_dim[in.dst.index] = in.dcl_texture_type == 3 ? 3 : in.dcl_texture_type == 4 ? 2 : 1;
                }
            } else if (in.opcode == op_label) {
                size_t end = i;
                while (end < p.code.size() && p.code[end].opcode != op_ret) ++end;
                subroutines[in.src[0].index] = {i, end};
            }
            for (int k = 0; k < in.src_count; ++k) {
                const SrcParam& s = in.src[k];
                if (s.type == RegType::const_int && !defs_i.count(s.index)) has_ib = true;
                if (s.type == RegType::const_bool && !defs_b.count(s.index)) has_ib = true;
            }
        }
    }

    bool run() {
        out = TranslateResult{};
        out.stage = p.stage;
        f32 = b.type_float(); i32 = b.type_int(); vec2 = vec(2); vec3 = vec(3); vec4 = b.type_vec4(); ivec4 = b.type_vec(i32, 4);
        boolean = b.type_bool(); bvec4 = b.type_vec(boolean, 4);
        constants = p.uses_relative_constants ? 256 : std::max<uint32_t>(p.constant_registers_read, 1);
        out.constant_registers = constants;
        scan();
        out.int_bool_registers = has_ib ? 32 : 0;

        // constants: a uniform block { vec4 u_fix0; vec4 u_fix1; vec4 c[constants]; [vec4 u_ib[32]]; } (the OpenGL backends flatten it into plain uniforms)
        const uint32_t c_array = b.type_array(vec4, constants);
        b.decorate(c_array, ArrayStride, {16});
        std::vector<uint32_t> members{vec4, vec4, c_array};
        uint32_t ib_array = 0;
        if (has_ib) { ib_array = b.type_array(vec4, 32); b.decorate(ib_array, ArrayStride, {16}); members.push_back(ib_array); ib_member = 3; }
        const uint32_t block = b.type_struct(members, "ubo" + std::to_string(constants) + (has_ib ? "ib" : ""));
        b.decorate(block, Block);
        b.decorate_member(block, 0, Offset, {0});
        b.decorate_member(block, 1, Offset, {16});
        b.decorate_member(block, 2, Offset, {32});
        if (has_ib) b.decorate_member(block, 3, Offset, {32 + 16 * constants});
        b.set_member_name(block, 0, "u_fix0");
        b.set_member_name(block, 1, "u_fix1");
        b.set_member_name(block, 2, ps() ? "u_psc" : "u_vsc");
        if (has_ib) b.set_member_name(block, 3, ps() ? "u_psib" : "u_vsib");
        b.set_name(block, ps() ? "PixelConstants" : "VertexConstants");
        ubo_var = b.global_variable(block, Uniform, ps() ? "ps_constants" : "vs_constants");
        b.decorate(ubo_var, DescriptorSet, {0});
        b.decorate(ubo_var, Binding, {ps() ? 1u : 0u});

        b.begin_block();
        // zero the registers the shader touches (undefined in Direct3D 9; zero keeps results deterministic)
        std::set<uint32_t> used_temps, used_tex;
        std::set<std::pair<RegType, uint16_t>> used_outputs;
        bool uses_a0 = false, uses_predicate = false, uses_loop = false;
        auto note_dst = [&](const DstParam& d) {
            if (d.type == RegType::temp) used_temps.insert(d.index);
            if (d.type == RegType::addr_texture && ps()) used_tex.insert(d.index);
            if (d.type == RegType::addr_texture && !ps()) uses_a0 = true;
            if (d.type == RegType::predicate) uses_predicate = true;
            if (d.type == RegType::rast_out || d.type == RegType::attr_out || d.type == RegType::texcoord_out || d.type == RegType::color_out) used_outputs.insert({d.type, d.index});
        };
        for (const Instruction& in : p.code) {
            if (in.has_dst && in.opcode != op_dcl && in.opcode != op_def && in.opcode != op_defi && in.opcode != op_defb) note_dst(in.dst);
            if (in.predicated) uses_predicate = true;
            if (in.opcode == op_loop) uses_loop = true;
            for (int k = 0; k < in.src_count; ++k) {
                if (in.src[k].type == RegType::temp) used_temps.insert(in.src[k].index);
                if (in.src[k].type == RegType::addr_texture && ps()) used_tex.insert(in.src[k].index);
                if (in.src[k].type == RegType::predicate) uses_predicate = true;
                if (in.src[k].relative) { if (in.src[k].relative_loop) uses_loop = true; else uses_a0 = true; }
            }
            // implicit texture-register reads of the ps 1.x texture instructions
            if (ps() && (in.opcode == op_tex || in.opcode == op_texcoord) && p.major < 2 && p.minor != 4) used_tex.insert(in.dst.index);
            if (ps() && in.opcode == op_texkill && p.major < 2) used_tex.insert(in.dst.index);
        }
        for (uint32_t r : used_temps) b.store(temp(uint16_t(r)), b.const_vec4(0, 0, 0, 0));
        if (uses_a0 && !ps()) b.store(address_var(), b.const_vec4(0, 0, 0, 0));
        if (uses_predicate) b.store(predicate_var(), b.emit(FOrdLessThan, bvec4, {b.const_vec4(1, 1, 1, 1), b.const_vec4(0, 0, 0, 0)}));
        if (uses_loop) b.store(loop_counter(), b.const_int(0));
        if (!ps()) {   // pre-model-3 interface: oD0, oD1, oT0..oT7 and oFog always exist, so a pixel shader's inputs line up register for register
            if (!sm3()) {
                for (uint16_t i = 0; i < 2; ++i) used_outputs.insert({RegType::attr_out, i});
                for (uint16_t i = 0; i < 8; ++i) used_outputs.insert({RegType::texcoord_out, i});
                used_outputs.insert({RegType::rast_out, 1});
            }
        }
        // Unwritten vertex outputs: the diffuse colour defaults to white (measured on the real Direct3D 9 device), everything else to zero.
        for (auto& [type, index] : used_outputs) {
            const bool white = !ps() && !sm3() && type == RegType::attr_out && index == 0;
            b.store(output(type, index), white ? b.const_vec4(1, 1, 1, 1) : b.const_vec4(0, 0, 0, 0));
        }
        for (uint32_t t : used_tex) {   // texture registers start as the interpolated coordinates
            if (ps()) b.store(texreg(uint16_t(t)), b.load(vec4, interpolant(RegType::addr_texture, uint16_t(t))));
        }
        if (ps()) for (auto& [reg, sem] : semantic_of_input) { (void)reg; (void)sem; }

        run_range(0, p.code.size());
        if (!ok) return false;
        if (!cfg.empty()) { fail("unterminated if/loop"); return false; }

        if (ps()) {
            // keep the whole fixed interface alive so that stages link register for register (OpenGL ES matches varyings by name)
            const uint32_t sink = b.local_variable(vec4, "interface_sink");
            if (!sm3()) {
                for (uint16_t i = 0; i < 2; ++i) b.store(sink, b.load(vec4, interpolant(RegType::input, i)));
                for (uint16_t i = 0; i < 8; ++i) b.store(sink, b.load(vec4, interpolant(RegType::addr_texture, i)));
            } else {   // locations 0..11 (colours, texture coordinates): the vertex shader writes all of them, whichever the pixel shader declared
                for (uint32_t location = 0; location < 12; ++location) b.store(sink, b.load(vec4, interpolant_at(location, 0)));
            }
            fog_input = b.global_variable(f32, Input, "vary" + std::to_string(fog_location));
            b.decorate(fog_input, Location, {fog_location});
            interface_ids.push_back(fog_input);
            b.store(sink, b.emit(CompositeConstruct, vec4, {b.load(f32, fog_input), b.const_float(0.0f), b.const_float(0.0f), b.const_float(0.0f)}));
        }
        // ---- epilogue
        if (ps()) epilogue_pixel(); else epilogue_vertex();
        b.emit_void(Return, {});
        out.spirv = b.finish(ps() ? Fragment : Vertex, "main", interface_ids, ps());
        return true;
    }

    void epilogue_pixel() {
        // colour 0: model 1 returns r0; models 2 and 3 write oC0
        uint32_t value;
        if (sm1()) value = b.load(vec4, temp(0));
        else if (color_out_vars.count(0)) value = b.load(vec4, color_out_vars[0]);
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
            conditional_kill(b.emit(LogicalNot, boolean, {pass}));
        }
        const uint32_t var = b.global_variable(vec4, Output, "oC0");
        b.decorate(var, Location, {0});
        interface_ids.push_back(var);
        b.store(var, value);
        for (auto& [index, local] : color_out_vars) {   // render targets 1..3
            if (index == 0 || index > 3) continue;
            const uint32_t extra = b.global_variable(vec4, Output, "oC" + std::to_string(index));
            b.decorate(extra, Location, {index});
            interface_ids.push_back(extra);
            b.store(extra, b.load(vec4, local));
        }
        if (depth_out_var) {
            const uint32_t depth = b.global_variable(f32, Output, "gl_FragDepth");
            b.decorate(depth, BuiltIn, {FragDepth});
            interface_ids.push_back(depth);
            b.add_execution_mode(12);
            b.store(depth, component(b.load(vec4, depth_out_var), 0));
        }
    }

    // Applies the viewport, depth range and half-pixel fixup, then writes every vertex output register to the varying of its semantic.
    void epilogue_vertex() {
        uint32_t pos_value = b.const_vec4(0, 0, 0, 1);
        bool have_position = false;
        uint32_t emitted_varyings = 0;   // bit per location below 32
        auto emit_varying = [&](uint32_t location, uint32_t type_id, uint32_t value) {
            if (location < 32) emitted_varyings |= 1u << location;
            const uint32_t var = b.global_variable(type_id, Output, "vary" + std::to_string(location));
            b.decorate(var, Location, {location});
            interface_ids.push_back(var);
            b.store(var, value);
        };
        for (auto& [key, local] : outputs) {
            const RegType type = RegType(key >> 8);
            const uint16_t index = uint16_t(key & 0xFF);
            uint32_t value = b.load(vec4, local);
            if (sm3() && type == RegType::texcoord_out) {   // model 3: o# registers with declared semantics
                auto sem = semantic_of_output.find(index);
                if (sem == semantic_of_output.end()) continue;
                const uint32_t usage = sem->second.first, usage_index = sem->second.second;
                if (usage == 0 || usage == 9) { pos_value = value; have_position = true; continue; }
                if (usage == 4) {
                    const uint32_t var = b.global_variable(f32, Output, "gl_PointSize");
                    b.decorate(var, BuiltIn, {PointSize});
                    interface_ids.push_back(var);
                    b.store(var, component(value, 0));
                    continue;
                }
                if (usage == 11) { emit_varying(fog_location, f32, b.ext(f32, FClamp, {component(value, 0), b.const_float(0.0f), b.const_float(1.0f)})); continue; }
                if (usage == 10) value = b.ext(vec4, FClamp, {value, splat(0.0f), splat(1.0f)});
                emit_varying(varying_location(usage, usage_index), vec4, value);
                continue;
            }
            if (type == RegType::rast_out && index == 0) { pos_value = value; have_position = true; continue; }
            if (type == RegType::rast_out && index == 2) {   // oPts
                const uint32_t var = b.global_variable(f32, Output, "gl_PointSize");
                b.decorate(var, BuiltIn, {PointSize});
                interface_ids.push_back(var);
                b.store(var, component(value, 0));
                continue;
            }
            if (type == RegType::attr_out) { emit_varying(index, vec4, b.ext(vec4, FClamp, {value, splat(0.0f), splat(1.0f)})); continue; }
            if (type == RegType::texcoord_out) { emit_varying(2 + index, vec4, value); continue; }
            if (type == RegType::rast_out && index == 1) { emit_varying(fog_location, f32, b.ext(f32, FClamp, {component(value, 0), b.const_float(0.0f), b.const_float(1.0f)})); continue; }
            fail("unsupported vertex output");
            return;
        }
        (void)have_position;
        if (sm3()) {   // the pixel shader reads the whole fixed interface, so a model 3 vertex shader provides all of it (unwritten ones: 0, fog 1)
            for (uint32_t location = 0; location < 12; ++location) if (!(emitted_varyings & (1u << location))) emit_varying(location, vec4, splat(0.0f));
            if (!(emitted_varyings & (1u << fog_location))) emit_varying(fog_location, f32, b.const_float(1.0f));
        }
        const uint32_t pos_out = b.global_variable(vec4, Output, "gl_Position");
        b.decorate(pos_out, BuiltIn, {Position});
        interface_ids.push_back(pos_out);
        if (o.viewport_fixup) {
            const uint32_t fix0 = b.load(vec4, b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(0)}));
            const uint32_t fix1 = b.load(vec4, b.emit(AccessChain, b.type_ptr(Uniform, vec4), {ubo_var, b.const_uint(1)}));
            const uint32_t w = component(pos_value, 3);
            auto scaled = [&](uint32_t value, uint32_t scale, uint32_t bias) { return b.emit(FAdd, f32, {b.emit(FMul, f32, {value, scale}), b.emit(FMul, f32, {w, bias})}); };
            const uint32_t x = scaled(component(pos_value, 0), component(fix0, 0), component(fix0, 2));
            const uint32_t y = scaled(component(pos_value, 1), component(fix0, 1), component(fix0, 3));
            const uint32_t z = scaled(component(pos_value, 2), component(fix1, 0), component(fix1, 1));
            pos_value = b.emit(CompositeConstruct, vec4, {x, y, z, w});
        }
        b.store(pos_out, pos_value);
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
