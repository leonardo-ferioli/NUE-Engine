// CRC-32 (IEEE 802.3, the zlib/PKZIP polynomial) used by the storage import integrity check.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nue::android {

// Slicing-by-8; about 1.5-3 GB/s on a phone core, so verifying 1.2 GB costs well under a second of CPU
// compared with the flash write time.
uint32_t crc32_update(uint32_t crc, const void* data, size_t size);

class Crc32 {
public:
    void update(const void* data, size_t size) { crc_ = crc32_update(crc_, data, size); }
    uint32_t value() const { return crc_; }
    void reset() { crc_ = 0; }

private:
    uint32_t crc_ = 0;
};

inline uint32_t crc32(const void* data, size_t size) { return crc32_update(0, data, size); }

} // namespace nue::android
