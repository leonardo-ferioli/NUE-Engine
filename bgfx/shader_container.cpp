#include "shader_container.hpp"
#include "nue/shader/cross_compile.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nue::gpu {
bool build_container_uncached(const shader::TranslateResult&, ShaderTarget, ContainerResult&);
namespace {

void put8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

constexpr uint8_t kUniformFragmentBit = 0x10, kUniformSamplerBit = 0x20;
constexpr uint8_t kVec4 = 2;   // bgfx::UniformType::Vec4 (Sampler = 0, End = 1, Vec4 = 2)

struct Uniform { std::string name; uint8_t type; uint8_t num; uint16_t reg_index, reg_count; uint8_t tex_dimension; };

std::vector<uint8_t> assemble(bool fragment, const std::vector<Uniform>& uniforms, const uint8_t* code, size_t code_size, const std::vector<uint16_t>& attributes, uint16_t cbuffer_size) {
    std::vector<uint8_t> out;
    put8(out, fragment ? 'F' : 'V'); put8(out, 'S'); put8(out, 'H'); put8(out, 13);
    put32(out, 0); put32(out, 0);        // input / output hashes (the stages are linked by location)
    put32(out, 0); put32(out, 0);        // raw SRV / UAV bindings
    put16(out, uint16_t(uniforms.size()));
    for (const Uniform& u : uniforms) {
        put8(out, uint8_t(u.name.size()));
        out.insert(out.end(), u.name.begin(), u.name.end());
        put8(out, uint8_t(u.type | (fragment ? kUniformFragmentBit : 0)));
        put8(out, u.num);
        put16(out, u.reg_index);
        put16(out, u.reg_count);
        put8(out, 0);                    // texture component: float
        put8(out, u.tex_dimension);
        put16(out, 0);                   // texture format: unknown
    }
    put32(out, uint32_t(code_size));
    out.insert(out.end(), code, code + code_size);
    put8(out, 0);
    put8(out, uint8_t(attributes.size()));
    for (uint16_t a : attributes) put16(out, a);
    put16(out, cbuffer_size);
    return out;
}

uint8_t dimension_id(uint32_t dim) { return dim == 3 ? 4 : dim == 2 ? 6 : 2; }   // cube, 3D, 2D (bgfx TextureDimension ids)

#ifdef _WIN32
struct Blob { struct VT { void* qi; ULONG(__stdcall* add_ref)(Blob*); ULONG(__stdcall* release)(Blob*); void*(__stdcall* pointer)(Blob*); SIZE_T(__stdcall* size)(Blob*); }* vt; };
using D3DCompileFn = HRESULT(__stdcall*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, Blob**, Blob**);

D3DCompileFn d3d_compile() {
    static D3DCompileFn fn = [] {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        return m ? reinterpret_cast<D3DCompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    return fn;
}
#endif

double ms_since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

}  // namespace

uint16_t bgfx_attribute_id(uint32_t attribute) {
    static const uint16_t ids[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0018, 0x0019, 0x000e, 0x000f,
                                   0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x001a, 0x001b, 0x001c, 0x001d, 0x001e, 0x001f, 0x0020, 0x0021};
    return attribute < sizeof(ids) / sizeof(ids[0]) ? ids[attribute] : 0xFFFF;
}

namespace {
constexpr uint32_t kCacheVersion = 1;   // bump when the container layout or the cross-compilation changes
uint64_t cache_key(const shader::TranslateResult& t, ShaderTarget target) {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xFF; h *= 0x100000001b3ull; } };
    mix(kCacheVersion); mix(uint64_t(target)); mix(uint64_t(t.stage)); mix(t.constant_registers); mix(t.int_bool_registers); mix(t.sampler_mask);
    for (uint32_t s = 0; s < 16; ++s) mix(t.sampler_dim[s]);
    for (const auto& in : t.inputs) mix(in.attribute);
    mix(t.spirv.size());
    for (uint32_t w : t.spirv) { h ^= w; h *= 0x100000001b3ull; h ^= h >> 29; }
    return h;
}
}  // namespace

#ifdef _WIN32
// The HLSL compiler and SPIRV-Cross can need several hundred KB of stack. A host that calls us on a small stack (the recompiled game's own 1 MB guest
// stack, when this runs inside a d3d9.dll) must not be able to overflow it, so the compilation runs on a thread with a large one.
template <class F> void run_on_large_stack(F&& work) {
    struct Job { F* work; } job{&work};
    HANDLE thread = CreateThread(nullptr, 16u << 20, [](LPVOID p) -> DWORD { (*static_cast<Job*>(p)->work)(); return 0; }, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) { work(); return; }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
#else
template <class F> void run_on_large_stack(F&& work) { work(); }
#endif

bool build_container(const shader::TranslateResult& t, ShaderTarget target, ContainerResult& out, const std::string& cache_dir) {
    std::filesystem::path cache_file;
    if (target == ShaderTarget::dxbc && !cache_dir.empty()) {
        char name[40];
        std::snprintf(name, sizeof name, "%016llx.nsc", (unsigned long long)cache_key(t, target));
        cache_file = std::filesystem::path(cache_dir) / name;
        if (FILE* f = std::fopen(cache_file.string().c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 0) {
                out.bytes.resize(size_t(size));
                if (std::fread(out.bytes.data(), 1, size_t(size), f) == size_t(size)) { std::fclose(f); out.from_cache = true; return true; }
                out.bytes.clear();
            }
            std::fclose(f);
        }
    }
    bool built = false;
    run_on_large_stack([&] { built = build_container_uncached(t, target, out); });
    if (!built) return false;
    if (!cache_file.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        const std::filesystem::path tmp = cache_file.string() + ".tmp";
        if (FILE* f = std::fopen(tmp.string().c_str(), "wb")) {
            const bool ok = std::fwrite(out.bytes.data(), 1, out.bytes.size(), f) == out.bytes.size();
            std::fclose(f);
            if (ok) std::filesystem::rename(tmp, cache_file, ec); else std::filesystem::remove(tmp, ec);
        }
    }
    return true;
}

bool build_container_uncached(const shader::TranslateResult& t, ShaderTarget target, ContainerResult& out) {
    const bool fragment = t.stage == shader::Stage::pixel;
    const uint16_t count = uint16_t(shader::TranslateOptions::fixup_registers + t.constant_registers + t.int_bool_registers);
    std::vector<Uniform> uniforms;
    uniforms.push_back({fragment ? "u_ps" : "u_vs", kVec4, uint8_t(std::min<uint32_t>(count, 255)), 0, count, 0});
    for (uint32_t s = 0; s < 16; ++s) {
        if (!(t.sampler_mask & (1u << s))) continue;
        const uint32_t dim = t.sampler_dim[s];
        uniforms.push_back({"s_tex" + std::to_string(s), uint8_t(kUniformSamplerBit), 1, uint16_t(target == ShaderTarget::spirv ? 2 + s : s), 0, dimension_id(dim)});
    }
    std::vector<uint16_t> attributes;
    if (!fragment) for (const auto& in : t.inputs) attributes.push_back(bgfx_attribute_id(in.attribute));

    if (target == ShaderTarget::spirv) {
        std::vector<uint32_t> words = t.spirv;
        if (!fragment) {
            // bgfx's Vulkan backend binds vertex attributes by their position in the container's attribute list, not by the attribute number:
            // vertex input locations are renumbered 0..n-1 in the order of `t.inputs` (the order of `attributes`).
            std::map<uint32_t, uint32_t> compact;
            for (uint32_t i = 0; i < t.inputs.size(); ++i) compact[t.inputs[i].attribute] = i;
            std::set<uint32_t> input_variables;
            for (size_t i = 5; i < words.size();) {   // first pass: Input variables
                const uint32_t op = words[i] & 0xFFFF, len = words[i] >> 16;
                if (!len) break;
                if (op == 59 && len >= 4 && words[i + 3] == 1) input_variables.insert(words[i + 2]);   // OpVariable, StorageClass Input
                i += len;
            }
            for (size_t i = 5; i < words.size();) {   // second pass: their Location decorations
                const uint32_t op = words[i] & 0xFFFF, len = words[i] >> 16;
                if (!len) break;
                if (op == 71 && len >= 4 && words[i + 2] == 30 && input_variables.count(words[i + 1])) {   // OpDecorate <id> Location <n>
                    const auto it = compact.find(words[i + 3]);
                    if (it != compact.end()) words[i + 3] = it->second;
                }
                i += len;
            }
        }
        out.bytes = assemble(fragment, uniforms, reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4, attributes, uint16_t(count * 16));
        return true;
    }
    if (target == ShaderTarget::dxbc) {
#ifdef _WIN32
        std::string hlsl;
        auto t0 = std::chrono::steady_clock::now();
        shader::CrossOptions co;
        if (!shader::cross_compile(t.spirv, t.stage, shader::Target::hlsl50, co, hlsl, out.error)) return false;
        out.cross_ms = ms_since(t0);
        D3DCompileFn compile = d3d_compile();
        if (!compile) { out.error = "d3dcompiler_47.dll not found"; return false; }
        t0 = std::chrono::steady_clock::now();
        Blob* code = nullptr; Blob* errors = nullptr;
        const HRESULT hr = compile(hlsl.data(), hlsl.size(), "nue", nullptr, nullptr, "main", fragment ? "ps_5_0" : "vs_5_0", 0, 0, &code, &errors);
        out.compile_ms = ms_since(t0);
        if (FAILED(hr) || !code) {
            out.error = "D3DCompile failed: ";
            if (errors) out.error += std::string(static_cast<const char*>(errors->vt->pointer(errors)), errors->vt->size(errors));
            out.error += "\n" + hlsl;
            if (errors) errors->vt->release(errors);
            return false;
        }
        out.bytes = assemble(fragment, uniforms, static_cast<const uint8_t*>(code->vt->pointer(code)), code->vt->size(code), attributes, uint16_t(count * 16));
        code->vt->release(code);
        if (errors) errors->vt->release(errors);
        return true;
#else
        out.error = "DXBC needs Windows";
        return false;
#endif
    }
    out.error = "shader target not implemented";
    return false;
}

}  // namespace nue::gpu
