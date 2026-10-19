// Direct3D 9 shader bytecode (vertex and pixel shader models 1.0 - 3.0): decoder and disassembler.
//
// Part of the shader path of docs/PLATFORM_API.md, 3.4. Portable C++ (no Windows types): the bytecode is data from the game, the decoder
// turns it into the D3D9-flavoured instruction form that the SPIR-V lowering (d3d9_to_spirv.hpp) consumes.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nue::shader {

enum class Stage : uint8_t { vertex, pixel };

// D3DSPR_*
enum class RegType : uint8_t {
    temp = 0, input = 1, const_float = 2, addr_texture = 3 /* a0 in vertex shaders, t# in pixel shaders */, rast_out = 4, attr_out = 5,
    texcoord_out = 6 /* oT# (or "output" in model 3) */, const_int = 7, color_out = 8, depth_out = 9, sampler = 10,
    const_float2 = 11, const_float3 = 12, const_float4 = 13, const_bool = 14, loop = 15, temp_float16 = 16, misc = 17, label = 18, predicate = 19
};

// D3DSIO_* (the ones this decoder knows by name; unknown opcodes make the decode fail).
enum Opcode : uint16_t {
    op_nop = 0, op_mov, op_add, op_sub, op_mad, op_mul, op_rcp, op_rsq, op_dp3, op_dp4, op_min, op_max, op_slt, op_sge, op_exp, op_log, op_lit, op_dst,
    op_lrp, op_frc, op_m4x4, op_m4x3, op_m3x4, op_m3x3, op_m3x2, op_call, op_callnz, op_loop, op_ret, op_endloop, op_label, op_dcl, op_pow, op_crs,
    op_sgn, op_abs, op_nrm, op_sincos, op_rep, op_endrep, op_if, op_ifc, op_else, op_endif, op_break, op_breakc, op_mova, op_defb, op_defi,
    op_texcoord = 64, op_texkill, op_tex, op_texbem, op_texbeml, op_texreg2ar, op_texreg2gb, op_texm3x2pad, op_texm3x2tex, op_texm3x3pad, op_texm3x3tex,
    op_texm3x3spec = 76, op_texm3x3vspec, op_expp, op_logp, op_cnd, op_def, op_texreg2rgb, op_texdp3tex, op_texm3x2depth, op_texdp3, op_texm3x3,
    op_texdepth, op_cmp, op_bem, op_dp2add, op_dsx, op_dsy, op_texldd, op_setp, op_texldl, op_breakp,
    op_phase = 0xFFFD, op_comment = 0xFFFE, op_end = 0xFFFF
};

enum DestModifier : uint8_t { mod_saturate = 1, mod_partial_precision = 2, mod_centroid = 4 };
// D3DSPSM_*
enum class SrcModifier : uint8_t { none, negate, bias, bias_negate, sign, sign_negate, complement, x2, x2_negate, dz, dw, abs, abs_negate, not_ };

struct DstParam {
    RegType type = RegType::temp;
    uint16_t index = 0;
    uint8_t mask = 0xF;          // bit 0 = x ... bit 3 = w
    uint8_t modifiers = 0;       // DestModifier bits
    int8_t shift = 0;            // result scale 2^shift (ps 1.x: _x2 = 1, _x4 = 2, _d2 = -1 ...)
    bool relative = false;
};

struct SrcParam {
    RegType type = RegType::temp;
    uint16_t index = 0;
    uint8_t swizzle[4] = {0, 1, 2, 3};   // 0 = x ... 3 = w
    SrcModifier modifier = SrcModifier::none;
    bool relative = false;
    // Relative addressing (c[a0.x + n], c[aL + n]) in models 2 and 3: the address register and its component. Model 1 always uses a0.x.
    bool relative_loop = false;
    uint8_t relative_component = 0;
};

struct Instruction {
    uint16_t opcode = op_nop;
    uint8_t control = 0;            // bits 16..23 of the opcode token (comparison for ifc/setp, texld variant)
    bool coissue = false;           // pixel shaders 1.x: "+" instruction paired with the previous one
    bool predicated = false;
    SrcParam predicate;             // the predicate register (p0 with swizzle and optional negation) when predicated
    bool has_dst = false;
    DstParam dst;
    SrcParam src[4];
    uint8_t src_count = 0;
    float immediate[4] = {0, 0, 0, 0};   // def
    uint32_t dcl_usage = 0, dcl_index = 0, dcl_texture_type = 0;   // dcl: usage / usage index / sampler texture type (2 = 2D, 3 = cube, 4 = volume)
};

struct Program {
    Stage stage = Stage::vertex;
    uint8_t major = 0, minor = 0;
    std::vector<Instruction> code;
    uint32_t constant_registers_read = 0;   // highest float constant register read plus one (constant buffer size the shader needs)
    bool uses_relative_constants = false;
};

// Decodes a complete shader (version token to END token). Returns false and sets `error` on anything it does not understand.
bool decode(std::span<const uint32_t> tokens, Program& out, std::string& error);
// Number of tokens of a shader whose size is not known (D3D9 gives only a pointer): up to and including the end token; 0 when it is not a shader.
size_t token_count(const uint32_t* tokens);

// Text in the style of Microsoft's disassembler ("mul r0, t0, v0", "add r0.xyz, r0, r0", "+ mov r0.w, r0.w", "dcl_position v0").
std::string disassemble(const Program&);

const char* opcode_name(uint16_t opcode);

}  // namespace nue::shader
