#include "crc32.hpp"

#include <cstring>

namespace nue::android {
namespace {

struct Tables {
    uint32_t t[8][256];
    Tables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = t[0][i];
            for (int s = 1; s < 8; ++s) {
                c = t[0][c & 0xFF] ^ (c >> 8);
                t[s][i] = c;
            }
        }
    }
};

const Tables& tables() {
    static const Tables instance;
    return instance;
}

uint32_t load_le32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

} // namespace

uint32_t crc32_update(uint32_t crc, const void* data, size_t size) {
    const Tables& tb = tables();
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint32_t c = ~crc;
    while (size >= 8) {
        uint32_t lo = load_le32(p) ^ c;
        uint32_t hi = load_le32(p + 4);
        c = tb.t[7][lo & 0xFF] ^ tb.t[6][(lo >> 8) & 0xFF] ^ tb.t[5][(lo >> 16) & 0xFF] ^ tb.t[4][lo >> 24] ^
            tb.t[3][hi & 0xFF] ^ tb.t[2][(hi >> 8) & 0xFF] ^ tb.t[1][(hi >> 16) & 0xFF] ^ tb.t[0][hi >> 24];
        p += 8;
        size -= 8;
    }
    while (size--) c = tb.t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

} // namespace nue::android
