// Software mixer. Unit helpers are implemented; the mixer engine itself (voices, buses, rendering) is the next block (see docs/STATUS.md).
#include "internal/core_internal.h"

#include <cmath>

namespace nuei {
void mixer_shutdown_all() {}
}  // namespace nuei

extern "C" {

NUE_API float NUE_CALL nue_db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }
NUE_API float NUE_CALL nue_linear_to_db(float linear) { return linear <= 1e-10f ? -200.0f : 20.0f * std::log10(linear); }

NUE_API float NUE_CALL nue_centibels_to_linear(int32_t cb) {
    if (cb >= 0) return 1.0f;
    if (cb <= -10000) return 0.0f;   // DirectSound: -10000 is silence
    return std::pow(10.0f, float(cb) / 2000.0f);
}

NUE_API int32_t NUE_CALL nue_linear_to_centibels(float linear) {
    if (linear >= 1.0f) return 0;
    if (linear <= 1e-5f) return -10000;
    int32_t cb = int32_t(std::lround(2000.0f * std::log10(linear)));
    return cb < -10000 ? -10000 : cb;
}

NUE_API float NUE_CALL nue_pan_from_centibels(int32_t pan) {
    // DirectSound pan: 0 = centre; positive attenuates the left channel by `pan` centibels, negative the right one.
    // Map to -1..1 by the attenuated side's remaining amplitude: -10000..10000 spans the full left..right range.
    if (pan > 10000) pan = 10000;
    if (pan < -10000) pan = -10000;
    float atten = nue_centibels_to_linear(-(pan < 0 ? -pan : pan));
    float side = 1.0f - atten;
    return pan >= 0 ? side : -side;
}

}  // extern "C"
