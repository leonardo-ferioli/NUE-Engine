#include "nue/shader/d3d9_shader.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nue::shader {
namespace {

struct OpInfo { const char* name; int8_t dst; int8_t src; };   // operand counts for models below 2.0 (src < 0: depends on version)

const OpInfo* info_for(uint16_t opcode) {
    static const OpInfo table[] = {
        {"nop", 0, 0}, {"mov", 1, 1}, {"add", 1, 2}, {"sub", 1, 2}, {"mad", 1, 3}, {"mul", 1, 2}, {"rcp", 1, 1}, {"rsq", 1, 1}, {"dp3", 1, 2}, {"dp4", 1, 2},
        {"min", 1, 2}, {"max", 1, 2}, {"slt", 1, 2}, {"sge", 1, 2}, {"exp", 1, 1}, {"log", 1, 1}, {"lit", 1, 1}, {"dst", 1, 2}, {"lrp", 1, 3}, {"frc", 1, 1},
        {"m4x4", 1, 2}, {"m4x3", 1, 2}, {"m3x4", 1, 2}, {"m3x3", 1, 2}, {"m3x2", 1, 2}, {"call", 0, 1}, {"callnz", 0, 2}, {"loop", 0, 2}, {"ret", 0, 0},
        {"endloop", 0, 0}, {"label", 0, 1}, {"dcl", 1, 0}, {"pow", 1, 2}, {"crs", 1, 2}, {"sgn", 1, 3}, {"abs", 1, 1}, {"nrm", 1, 1}, {"sincos", 1, 3},
        {"rep", 0, 1}, {"endrep", 0, 0}, {"if", 0, 1}, {"ifc", 0, 2}, {"else", 0, 0}, {"endif", 0, 0}, {"break", 0, 0}, {"breakc", 0, 2}, {"mova", 1, 1},
        {"defb", 1, 0}, {"defi", 1, 0}};
    return opcode < sizeof(table) / sizeof(table[0]) ? &table[opcode] : nullptr;
}

struct PixelOp { uint16_t opcode; const char* name; int8_t dst; int8_t src; };
// Pixel-shader texture and misc instructions (operand counts for 1.0-1.3; texcoord/tex take a source in 1.4).
const PixelOp pixel_ops[] = {
    {op_texcoord, "texcoord", 1, 0}, {op_texkill, "texkill", 1, 0}, {op_tex, "tex", 1, 0}, {op_texbem, "texbem", 1, 1}, {op_texbeml, "texbeml", 1, 1},
    {op_texreg2ar, "texreg2ar", 1, 1}, {op_texreg2gb, "texreg2gb", 1, 1}, {op_texm3x2pad, "texm3x2pad", 1, 1}, {op_texm3x2tex, "texm3x2tex", 1, 1},
    {op_texm3x3pad, "texm3x3pad", 1, 1}, {op_texm3x3tex, "texm3x3tex", 1, 1}, {op_texm3x3spec, "texm3x3spec", 1, 2}, {op_texm3x3vspec, "texm3x3vspec", 1, 1},
    {op_expp, "expp", 1, 1}, {op_logp, "logp", 1, 1}, {op_cnd, "cnd", 1, 3}, {op_def, "def", 1, 0}, {op_texreg2rgb, "texreg2rgb", 1, 1},
    {op_texdp3tex, "texdp3tex", 1, 1}, {op_texm3x2depth, "texm3x2depth", 1, 1}, {op_texdp3, "texdp3", 1, 1}, {op_texm3x3, "texm3x3", 1, 1},
    {op_texdepth, "texdepth", 1, 0}, {op_cmp, "cmp", 1, 3}, {op_bem, "bem", 1, 2}, {op_dp2add, "dp2add", 1, 3}, {op_dsx, "dsx", 1, 1}, {op_dsy, "dsy", 1, 1},
    {op_texldd, "texldd", 1, 4}, {op_setp, "setp", 1, 2}, {op_texldl, "texldl", 1, 2}, {op_breakp, "breakp", 0, 1}};

const PixelOp* pixel_info(uint16_t opcode) {
    for (const auto& p : pixel_ops) if (p.opcode == opcode) return &p;
    return nullptr;
}

RegType type_of(uint32_t token) { return RegType(((token >> 28) & 7) | ((token >> 8) & 0x18)); }
uint16_t index_of(uint32_t token) { return uint16_t(token & 0x7FF); }

DstParam decode_dst(uint32_t token) {
    DstParam d;
    d.type = type_of(token);
    d.index = index_of(token);
    d.relative = (token & 0x2000) != 0;
    d.mask = uint8_t((token >> 16) & 0xF);
    d.modifiers = uint8_t((token >> 20) & 0xF);
    int shift = int((token >> 24) & 0xF);
    d.shift = int8_t(shift > 7 ? shift - 16 : shift);
    return d;
}

SrcParam decode_src(uint32_t token) {
    SrcParam s;
    s.type = type_of(token);
    s.index = index_of(token);
    s.relative = (token & 0x2000) != 0;
    for (int i = 0; i < 4; ++i) s.swizzle[i] = uint8_t((token >> (16 + 2 * i)) & 3);
    s.modifier = SrcModifier((token >> 24) & 0xF);
    return s;
}

float as_float(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

}  // namespace

const char* opcode_name(uint16_t opcode) {
    if (opcode == op_phase) return "phase";
    if (opcode == op_comment) return "comment";
    if (opcode == op_end) return "end";
    if (const PixelOp* p = pixel_info(opcode)) return p->name;
    if (const OpInfo* i = info_for(opcode)) return i->name;
    return "?";
}

size_t token_count(const uint32_t* tokens) {
    const uint32_t version = tokens[0], kind = version >> 16;
    if (kind != 0xFFFE && kind != 0xFFFF) return 0;
    const uint32_t major = (version >> 8) & 0xFF, minor = version & 0xFF;
    const bool has_length = major >= 2, pixel = kind == 0xFFFF;
    size_t pos = 1;
    while (pos < (1u << 20)) {
        const uint32_t token = tokens[pos];
        const uint16_t opcode = uint16_t(token & 0xFFFF);
        if (opcode == op_end) return pos + 1;
        if (opcode == op_comment) { pos += 1 + ((token >> 16) & 0x7FFF); continue; }
        size_t length;
        if (opcode == op_phase) length = 0;
        else if (opcode == op_def || opcode == op_defi) length = 5;
        else if (opcode == op_dcl || opcode == op_defb) length = 2;
        else if (has_length) length = (token >> 24) & 0xF;
        else {
            const PixelOp* p = pixel_info(opcode);
            const OpInfo* i = info_for(opcode);
            if (p) length = size_t(p->dst + p->src);
            else if (i) length = size_t(i->dst + i->src);
            else return 0;
            if (pixel && major == 1 && minor == 4 && (opcode == op_texcoord || opcode == op_tex)) length = 2;
        }
        pos += 1 + length;
    }
    return 0;
}

bool decode(std::span<const uint32_t> tokens, Program& out, std::string& error) {
    out = Program{};
    if (tokens.size() < 2) { error = "too short"; return false; }
    const uint32_t version = tokens[0];
    const uint32_t kind = version >> 16;
    if (kind != 0xFFFE && kind != 0xFFFF) { error = "no version token"; return false; }
    out.stage = kind == 0xFFFE ? Stage::vertex : Stage::pixel;
    out.major = uint8_t((version >> 8) & 0xFF);
    out.minor = uint8_t(version & 0xFF);
    if (out.major < 1 || out.major > 3) { error = "unsupported shader model"; return false; }
    const bool has_length = out.major >= 2;
    size_t pos = 1;
    while (pos < tokens.size()) {
        const uint32_t token = tokens[pos];
        const uint16_t opcode = uint16_t(token & 0xFFFF);
        if (opcode == op_end) return true;
        if (opcode == op_comment) { pos += 1 + ((token >> 16) & 0x7FFF); continue; }
        Instruction in;
        in.opcode = opcode;
        in.control = uint8_t((token >> 16) & 0xFF);
        in.coissue = !has_length && (token & 0x40000000) != 0;
        in.predicated = has_length && (token & 0x10000000) != 0;
        size_t length;
        int dst_count, src_count;
        if (opcode == op_phase) { length = 0; dst_count = src_count = 0; }
        else if (opcode == op_def) { length = 5; dst_count = 1; src_count = 0; }
        else if (opcode == op_dcl) { length = 2; dst_count = 1; src_count = 0; }
        else if (opcode == op_defi) { length = 5; dst_count = 1; src_count = 0; }
        else if (opcode == op_defb) { length = 2; dst_count = 1; src_count = 0; }
        else if (has_length) {
            length = (token >> 24) & 0xF;
            if (!info_for(opcode) && !pixel_info(opcode)) { error = "unknown opcode " + std::to_string(opcode); return false; }
            const OpInfo* i = info_for(opcode);
            const PixelOp* p = pixel_info(opcode);
            dst_count = i ? i->dst : p->dst;
            src_count = int(length) - dst_count - (in.predicated ? 1 : 0);
        } else {
            const PixelOp* p = pixel_info(opcode);
            const OpInfo* i = info_for(opcode);
            if (p) { dst_count = p->dst; src_count = p->src; }
            else if (i) { dst_count = i->dst; src_count = i->src; }
            else { error = "unknown opcode " + std::to_string(opcode); return false; }
            // Pixel shader 1.4: texld and texcrd take a source register (the coordinates); texcoord is spelled texcrd there.
            if (out.stage == Stage::pixel && out.major == 1 && out.minor == 4 && (opcode == op_texcoord || opcode == op_tex)) src_count = 1;
            length = size_t(dst_count + src_count);
        }
        const size_t advance = 1 + length;
        if (pos + advance > tokens.size()) { error = "truncated instruction"; return false; }
        size_t at = pos + 1;
        if (opcode == op_dcl) {
            const uint32_t usage = tokens[at++];
            in.dcl_usage = usage & 0x1F;
            in.dcl_index = (usage >> 16) & 0xF;
            in.dcl_texture_type = (usage >> 27) & 0xF;
            in.has_dst = true;
            in.dst = decode_dst(tokens[at++]);
        } else if (opcode == op_def || opcode == op_defi) {
            in.has_dst = true;
            in.dst = decode_dst(tokens[at++]);
            for (int k = 0; k < 4; ++k) in.immediate[k] = opcode == op_def ? as_float(tokens[at++]) : float(int32_t(tokens[at++]));
        } else if (opcode == op_defb) {
            in.has_dst = true;
            in.dst = decode_dst(tokens[at++]);
            in.immediate[0] = float(tokens[at++] & 1);
        } else {
            size_t tail = pos + advance;
            if (in.predicated && tail > at) { in.predicate = decode_src(tokens[tail - 1]); --tail; }   // the predicate is the last token of the instruction
            if (dst_count) {
                in.has_dst = true; in.dst = decode_dst(tokens[at++]);
                if (in.dst.relative && has_length && at < tail) ++at;   // the address register of a relative destination (not interpreted)
            }
            // Sources are read until the tokens run out: in models 2 and 3 a relative source is followed by the token of its address register.
            int k = 0;
            while (at < tail && k < 4) {
                SrcParam s = decode_src(tokens[at++]);
                if (s.relative && has_length && at < tail + (in.predicated ? 0 : 0) && at < pos + advance) {
                    const uint32_t rel = tokens[at++];
                    s.relative_loop = type_of(rel) == RegType::loop;
                    s.relative_component = uint8_t((rel >> 16) & 3);
                }
                in.src[k++] = s;
            }
            in.src_count = uint8_t(k);
            (void)src_count;
        }
        for (int k = 0; k < in.src_count; ++k) {
            const SrcParam& s = in.src[k];
            if (s.type == RegType::const_float) {
                unsigned rows = 1;
                if (k == 1 && (opcode == op_m4x4 || opcode == op_m4x3)) rows = 4;
                else if (k == 1 && (opcode == op_m3x4 || opcode == op_m3x3)) rows = 3;
                else if (k == 1 && opcode == op_m3x2) rows = 2;
                out.constant_registers_read = std::max<uint32_t>(out.constant_registers_read, s.index + rows);
            }
            if (s.relative) out.uses_relative_constants = true;
        }
        out.code.push_back(in);
        pos += advance;
    }
    error = "no END token";
    return false;
}

namespace {
std::string reg_name(RegType type, uint16_t index, Stage stage) {
    char buf[32];
    switch (type) {
    case RegType::temp: std::snprintf(buf, sizeof buf, "r%u", index); break;
    case RegType::input: std::snprintf(buf, sizeof buf, "v%u", index); break;
    case RegType::const_float: std::snprintf(buf, sizeof buf, "c%u", index); break;
    case RegType::addr_texture:
        if (stage == Stage::vertex) std::snprintf(buf, sizeof buf, "a%u", index); else std::snprintf(buf, sizeof buf, "t%u", index);
        break;
    case RegType::rast_out: std::snprintf(buf, sizeof buf, "%s", index == 0 ? "oPos" : index == 1 ? "oFog" : "oPts"); break;
    case RegType::attr_out: std::snprintf(buf, sizeof buf, "oD%u", index); break;
    case RegType::texcoord_out: std::snprintf(buf, sizeof buf, "oT%u", index); break;
    case RegType::const_int: std::snprintf(buf, sizeof buf, "i%u", index); break;
    case RegType::color_out: std::snprintf(buf, sizeof buf, "oC%u", index); break;
    case RegType::depth_out: std::snprintf(buf, sizeof buf, "oDepth"); break;
    case RegType::sampler: std::snprintf(buf, sizeof buf, "s%u", index); break;
    case RegType::const_bool: std::snprintf(buf, sizeof buf, "b%u", index); break;
    case RegType::loop: std::snprintf(buf, sizeof buf, "aL"); break;
    case RegType::misc: std::snprintf(buf, sizeof buf, "%s", index == 0 ? "vPos" : "vFace"); break;
    case RegType::predicate: std::snprintf(buf, sizeof buf, "p%u", index); break;
    default: std::snprintf(buf, sizeof buf, "?%u_%u", unsigned(type), index); break;
    }
    return buf;
}

const char* usage_name(uint32_t usage) {
    static const char* const names[] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent", "binormal", "tessfactor", "positiont",
                                        "color", "fog", "depth", "sample"};
    return usage < 14 ? names[usage] : "?";
}

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", double(v));
    return buf;
}

std::string src_text(const SrcParam& s, Stage stage) {
    std::string name = reg_name(s.type, s.index, stage);
    if (s.relative) name += "[a0.x]";
    std::string swz;
    const char* letters = "xyzw";
    const bool identity = s.swizzle[0] == 0 && s.swizzle[1] == 1 && s.swizzle[2] == 2 && s.swizzle[3] == 3;
    const bool replicate = s.swizzle[0] == s.swizzle[1] && s.swizzle[1] == s.swizzle[2] && s.swizzle[2] == s.swizzle[3];
    if (replicate) swz = std::string(".") + letters[s.swizzle[0]];
    else if (!identity) { swz = "."; for (int i = 0; i < 4; ++i) swz += letters[s.swizzle[i]]; }
    name += swz;
    switch (s.modifier) {
    case SrcModifier::none: return name;
    case SrcModifier::negate: return "-" + name;
    case SrcModifier::bias: return name + "_bias";
    case SrcModifier::bias_negate: return "-" + name + "_bias";
    case SrcModifier::sign: return name + "_bx2";
    case SrcModifier::sign_negate: return "-" + name + "_bx2";
    case SrcModifier::complement: return "1-" + name;
    case SrcModifier::x2: return name + "_x2";
    case SrcModifier::x2_negate: return "-" + name + "_x2";
    case SrcModifier::dz: return name + "_dz";
    case SrcModifier::dw: return name + "_dw";
    case SrcModifier::abs: return name + "_abs";
    case SrcModifier::abs_negate: return "-" + name + "_abs";
    case SrcModifier::not_: return "!" + name;
    }
    return name;
}
}  // namespace

std::string disassemble(const Program& p) {
    std::string text;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s_%u_%u\n", p.stage == Stage::vertex ? "vs" : "ps", p.major, p.minor);
    text += buf;
    for (const Instruction& in : p.code) {
        std::string line;
        if (in.coissue) line += "+ ";
        if (in.opcode == op_phase) { text += "phase\n"; continue; }
        if (in.opcode == op_dcl) {
            if (in.dst.type == RegType::sampler) {
                static const char* const types[] = {"?", "?", "2d", "cube", "volume"};
                line += std::string("dcl_") + (in.dcl_texture_type < 5 ? types[in.dcl_texture_type] : "?") + " " + reg_name(in.dst.type, in.dst.index, p.stage);
            } else if (in.dst.type == RegType::addr_texture || (p.stage == Stage::pixel && in.dst.type == RegType::input)) {
                std::string usage = in.dcl_usage == 5 ? std::string("dcl_texcoord") : in.dcl_usage == 10 ? std::string("dcl_color") : std::string("dcl");
                if (usage != "dcl" && in.dcl_index) usage += std::to_string(in.dcl_index);
                line += usage + " " + reg_name(in.dst.type, in.dst.index, p.stage);
                if (in.dst.mask != 0xF) { line += "."; if (in.dst.mask & 1) line += "x"; if (in.dst.mask & 2) line += "y"; if (in.dst.mask & 4) line += "z"; if (in.dst.mask & 8) line += "w"; }
            } else {
                std::string usage = std::string("dcl_") + usage_name(in.dcl_usage);
                if (in.dcl_index) usage += std::to_string(in.dcl_index);
                line += usage + " " + reg_name(in.dst.type, in.dst.index, p.stage);
            }
            text += line + "\n";
            continue;
        }
        const char* name = opcode_name(in.opcode);
        if (p.stage == Stage::pixel && p.major == 1 && p.minor == 4 && in.opcode == op_texcoord) name = "texcrd";
        if (p.stage == Stage::pixel && ((p.major == 1 && p.minor == 4) || p.major >= 2) && in.opcode == op_tex) name = "texld";
        line += name;
        if (in.has_dst) {
            // shift and saturate are printed as instruction modifiers
            if (in.dst.shift == 1) line += "_x2"; else if (in.dst.shift == 2) line += "_x4"; else if (in.dst.shift == 3) line += "_x8";
            else if (in.dst.shift == -1) line += "_d2"; else if (in.dst.shift == -2) line += "_d4"; else if (in.dst.shift == -3) line += "_d8";
            if (in.dst.modifiers & mod_saturate) line += "_sat";
            if (in.dst.modifiers & mod_partial_precision) line += "_pp";
            if (in.dst.modifiers & mod_centroid) line += "_centroid";
            std::string dst = reg_name(in.dst.type, in.dst.index, p.stage);
            if (in.dst.mask != 0xF) { dst += "."; if (in.dst.mask & 1) dst += "x"; if (in.dst.mask & 2) dst += "y"; if (in.dst.mask & 4) dst += "z"; if (in.dst.mask & 8) dst += "w"; }
            line += " " + dst;
        }
        if (in.opcode == op_def) {
            for (int k = 0; k < 4; ++k) line += ", " + number(in.immediate[k]);
        }
        for (int k = 0; k < in.src_count; ++k) line += (in.has_dst || k ? ", " : " ") + src_text(in.src[k], p.stage);
        text += line + "\n";
    }
    return text;
}

}  // namespace nue::shader
