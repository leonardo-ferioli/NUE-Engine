// Explicit little-endian accessors: files never depend on the host byte order or on struct layout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nuea {

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
inline void wr64(uint8_t* p, uint64_t v) { wr32(p, uint32_t(v)); wr32(p + 4, uint32_t(v >> 32)); }

inline uint64_t align_up(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }
inline bool is_pow2(uint64_t v) { return v && !(v & (v - 1)); }
inline uint64_t lowbit(uint64_t v) { return v & (~v + 1); }

// Overflow checked arithmetic for sizes that come from files.
inline bool add_ok(uint64_t a, uint64_t b, uint64_t* out) { uint64_t r = a + b; if (r < a) return false; *out = r; return true; }
inline bool mul_ok(uint64_t a, uint64_t b, uint64_t* out) { if (a && b > UINT64_MAX / a) return false; *out = a * b; return true; }

}  // namespace nuea
