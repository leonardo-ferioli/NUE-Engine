// A small SPIR-V module writer: just what the Direct3D 9 shader lowering needs (logical addressing, GLSL.std.450, one entry point).
// Written from the SPIR-V specification; only the numeric constants of the Khronos headers are used.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nue::shader {

namespace spv {
enum Op : uint16_t {
    Name = 5, MemberName = 6, ExtInst = 12, ExtInstImport = 11, MemoryModel = 14, EntryPoint = 15, ExecutionMode = 16, Capability = 17,
    TypeVoid = 19, TypeBool = 20, TypeInt = 21, TypeFloat = 22, TypeVector = 23, TypeImage = 25, TypeSampler = 26, TypeSampledImage = 27, TypeArray = 28,
    TypeStruct = 30, TypePointer = 32, TypeFunction = 33,
    ConstantTrue = 41, ConstantFalse = 42, Constant = 43, ConstantComposite = 44,
    Function = 54, FunctionEnd = 56, Variable = 59, Load = 61, Store = 62, AccessChain = 65,
    Decorate = 71, MemberDecorate = 72,
    VectorShuffle = 79, CompositeConstruct = 80, CompositeExtract = 81,
    SampledImage = 86, ImageSampleImplicitLod = 87, ImageSampleProjImplicitLod = 91,
    ConvertFToS = 110, ConvertSToF = 111,
    IAdd = 128, FNegate = 127, FAdd = 129, FSub = 131, FMul = 133, FDiv = 136, Dot = 148,
    LogicalOr = 166, LogicalAnd = 167, LogicalNot = 168, Select = 169, FOrdEqual = 180, FOrdNotEqual = 182, FOrdLessThan = 184, FOrdGreaterThan = 186, FOrdLessThanEqual = 188, FOrdGreaterThanEqual = 190, Any = 154,
    SelectionMerge = 247, Label = 248, Branch = 249, BranchConditional = 250, Kill = 252, Return = 253
};
enum StorageClass : uint32_t { UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Function_ = 7 };
enum Decoration : uint32_t { Block = 2, ArrayStride = 6, BuiltIn = 11, Location = 30, Binding = 33, DescriptorSet = 34, Offset = 35 };
enum BuiltIn : uint32_t { Position = 0, PointSize = 1 };
enum ExecutionModel : uint32_t { Vertex = 0, Fragment = 4 };
enum GlslStd450 : uint32_t { FAbs = 4, Floor = 8, Fract = 10, Pow = 26, Exp2 = 29, Log2 = 30, Sqrt = 31, InverseSqrt = 32, FMin = 37, FMax = 40, FClamp = 43, SClamp = 45, FMix = 46, Cross = 68, Normalize = 69 };
}  // namespace spv

class SpirvBuilder {
public:
    uint32_t new_id() { return next_id_++; }

    // ---- types and constants (de-duplicated)
    uint32_t type_void() { return cached("void", [&](uint32_t id) { inst(types_, spv::TypeVoid, {id}); }); }
    uint32_t type_bool() { return cached("bool", [&](uint32_t id) { inst(types_, spv::TypeBool, {id}); }); }
    uint32_t type_float() { return cached("f32", [&](uint32_t id) { inst(types_, spv::TypeFloat, {id, 32}); }); }
    uint32_t type_int() { return cached("i32", [&](uint32_t id) { inst(types_, spv::TypeInt, {id, 32, 1}); }); }
    uint32_t type_vec(uint32_t component, uint32_t count) {
        return cached("vec" + std::to_string(component) + "x" + std::to_string(count), [&](uint32_t id) { inst(types_, spv::TypeVector, {id, component, count}); });
    }
    uint32_t type_vec4() { return type_vec(type_float(), 4); }
    uint32_t type_ptr(uint32_t storage, uint32_t pointee) {
        return cached("ptr" + std::to_string(storage) + "_" + std::to_string(pointee), [&](uint32_t id) { inst(types_, spv::TypePointer, {id, storage, pointee}); });
    }
    uint32_t type_array(uint32_t element, uint32_t length) {
        const uint32_t length_id = const_uint(length);
        return cached("arr" + std::to_string(element) + "_" + std::to_string(length), [&](uint32_t id) { inst(types_, spv::TypeArray, {id, element, length_id}); });
    }
    uint32_t type_struct(const std::vector<uint32_t>& members, const std::string& key) {
        return cached("struct_" + key, [&](uint32_t id) { std::vector<uint32_t> w{id}; w.insert(w.end(), members.begin(), members.end()); inst(types_, spv::TypeStruct, w); });
    }
    // Sampled float image: dim 1 = 2D, 2 = 3D, 3 = Cube.
    uint32_t type_image(uint32_t dim) {
        return cached("img" + std::to_string(dim), [&](uint32_t id) { inst(types_, spv::TypeImage, {id, type_float(), dim, 0, 0, 0, 1, 0}); });
    }
    uint32_t type_sampler() { return cached("sampler", [&](uint32_t id) { inst(types_, spv::TypeSampler, {id}); }); }
    uint32_t type_sampled_image(uint32_t image) {
        return cached("simg" + std::to_string(image), [&](uint32_t id) { inst(types_, spv::TypeSampledImage, {id, image}); });
    }
    uint32_t type_function(uint32_t result) {
        return cached("fn" + std::to_string(result), [&](uint32_t id) { inst(types_, spv::TypeFunction, {id, result}); });
    }
    uint32_t const_float(float value) {
        uint32_t bits; std::memcpy(&bits, &value, 4);
        return cached("cf" + std::to_string(bits), [&](uint32_t id) { inst(types_, spv::Constant, {type_float(), id, bits}); });
    }
    uint32_t const_int(int32_t value) { return const_uint(uint32_t(value)); }
    uint32_t const_uint(uint32_t value) {
        return cached("cu" + std::to_string(value), [&](uint32_t id) { inst(types_, spv::Constant, {type_int(), id, value}); });
    }
    uint32_t const_vec4(float x, float y, float z, float w) {
        const uint32_t a = const_float(x), b = const_float(y), c = const_float(z), d = const_float(w);
        return cached("cv4_" + std::to_string(a) + "_" + std::to_string(b) + "_" + std::to_string(c) + "_" + std::to_string(d),
                      [&](uint32_t id) { inst(types_, spv::ConstantComposite, {type_vec4(), id, a, b, c, d}); });
    }

    // ---- global declarations
    uint32_t global_variable(uint32_t type, uint32_t storage, const std::string& name) {
        const uint32_t id = new_id();
        inst(globals_, spv::Variable, {type_ptr(storage, type), id, storage});
        if (!name.empty()) set_name(id, name);
        return id;
    }
    void set_name(uint32_t id, const std::string& name) { std::vector<uint32_t> w{id}; pack_string(w, name); inst(names_, spv::Name, w); }
    void set_member_name(uint32_t type, uint32_t member, const std::string& name) { std::vector<uint32_t> w{type, member}; pack_string(w, name); inst(names_, spv::MemberName, w); }
    void decorate(uint32_t id, uint32_t decoration, std::initializer_list<uint32_t> operands = {}) {
        std::vector<uint32_t> w{id, decoration}; w.insert(w.end(), operands.begin(), operands.end()); inst(annotations_, spv::Decorate, w);
    }
    void decorate_member(uint32_t type, uint32_t member, uint32_t decoration, std::initializer_list<uint32_t> operands = {}) {
        std::vector<uint32_t> w{type, member, decoration}; w.insert(w.end(), operands.begin(), operands.end()); inst(annotations_, spv::MemberDecorate, w);
    }
    uint32_t import_glsl_std() {
        if (!glsl_) { glsl_ = new_id(); std::vector<uint32_t> w{glsl_}; pack_string(w, "GLSL.std.450"); inst(imports_, spv::ExtInstImport, w); }
        return glsl_;
    }

    // ---- function body (a single function, blocks written in order)
    uint32_t begin_block() { const uint32_t id = new_id(); label(id); return id; }
    void label(uint32_t id) { inst(body_, spv::Label, {id}); }
    // Variables of Function storage must come first in the entry block: they are collected separately.
    uint32_t local_variable(uint32_t type, const std::string& name = "") {
        const uint32_t id = new_id();
        inst(locals_, spv::Variable, {type_ptr(spv::Function_, type), id, spv::Function_});
        if (!name.empty()) set_name(id, name);
        return id;
    }
    uint32_t emit(spv::Op op, uint32_t result_type, std::initializer_list<uint32_t> operands) {
        const uint32_t id = new_id();
        std::vector<uint32_t> w{result_type, id}; w.insert(w.end(), operands.begin(), operands.end()); inst(body_, op, w);
        return id;
    }
    uint32_t emit_v(spv::Op op, uint32_t result_type, const std::vector<uint32_t>& operands) {
        const uint32_t id = new_id();
        std::vector<uint32_t> w{result_type, id}; w.insert(w.end(), operands.begin(), operands.end()); inst(body_, op, w);
        return id;
    }
    void emit_void(spv::Op op, std::initializer_list<uint32_t> operands) { inst(body_, op, std::vector<uint32_t>(operands)); }
    uint32_t ext(uint32_t result_type, uint32_t instruction, std::initializer_list<uint32_t> operands) {
        const uint32_t id = new_id();
        std::vector<uint32_t> w{result_type, id, import_glsl_std(), instruction}; w.insert(w.end(), operands.begin(), operands.end()); inst(body_, spv::ExtInst, w);
        return id;
    }
    uint32_t load(uint32_t type, uint32_t pointer) { return emit(spv::Load, type, {pointer}); }
    void store(uint32_t pointer, uint32_t value) { emit_void(spv::Store, {pointer, value}); }

    // ---- module assembly
    std::vector<uint32_t> finish(uint32_t execution_model, const std::string& entry_name, const std::vector<uint32_t>& interface_ids, bool origin_upper_left) {
        const uint32_t void_type = type_void();
        const uint32_t function_type = type_function(void_type);
        const uint32_t function_id = new_id();
        std::vector<uint32_t> out{0x07230203, 0x00010000, 0 /* generator */, 0 /* bound, patched */, 0};
        inst(out, spv::Capability, {1});   // Shader
        out.insert(out.end(), imports_.begin(), imports_.end());
        inst(out, spv::MemoryModel, {0, 1});   // Logical, GLSL450
        { std::vector<uint32_t> w{execution_model, function_id}; pack_string(w, entry_name); w.insert(w.end(), interface_ids.begin(), interface_ids.end()); inst(out, spv::EntryPoint, w); }
        if (origin_upper_left) inst(out, spv::ExecutionMode, {function_id, 7});
        out.insert(out.end(), names_.begin(), names_.end());
        out.insert(out.end(), annotations_.begin(), annotations_.end());
        out.insert(out.end(), types_.begin(), types_.end());
        out.insert(out.end(), globals_.begin(), globals_.end());
        inst(out, spv::Function, {void_type, function_id, 0, function_type});
        // entry block label first, then the locals, then the body without its own first label
        out.insert(out.end(), body_.begin(), body_.begin() + 2);   // OpLabel (2 words)
        out.insert(out.end(), locals_.begin(), locals_.end());
        out.insert(out.end(), body_.begin() + 2, body_.end());
        inst(out, spv::FunctionEnd, {});
        out[3] = next_id_;
        return out;
    }

private:
    static void inst(std::vector<uint32_t>& to, uint16_t op, const std::vector<uint32_t>& words) {
        to.push_back((uint32_t(words.size() + 1) << 16) | op);
        to.insert(to.end(), words.begin(), words.end());
    }
    static void pack_string(std::vector<uint32_t>& to, const std::string& text) {
        const size_t words = text.size() / 4 + 1;
        const size_t base = to.size();
        to.resize(base + words, 0);
        std::memcpy(reinterpret_cast<char*>(to.data() + base), text.data(), text.size());
    }
    template <class F> uint32_t cached(const std::string& key, F make) {
        auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const uint32_t id = new_id();
        cache_.emplace(key, id);
        make(id);
        return id;
    }

    uint32_t next_id_ = 1, glsl_ = 0;
    std::map<std::string, uint32_t> cache_;
    std::vector<uint32_t> imports_, names_, annotations_, types_, globals_, locals_, body_;
};

}  // namespace nue::shader
