// PNG (read through stb_image, write with stored deflate blocks), file helpers, and the nue_tex_image_load / save entry points.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#include "container.hpp"
#include <cctype>
#include <cerrno>
#include <thread>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace nuetex {

namespace {
std::filesystem::path to_path(const std::string& u8) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, &w[0], n);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(u8);
#endif
}

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        const size_t k = std::min<size_t>(n, 5552);
        for (size_t i = 0; i < k; ++i) { a += p[i]; b += a; }
        a %= 65521; b %= 65521; p += k; n -= k;
    }
    return (b << 16) | a;
}
void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload) {
    be32(out, uint32_t(payload.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), payload.begin(), payload.end());
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc32(0, body.data(), body.size()));
}
}  // namespace

bool read_file(const char* utf8_path, std::vector<uint8_t>& out) {
    std::ifstream f(to_path(utf8_path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize(size_t(n));
    f.seekg(0);
    return n == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), n));
}

bool make_dirs(const std::string& utf8_path) {
    std::error_code ec;
    std::filesystem::create_directories(to_path(utf8_path), ec);
    return !ec;
}

bool write_file_atomic(const char* utf8_path, const void* data, size_t size) {
    const std::filesystem::path dst = to_path(utf8_path);
    std::filesystem::path tmp = dst;
    tmp += ".tmp" + std::to_string(uint64_t(reinterpret_cast<uintptr_t>(&tmp)) ^ uint64_t(std::hash<std::thread::id>()(std::this_thread::get_id())));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(static_cast<const char*>(data), std::streamsize(size));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {                                         // Windows cannot rename over an existing file in some cases: replace explicitly
        std::filesystem::remove(dst, ec);
        ec.clear();
        std::filesystem::rename(tmp, dst, ec);
        if (ec) { std::filesystem::remove(tmp, ec); return false; }
    }
    return true;
}

bool remove_file(const char* utf8_path) { std::error_code ec; return std::filesystem::remove(to_path(utf8_path), ec); }

nue_result png_load(const uint8_t* data, size_t size, uint32_t flags, Image** out) {
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, int(size), &w, &h, &comp)) return NUE_ERR_CORRUPT;
    const bool is16 = stbi_is_16_bit_from_memory(data, int(size)) != 0;
    const bool srgb = (flags & NUE_TEX_FILE_ASSUME_SRGB) != 0;
    Fmt f;
    int want = comp;
    if (is16) { f = comp == 1 ? NUE_TEX_L16_UNORM : NUE_TEX_RGBA16_UNORM; want = comp == 1 ? 1 : 4; }
    else if (comp == 1) f = NUE_TEX_L8_UNORM;
    else if (comp == 2) f = NUE_TEX_LA8_UNORM;
    else if (comp == 3) f = srgb ? NUE_TEX_RGB8_SRGB : NUE_TEX_RGB8_UNORM;
    else f = srgb ? NUE_TEX_RGBA8_SRGB : NUE_TEX_RGBA8_UNORM;
    nue_tex_image_desc d{};
    d.struct_size = sizeof d; d.format = f; d.width = uint32_t(w); d.height = uint32_t(h); d.mip_levels = 1;
    Image* im = nullptr;
    nue_result r = image_create(d, &im);
    if (r < 0) return r;
    std::unique_ptr<Image> g(im);
    if (is16) {
        stbi_us* px = stbi_load_16_from_memory(data, int(size), &w, &h, &comp, want);
        if (!px) return NUE_ERR_CORRUPT;
        std::memcpy(im->data.data(), px, im->data.size());
        stbi_image_free(px);
    } else {
        stbi_uc* px = stbi_load_from_memory(data, int(size), &w, &h, &comp, want);
        if (!px) return NUE_ERR_CORRUPT;
        std::memcpy(im->data.data(), px, im->data.size());
        stbi_image_free(px);
    }
    *out = g.release();
    return NUE_OK;
}

nue_result png_save(const Image& im, std::vector<uint8_t>& out) {
    const uint32_t w = im.desc.width, h = im.desc.height;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    nue_result r = nue_tex_decode_level(im.desc.format, w, h, im.level_ptr(0, 0, 0), size_t(im.level_size(0, 0, 0)), NUE_TEX_RGBA8_UNORM, rgba.data(), rgba.size(), nullptr);
    if (r < 0) return r;
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (uint32_t y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4); }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    be32(z, adler32(raw.data(), raw.size()));
    out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return NUE_OK;
}

}  // namespace nuetex

using namespace nuetex;

extern "C" {

nue_tex_file_kind NUE_CALL nue_tex_detect_file_kind(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (size >= 4 && std::memcmp(p, "DDS ", 4) == 0) return NUE_TEX_FILE_DDS;
    if (size >= 12 && p[0] == 0xAB && std::memcmp(p + 1, "KTX 20", 6) == 0) return NUE_TEX_FILE_KTX2;
    if (size >= 8 && p[0] == 0x89 && std::memcmp(p + 1, "PNG", 3) == 0) return NUE_TEX_FILE_PNG;
    return NUE_TEX_FILE_AUTO;
}

nue_result NUE_CALL nue_tex_image_load_memory(const void* data, size_t size, nue_tex_file_kind kind, uint32_t flags, nue_tex_image* out) {
    if (!data || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (kind == NUE_TEX_FILE_AUTO) kind = nue_tex_detect_file_kind(data, size);
    Image* im = nullptr;
    nue_result r;
    switch (kind) {
    case NUE_TEX_FILE_DDS: r = dds_load(static_cast<const uint8_t*>(data), size, flags, &im); break;
    case NUE_TEX_FILE_KTX2: r = ktx2_load(static_cast<const uint8_t*>(data), size, flags, &im, nullptr); break;
    case NUE_TEX_FILE_PNG: r = png_load(static_cast<const uint8_t*>(data), size, flags, &im); break;
    default: r = NUE_ERR_UNSUPPORTED; break;
    }
    if (r >= 0) *out = handle(im);
    return r;
}

nue_result NUE_CALL nue_tex_image_load_file(const char* path, nue_tex_file_kind kind, uint32_t flags, nue_tex_image* out) {
    if (!path || !out) return NUE_ERR_INVALID_ARGUMENT;
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) return NUE_ERR_NOT_FOUND;
    return nue_tex_image_load_memory(bytes.data(), bytes.size(), kind, flags, out);
}

nue_result NUE_CALL nue_tex_image_save_memory(nue_tex_image image, nue_tex_file_kind kind, uint32_t flags, void** out_data, size_t* out_size) {
    if (!image || !out_data || !out_size) return NUE_ERR_INVALID_ARGUMENT;
    std::vector<uint8_t> bytes;
    nue_result r;
    switch (kind) {
    case NUE_TEX_FILE_DDS: r = dds_save(*img(image), flags, bytes); break;
    case NUE_TEX_FILE_KTX2: r = ktx2_save(*img(image), nullptr, bytes); break;
    case NUE_TEX_FILE_PNG: r = png_save(*img(image), bytes); break;
    default: return NUE_ERR_INVALID_ARGUMENT;
    }
    if (r < 0) return r;
    void* p = std::malloc(bytes.size() ? bytes.size() : 1);
    if (!p) return NUE_ERR_OUT_OF_MEMORY;
    std::memcpy(p, bytes.data(), bytes.size());
    *out_data = p; *out_size = bytes.size();
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_image_save_file(nue_tex_image image, const char* path, nue_tex_file_kind kind, uint32_t flags) {
    if (!path) return NUE_ERR_INVALID_ARGUMENT;
    if (kind == NUE_TEX_FILE_AUTO) {
        const std::string p = path;
        auto ends = [&](const char* e) { const size_t n = std::strlen(e); if (p.size() < n) return false; for (size_t i = 0; i < n; ++i) if (std::tolower((unsigned char)p[p.size() - n + i]) != e[i]) return false; return true; };
        kind = ends(".dds") ? NUE_TEX_FILE_DDS : ends(".ktx2") ? NUE_TEX_FILE_KTX2 : ends(".png") ? NUE_TEX_FILE_PNG : NUE_TEX_FILE_AUTO;
    }
    void* data = nullptr; size_t size = 0;
    nue_result r = nue_tex_image_save_memory(image, kind, flags, &data, &size);
    if (r < 0) return r;
    const bool ok = write_file_atomic(path, data, size);
    std::free(data);
    return ok ? NUE_OK : NUE_ERR_IO;
}

}  // extern "C"
