// Shared helpers of the DSP files: lock-free parameter hand-over, decibel conversions, denormal guard.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace nuea {

constexpr float kPi = 3.14159265358979323846f;
constexpr double kPiD = 3.14159265358979323846;

inline float db_to_lin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float lin_to_db(float lin) { return 20.0f * std::log10(lin > 1e-12f ? lin : 1e-12f); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float flush_denormal(float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; }

// Single writer (any thread), single reader (audio thread). The reader gets a consistent copy or keeps what it had.
template <class T>
class ParamBox {
    static_assert(std::is_trivially_copyable<T>::value, "ParamBox needs a trivially copyable type");
public:
    void set(const T& v) {                                 // writers are serialised by the caller (setters are not called concurrently)
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&val_, &v, sizeof(T));
        seq_.store(s + 2, std::memory_order_release);
    }
    // Returns true when `out` was refreshed.
    bool get(T& out, uint32_t& seen) const {
        for (int tries = 0; tries < 4; ++tries) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 == seen) return false;
            if (s1 & 1) continue;
            T tmp;
            std::memcpy(&tmp, &val_, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_acquire) == s1) { out = tmp; seen = s1; return true; }
        }
        return false;
    }
private:
    alignas(8) T val_{};
    std::atomic<uint32_t> seq_{0};
};

}  // namespace nuea
