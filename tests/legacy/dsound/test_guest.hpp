// Test support: a fake guest (arena memory, host services, call-by-token helper) around the DirectSound facade, shared by the facade tests and
// the differential test. It drives the facade the way translated code does: COM pointer -> vtable of tokens -> stdcall words.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nue_legacy/dsound_engine.hpp"
#include "nue_legacy/dsound_guest.hpp"
#include "test_support.hpp"

namespace nue::dsound::testing {
using namespace nue::dsound::guest;

inline constexpr uint32_t HR_OK = 0, HR_INVALIDARG = 0x80070057, HR_NOINTERFACE = 0x80004002, HR_NODRIVER = 0x88780078, HR_NOAGG = 0x80040110, HR_INVALIDCALL = 0x88780032,
                   HR_PRIOLEVELNEEDED = 0x88780046, HR_BADFORMAT = 0x88780064, HR_CONTROLUNAVAIL = 0x8878001E, HR_ALREADYINIT = 0x88780082;

// Where the next Arena puts guest address 0 of its memory (tests run once low and once above 2 GiB to catch signed or truncated address arithmetic).
inline uint32_t g_arena_base = 0x10000;

// A guest arena: a flat byte array at 32-bit addresses starting at 0x10000. Its host addresses have nothing to do with the guest addresses (a heap block on
// any host, 32 or 64 bits), so a library that confuses the two fails here.
class Arena final : public Memory {
public:
    Arena() : mem_(32u << 20, 0) {}
    uint32_t alloc(uint32_t bytes) override {
        next_ = (next_ + 15) & ~15u;
        const uint32_t a = next_;
        if (size_t(a) - base_ + bytes > mem_.size()) return 0;
        next_ += bytes ? bytes : 1;
        std::memset(mem_.data() + (a - base_), 0, bytes);
        ++live;
        return a;
    }
    void free(uint32_t) override { --live; }
    uint8_t* host(uint32_t a, uint32_t bytes) override {
        if (a < base_ || size_t(a) - base_ + bytes > mem_.size()) return nullptr;
        return mem_.data() + (a - base_);
    }
    uint32_t address_of(const void* p) override {
        const auto* b = static_cast<const uint8_t*>(p);
        if (b < mem_.data() || b >= mem_.data() + mem_.size()) return 0;
        return base_ + uint32_t(b - mem_.data());
    }
    int live = 0;

private:
    uint32_t base_ = g_arena_base;
    std::vector<uint8_t> mem_;
    uint32_t next_ = base_;
};

struct Host final : HostServices {
    std::vector<uint32_t> events;
    void set_event(uint32_t h) override { events.push_back(h); }
    bool is_window(uint32_t h) override { return h == 0x1234 || h == 0x5678; }
};

inline constexpr uint32_t token_base = 0xf1000000u;

struct Guest {
    Arena arena;
    Host host;
    std::unique_ptr<Engine> engine;
    ManualSink* sink = nullptr;
    std::unique_ptr<DsoundGuestFacade> facade;
    Guest() {
        auto s = std::make_unique<ManualSink>(480, 480);
        sink = s.get();
        EngineConfig cfg;
        engine = Engine::create(cfg, std::move(s));
        engine->start();
        facade = std::make_unique<DsoundGuestFacade>(arena, host, engine->device());
        const auto& m = DsoundGuestFacade::methods();
        for (size_t i = 0; i < m.size(); ++i) facade->set_token(i, token_base + uint32_t(i) * 16);
    }
    // ---- memory helpers
    uint32_t put(const void* p, uint32_t n) {
        const uint32_t a = arena.alloc(n);
        std::memcpy(arena.host(a, n), p, n);
        return a;
    }
    uint32_t cell(uint32_t value = 0) { return put(&value, 4); }
    uint32_t get32(uint32_t a) {
        uint32_t v;
        std::memcpy(&v, arena.host(a, 4), 4);
        return v;
    }
    uint32_t guid(const uint8_t (&g)[16]) { return put(g, 16); }
    // A call through a COM pointer like `mov ecx,[obj]; push args; push obj; call [ecx+slot*4]`.
    uint32_t call(uint32_t obj, uint32_t slot, std::vector<uint32_t> args = {}) {
        const uint32_t vtable = get32(obj);
        const uint32_t token = get32(vtable + slot * 4);
        if (token < token_base) { std::printf("guest: slot %u of object 0x%x holds no token\n", slot, obj); std::abort(); }
        const size_t index = (token - token_base) / 16;
        const auto& methods = DsoundGuestFacade::methods();
        if (index >= methods.size()) { std::printf("guest: bad token index\n"); std::abort(); }
        std::vector<uint32_t> words;
        words.push_back(obj);
        words.insert(words.end(), args.begin(), args.end());
        // the callee pops exactly the arguments pushed
        if (methods[index].words != words.size()) { std::printf("guest: %s expects %u words, call pushed %zu\n", methods[index].name, unsigned(methods[index].words), words.size()); std::abort(); }
        return facade->call(index, words.data());
    }
    uint32_t create(uint32_t* out_obj, uint32_t guid_ptr = 0, uint32_t outer = 0) {
        const uint32_t pp = cell();
        const uint32_t hr = facade->direct_sound_create(guid_ptr, pp, outer);
        *out_obj = get32(pp);
        return hr;
    }
    // WAVEFORMATEX / WAVEFORMATEXTENSIBLE as the game builds them
    uint32_t wave(uint16_t channels, uint32_t rate, uint16_t bits = 16, uint32_t mask = 0, bool extensible = false) {
        uint8_t b[40] = {};
        auto p16 = [&](int o, uint16_t v) { std::memcpy(b + o, &v, 2); };
        auto p32 = [&](int o, uint32_t v) { std::memcpy(b + o, &v, 4); };
        p16(0, extensible ? 0xFFFE : 1);
        p16(2, channels);
        p32(4, rate);
        p32(8, rate * channels * bits / 8);
        p16(12, uint16_t(channels * bits / 8));
        p16(14, bits);
        if (extensible) {
            p16(16, 22);
            p16(18, bits);
            p32(20, mask);
            const uint8_t pcm[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
            std::memcpy(b + 24, pcm, 16);
        }
        return put(b, extensible ? 40 : 18);
    }
    uint32_t desc(uint32_t flags, uint32_t bytes, uint32_t wfx, uint32_t size = 36) {
        uint32_t d[9] = {size, flags, bytes, 0, wfx, 0, 0, 0, 0};
        return put(d, 36);
    }
};

inline constexpr uint8_t IID_IUnknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
inline constexpr uint8_t IID_IDirectSound[16] = {0x83, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
inline constexpr uint8_t IID_IDirectSoundBuffer[16] = {0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
inline constexpr uint8_t IID_IDirectSound3DBuffer[16] = {0x86, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
inline constexpr uint8_t IID_IDirectSound3DListener[16] = {0x84, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
inline constexpr uint8_t IID_IDirectSoundNotify[16] = {0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16};
inline constexpr uint8_t IID_IDirectSound8[16] = {0x93, 0x7E, 0x0A, 0xC5, 0x95, 0xF3, 0x34, 0x48, 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE8, 0x2C, 0x07};
inline constexpr uint8_t IID_IKsPropertySet[16] = {0x30, 0xAC, 0xEF, 0x31, 0x5C, 0x51, 0xD0, 0x11, 0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93};

// Slot numbers (dsound.h order).
namespace ds_slot { enum { QI = 0, AddRef = 1, Release = 2, CreateSoundBuffer = 3, GetCaps = 4, Duplicate = 5, SetCoop = 6, Compact = 7, GetSpeaker = 8, SetSpeaker = 9, Initialize = 10 }; }
namespace buf_slot {
enum { QI = 0, AddRef = 1, Release = 2, GetCaps = 3, GetCurrentPosition = 4, GetFormat = 5, GetVolume = 6, GetPan = 7, GetFrequency = 8, GetStatus = 9, Initialize = 10, Lock = 11, Play = 12,
       SetCurrentPosition = 13, SetFormat = 14, SetVolume = 15, SetPan = 16, SetFrequency = 17, Stop = 18, Unlock = 19, Restore = 20 };
}


}  // namespace nue::dsound::testing
