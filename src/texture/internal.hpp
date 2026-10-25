// Internal declarations shared by the texture module sources. Not installed.
#pragma once
#include "nue/texture.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace nuetex {

using Fmt = nue_tex_format;

inline const nue_tex_format_info* info(Fmt f) { return nue_tex_get_format_info(f); }
inline bool is_block(Fmt f) { const auto* i = info(f); return i && i->kind == NUE_TEX_KIND_BLOCK; }
inline bool is_uncompressed(Fmt f) { const auto* i = info(f); return i && i->kind == NUE_TEX_KIND_UNCOMPRESSED; }
inline bool is_astc(Fmt f) { return (f >= 160 && f <= 173) || (f >= 176 && f <= 189) || (f >= 192 && f <= 205); }
inline int astc_index(Fmt f) { return is_astc(f) ? int(f - 160) % 16 : -1; }       // 0..13
inline bool is_srgb(Fmt f) { const auto* i = info(f); return i && i->colorspace == NUE_TEX_COLORSPACE_SRGB; }
inline bool is_hdr(Fmt f) { const auto* i = info(f); return i && (i->flags & NUE_TEX_FINFO_HDR); }

// 8-bit RGBA / float RGBA intermediates.
inline bool is_rgba8(Fmt f) { return f == NUE_TEX_RGBA8_UNORM; }
inline bool is_rgba32f(Fmt f) { return f == NUE_TEX_RGBA32_FLOAT; }

void log(int level, const char* fmt, ...);

// Phase timers (nue_tex_profile).
enum ProfSlot { P_ALPHA, P_PLAN, P_SRCHASH, P_CACHE, P_DROP, P_CONVERT, P_DECODE, P_ENCODE, P_MIPS, P_LEVELHASH, P_STORE, P_LOOKUP, P_LOAD, P_HASHED_BYTES, P_COUNT };
extern std::atomic<uint64_t> g_prof[P_COUNT];
extern std::atomic<uint64_t> g_enc_ns[NUE_TEX_FORMAT_COUNT], g_enc_px[NUE_TEX_FORMAT_COUNT];
struct ProfScope {
    int slot; std::chrono::steady_clock::time_point t0;
    explicit ProfScope(int s) : slot(s), t0(std::chrono::steady_clock::now()) {}
    ~ProfScope() { g_prof[slot].fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed); }
};

// Pixel pack/unpack for uncompressed formats (pixel.cpp). dst/src RGBA8 or RGBA32F rows of `count` pixels.
nue_result unpack_row(Fmt f, const void* src, uint32_t count, bool to_float, void* dst);
nue_result pack_row(Fmt f, const void* src, uint32_t count, bool from_float, void* dst);

struct ChanInfo { uint8_t id, offset, bits; };      // KTX2 DFD style: id 0 R, 1 G, 2 B, 15 A
bool channel_layout(Fmt f, ChanInfo* out /*[4]*/, int* n, bool* is_float, bool* is_signed, int* type_size);

// Block codecs (codecs.cpp). Pixels are RGBA8 (u32 per pixel, R in the low byte) or RGBA32F, tightly strided by `pitch` bytes.
bool can_decode(Fmt f);
bool can_encode(Fmt f);
nue_result decode_blocks(Fmt f, uint32_t w, uint32_t h, const void* src, bool to_float, void* dst, size_t dst_pitch, nue_tex_executor ex);
nue_result encode_blocks(Fmt f, uint32_t w, uint32_t h, bool src_float, const void* src, size_t src_pitch, void* dst, const nue_tex_encode_params& p);

// Run fn(begin, end) over [0,n) split in chunks on the executor (inline when ex is NULL or n small).
void parallel_for(nue_tex_executor ex, uint32_t n, uint32_t min_chunk, uint32_t max_threads, void (*fn)(void*, uint32_t, uint32_t), void* user);
template <class F> void parallel_for(nue_tex_executor ex, uint32_t n, uint32_t min_chunk, uint32_t max_threads, F&& f) {
    parallel_for(ex, n, min_chunk, max_threads, [](void* u, uint32_t b, uint32_t e) { (*static_cast<std::remove_reference_t<F>*>(u))(b, e); }, &f);
}

uint32_t crc32(uint32_t crc, const void* data, size_t size);
void sha256(const void* data, size_t size, uint8_t out[32]);
struct Sha256 { uint32_t s[8]; uint64_t len; uint8_t buf[64]; uint32_t used; Sha256(); void update(const void*, size_t); void finish(uint8_t out[32]); };

// Fast 128-bit content hash (two multiply-fold lanes, 32 bytes per step, several GB/s). Not cryptographic: it identifies texture content in a local cache, nothing is adversarial.
struct Hash128 { uint64_t a, b; };
Hash128 fast_hash128(const void* data, size_t size, uint64_t seed = 0);
std::string hex128(const Hash128& h);

float srgb_to_linear(float c);
float linear_to_srgb(float c);
uint16_t float_to_half(float f);
float half_to_float(uint16_t h);

}  // namespace nuetex
