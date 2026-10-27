#include "nue_legacy/dsound_guest.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace nue::dsound::guest {
namespace {
using namespace nue::dsound;

constexpr uint32_t e_pointer = 0x80004003u;

// ---- the interfaces' vtables (dsound.h order) ------------------------------------------------------------------------------------------
#define M(i, slot, words, name) {DsInterface::i, slot, words, name}
const std::vector<MethodDesc> method_table = {
    // IDirectSound
    M(direct_sound, 0, 3, "IDirectSound::QueryInterface"), M(direct_sound, 1, 1, "IDirectSound::AddRef"), M(direct_sound, 2, 1, "IDirectSound::Release"),
    M(direct_sound, 3, 4, "IDirectSound::CreateSoundBuffer"), M(direct_sound, 4, 2, "IDirectSound::GetCaps"),
    M(direct_sound, 5, 3, "IDirectSound::DuplicateSoundBuffer"), M(direct_sound, 6, 3, "IDirectSound::SetCooperativeLevel"),
    M(direct_sound, 7, 1, "IDirectSound::Compact"), M(direct_sound, 8, 2, "IDirectSound::GetSpeakerConfig"),
    M(direct_sound, 9, 2, "IDirectSound::SetSpeakerConfig"), M(direct_sound, 10, 2, "IDirectSound::Initialize"),
    // IDirectSoundBuffer
    M(buffer, 0, 3, "IDirectSoundBuffer::QueryInterface"), M(buffer, 1, 1, "IDirectSoundBuffer::AddRef"), M(buffer, 2, 1, "IDirectSoundBuffer::Release"),
    M(buffer, 3, 2, "IDirectSoundBuffer::GetCaps"), M(buffer, 4, 3, "IDirectSoundBuffer::GetCurrentPosition"), M(buffer, 5, 4, "IDirectSoundBuffer::GetFormat"),
    M(buffer, 6, 2, "IDirectSoundBuffer::GetVolume"), M(buffer, 7, 2, "IDirectSoundBuffer::GetPan"), M(buffer, 8, 2, "IDirectSoundBuffer::GetFrequency"),
    M(buffer, 9, 2, "IDirectSoundBuffer::GetStatus"), M(buffer, 10, 3, "IDirectSoundBuffer::Initialize"), M(buffer, 11, 8, "IDirectSoundBuffer::Lock"),
    M(buffer, 12, 4, "IDirectSoundBuffer::Play"), M(buffer, 13, 2, "IDirectSoundBuffer::SetCurrentPosition"), M(buffer, 14, 2, "IDirectSoundBuffer::SetFormat"),
    M(buffer, 15, 2, "IDirectSoundBuffer::SetVolume"), M(buffer, 16, 2, "IDirectSoundBuffer::SetPan"), M(buffer, 17, 2, "IDirectSoundBuffer::SetFrequency"),
    M(buffer, 18, 1, "IDirectSoundBuffer::Stop"), M(buffer, 19, 5, "IDirectSoundBuffer::Unlock"), M(buffer, 20, 1, "IDirectSoundBuffer::Restore"),
    // IDirectSound3DBuffer
    M(buffer_3d, 0, 3, "IDirectSound3DBuffer::QueryInterface"), M(buffer_3d, 1, 1, "IDirectSound3DBuffer::AddRef"), M(buffer_3d, 2, 1, "IDirectSound3DBuffer::Release"),
    M(buffer_3d, 3, 2, "IDirectSound3DBuffer::GetAllParameters"), M(buffer_3d, 4, 3, "IDirectSound3DBuffer::GetConeAngles"),
    M(buffer_3d, 5, 2, "IDirectSound3DBuffer::GetConeOrientation"), M(buffer_3d, 6, 2, "IDirectSound3DBuffer::GetConeOutsideVolume"),
    M(buffer_3d, 7, 2, "IDirectSound3DBuffer::GetMaxDistance"), M(buffer_3d, 8, 2, "IDirectSound3DBuffer::GetMinDistance"),
    M(buffer_3d, 9, 2, "IDirectSound3DBuffer::GetMode"), M(buffer_3d, 10, 2, "IDirectSound3DBuffer::GetPosition"), M(buffer_3d, 11, 2, "IDirectSound3DBuffer::GetVelocity"),
    M(buffer_3d, 12, 3, "IDirectSound3DBuffer::SetAllParameters"), M(buffer_3d, 13, 4, "IDirectSound3DBuffer::SetConeAngles"),
    M(buffer_3d, 14, 5, "IDirectSound3DBuffer::SetConeOrientation"), M(buffer_3d, 15, 3, "IDirectSound3DBuffer::SetConeOutsideVolume"),
    M(buffer_3d, 16, 3, "IDirectSound3DBuffer::SetMaxDistance"), M(buffer_3d, 17, 3, "IDirectSound3DBuffer::SetMinDistance"),
    M(buffer_3d, 18, 3, "IDirectSound3DBuffer::SetMode"), M(buffer_3d, 19, 5, "IDirectSound3DBuffer::SetPosition"), M(buffer_3d, 20, 5, "IDirectSound3DBuffer::SetVelocity"),
    // IDirectSound3DListener
    M(listener_3d, 0, 3, "IDirectSound3DListener::QueryInterface"), M(listener_3d, 1, 1, "IDirectSound3DListener::AddRef"), M(listener_3d, 2, 1, "IDirectSound3DListener::Release"),
    M(listener_3d, 3, 2, "IDirectSound3DListener::GetAllParameters"), M(listener_3d, 4, 2, "IDirectSound3DListener::GetDistanceFactor"),
    M(listener_3d, 5, 2, "IDirectSound3DListener::GetDopplerFactor"), M(listener_3d, 6, 3, "IDirectSound3DListener::GetOrientation"),
    M(listener_3d, 7, 2, "IDirectSound3DListener::GetPosition"), M(listener_3d, 8, 2, "IDirectSound3DListener::GetRolloffFactor"),
    M(listener_3d, 9, 2, "IDirectSound3DListener::GetVelocity"), M(listener_3d, 10, 3, "IDirectSound3DListener::SetAllParameters"),
    M(listener_3d, 11, 3, "IDirectSound3DListener::SetDistanceFactor"), M(listener_3d, 12, 3, "IDirectSound3DListener::SetDopplerFactor"),
    M(listener_3d, 13, 8, "IDirectSound3DListener::SetOrientation"), M(listener_3d, 14, 5, "IDirectSound3DListener::SetPosition"),
    M(listener_3d, 15, 3, "IDirectSound3DListener::SetRolloffFactor"), M(listener_3d, 16, 5, "IDirectSound3DListener::SetVelocity"),
    M(listener_3d, 17, 1, "IDirectSound3DListener::CommitDeferredSettings"),
    // IDirectSoundNotify
    M(notify, 0, 3, "IDirectSoundNotify::QueryInterface"), M(notify, 1, 1, "IDirectSoundNotify::AddRef"), M(notify, 2, 1, "IDirectSoundNotify::Release"),
    M(notify, 3, 3, "IDirectSoundNotify::SetNotificationPositions"),
};
#undef M

constexpr size_t iface_count = size_t(DsInterface::count);
constexpr uint8_t guid_unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
constexpr uint8_t guid_ids[][16] = {
    {0x83, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60},   // IDirectSound
    {0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60},   // IDirectSoundBuffer
    {0x86, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60},   // IDirectSound3DBuffer
    {0x84, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60},   // IDirectSound3DListener
    {0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16},   // IDirectSoundNotify
};
// DSDEVID_DefaultPlayback {DEF00000-9C6D-47ED-AAF1-4DDA8F2B5C03} and DSDEVID_DefaultVoicePlayback {DEF00002-9C6D-47ED-AAF1-4DDA8F2B5C03}
constexpr uint8_t guid_default_playback[16] = {0x00, 0x00, 0xF0, 0xDE, 0x6D, 0x9C, 0xED, 0x47, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03};
constexpr uint8_t guid_default_voice[16] = {0x02, 0x00, 0xF0, 0xDE, 0x6D, 0x9C, 0xED, 0x47, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03};
constexpr uint8_t guid_subtype_pcm[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
constexpr uint8_t guid_subtype_float[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

uint32_t u32(Hr h) { return uint32_t(h); }
float as_float(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}
uint32_t as_word(float f) {
    uint32_t w;
    std::memcpy(&w, &f, 4);
    return w;
}
}  // namespace

struct DsoundGuestFacade::Impl {
    struct Obj {
        enum class Kind { device, buffer, primary } kind = Kind::device;
        uint32_t refs = 0, children = 0;
        Obj* owner = nullptr;
        bool zombie = false;                  // released by the application but still has live children (device)
        BufferId id;
        uint32_t flags = 0, bytes = 0, mirror = 0;
        uint32_t view[iface_count] = {};
        std::vector<Regions> locks;   // outstanding Locks (Windows allows several)
        bool has_pending_3d = false;
        Buffer3D pending_3d;
    };
    struct View {
        Obj* obj;
        DsInterface iface;
    };

    Impl(Memory& m, HostServices& h, Device& d) : memory(m), host(h), device(d) {
        counts.assign(method_table.size(), 0);
        failures.assign(method_table.size(), 0);
        tokens.assign(method_table.size(), 0);
        device.set_notify_callback(&Impl::on_notify, this);
    }
    ~Impl() {
        device.set_notify_callback(nullptr, nullptr);
        for (auto& o : objects) release_guest(*o);
        for (uint32_t t : vtables) if (t) memory.free(t);
    }

    Memory& memory;
    HostServices& host;
    Device& device;
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Obj>> objects;
    std::unordered_map<uint32_t, View> views;
    std::vector<uint64_t> counts, failures;
    std::vector<uint32_t> tokens;
    uint32_t vtables[iface_count] = {};
    uint32_t live_devices = 0;
    Obj* primary_obj = nullptr;
    std::string first_failure;
    Listener3D pending_listener;
    bool has_pending_listener = false;

    static void on_notify(void* self, uint64_t token) { static_cast<Impl*>(self)->host.set_event(uint32_t(token)); }

    // ---- guest memory helpers
    uint8_t* at(uint32_t address, uint32_t bytes) { return address ? memory.host(address, bytes) : nullptr; }
    bool rd32(uint32_t a, uint32_t& v) {
        const uint8_t* p = at(a, 4);
        if (!p) return false;
        std::memcpy(&v, p, 4);
        return true;
    }
    bool wr32(uint32_t a, uint32_t v) {
        uint8_t* p = at(a, 4);
        if (!p) return false;
        std::memcpy(p, &v, 4);
        return true;
    }
    bool wr_float(uint32_t a, float v) { return wr32(a, as_word(v)); }
    bool rd_float(uint32_t a, float& v) {
        uint32_t w;
        if (!rd32(a, w)) return false;
        v = as_float(w);
        return true;
    }
    bool rd_vec(uint32_t a, Vec3& v) { return rd_float(a, v.x) && rd_float(a + 4, v.y) && rd_float(a + 8, v.z); }
    bool wr_vec(uint32_t a, const Vec3& v) { return wr_float(a, v.x) && wr_float(a + 4, v.y) && wr_float(a + 8, v.z); }
    bool guid_is(uint32_t a, const uint8_t (&ref)[16]) {
        const uint8_t* p = at(a, 16);
        return p && std::memcmp(p, ref, 16) == 0;
    }

    // ---- COM objects
    uint32_t vtable_of(DsInterface iface) {
        const size_t i = size_t(iface);
        if (vtables[i]) return vtables[i];
        uint32_t slots = 0;
        for (const MethodDesc& m : method_table)
            if (m.iface == iface) slots = std::max<uint32_t>(slots, m.slot + 1u);
        const uint32_t table = memory.alloc(slots * 4);
        if (!table) return 0;
        for (size_t k = 0; k < method_table.size(); ++k)
            if (method_table[k].iface == iface) wr32(table + method_table[k].slot * 4u, tokens[k]);
        vtables[i] = table;
        return table;
    }
    uint32_t view_of(Obj& o, DsInterface iface) {
        uint32_t& v = o.view[size_t(iface)];
        if (v) return v;
        const uint32_t vt = vtable_of(iface);
        const uint32_t address = vt ? memory.alloc(16) : 0;
        if (!address) return 0;
        wr32(address, vt);
        wr32(address + 4, uint32_t(iface));
        views[address] = {&o, iface};
        v = address;
        return v;
    }
    Obj* new_obj(Obj::Kind kind) {
        objects.push_back(std::make_unique<Obj>());
        Obj* o = objects.back().get();
        o->kind = kind;
        o->refs = 1;
        return o;
    }
    void release_guest(Obj& o) {
        for (uint32_t& v : o.view)
            if (v) {
                views.erase(v);
                memory.free(v);
                v = 0;
            }
        if (o.mirror) {
            memory.free(o.mirror);
            o.mirror = 0;
        }
    }
    void destroy(Obj* o) {
        Obj* owner = o->owner;
        if (o->kind != Obj::Kind::device) {
            device.destroy_buffer(o->id);
            if (o == primary_obj) primary_obj = nullptr;
        }
        release_guest(*o);
        std::erase_if(objects, [&](const std::unique_ptr<Obj>& p) { return p.get() == o; });
        if (owner) {
            --owner->children;
            maybe_free_device(owner);
        }
    }
    void maybe_free_device(Obj* d) {
        if (d->zombie && d->children == 0) {
            if (live_devices > 0) --live_devices;
            destroy(d);
        }
    }
    uint32_t add_ref(Obj& o) { return ++o.refs; }
    uint32_t release(Obj* o) {
        if (o->refs == 0) return 0;
        const uint32_t refs = --o->refs;
        if (refs == 0) {
            if (o->kind == Obj::Kind::device && o->children > 0) {
                o->zombie = true;   // buffers keep the device alive, as in DirectSound
            } else {
                if (o->kind == Obj::Kind::device && live_devices > 0) --live_devices;
                destroy(o);
            }
        }
        return refs;
    }

    // ---- WAVEFORMATEX in guest memory
    // Returns hr::ok, or the error to give the application.
    // `ignored` (SetFormat only): the format tag is unknown to us but valid in shape; Windows accepts such a SetFormat and does nothing.
    Hr read_format(uint32_t a, WaveFormat& out, bool for_set_format, bool* ignored = nullptr) {
        const uint8_t* p = at(a, 16);
        if (!p) return hr::invalid_param;
        auto u16 = [&](uint32_t o) { uint16_t v; std::memcpy(&v, p + o, 2); return v; };
        auto u32_ = [&](uint32_t o) { uint32_t v; std::memcpy(&v, p + o, 4); return v; };
        const uint16_t tag = u16(0), channels = u16(2), block = u16(12), bits = u16(14);
        const uint32_t rate = u32_(4);
        const Hr bad = hr::invalid_param;   // measured: malformed formats are E_INVALIDARG, for SetFormat as well as for CreateSoundBuffer
        out = WaveFormat{};
        out.channels = channels;
        out.sample_rate = rate;
        out.bits = bits;
        if (tag == 1) {
            out.type = SampleType::pcm;
        } else if (tag == 3) {
            out.type = SampleType::ieee_float;
        } else if (tag == 0xFFFE) {
            const uint8_t* e = at(a, 40);
            if (!e || u16(16) < 22) return hr::bad_format;
            uint32_t mask;
            std::memcpy(&mask, e + 20, 4);
            out.channel_mask = mask;
            if (std::memcmp(e + 24, guid_subtype_pcm, 16) == 0) out.type = SampleType::pcm;
            else if (std::memcmp(e + 24, guid_subtype_float, 16) == 0) out.type = SampleType::ieee_float;
            else return hr::bad_format;
        } else {
            // Unknown tag. Measured: SetFormat on the primary buffer returns S_OK and ignores it, CreateSoundBuffer returns E_NOTIMPL.
            if (for_set_format && ignored) {
                *ignored = true;
                return hr::ok;
            }
            return hr::unsupported;
        }
        if (channels == 0 || bits == 0 || bits % 8 != 0) return bad;
        if (block != channels * (bits / 8u)) return bad;
        // Measured: more than two channels need WAVEFORMATEXTENSIBLE; a plain format with more is E_INVALIDARG for a buffer and DSERR_ALLOCATED for SetFormat.
        if (channels > 2 && tag != 0xFFFE) return for_set_format ? hr::allocated : bad;
        return hr::ok;
    }
    uint32_t write_format(uint32_t a, uint32_t allocated, const WaveFormat& f, uint32_t written_ptr) {
        uint8_t blob[40] = {};
        const bool extensible = f.channels > 2 || (f.channel_mask && f.channel_mask != speaker::default_mask(f.channels)) || (f.bits > 16);
        auto p16 = [&](uint32_t o, uint16_t v) { std::memcpy(blob + o, &v, 2); };
        auto p32 = [&](uint32_t o, uint32_t v) { std::memcpy(blob + o, &v, 4); };
        p16(0, extensible ? 0xFFFE : (f.type == SampleType::ieee_float ? 3 : 1));
        p16(2, f.channels);
        p32(4, f.sample_rate);
        p32(8, f.sample_rate * f.frame_bytes());
        p16(12, uint16_t(f.frame_bytes()));
        p16(14, f.bits);
        uint32_t size = 18;
        if (extensible) {
            p16(16, 22);
            p16(18, f.bits);
            p32(20, f.effective_mask());
            std::memcpy(blob + 24, f.type == SampleType::ieee_float ? guid_subtype_float : guid_subtype_pcm, 16);
            size = 40;
        }
        const uint32_t n = std::min(size, allocated);
        if (a && n) {
            uint8_t* dst = at(a, n);
            if (!dst) return u32(hr::invalid_param);
            std::memcpy(dst, blob, n);
        }
        if (written_ptr) wr32(written_ptr, a ? n : size);
        return u32(hr::ok);
    }

    // ---- method implementations
    Hr query_interface(Obj& o, uint32_t riid, uint32_t ppv) {
        if (!ppv) return Hr(e_pointer);
        wr32(ppv, 0);
        if (!at(riid, 16)) return hr::invalid_param;
        auto grant = [&](DsInterface iface) {
            const uint32_t v = view_of(o, iface);
            if (!v) return hr::out_of_memory;
            add_ref(o);
            wr32(ppv, v);
            return hr::ok;
        };
        if (guid_is(riid, guid_unknown)) return grant(o.kind == Obj::Kind::device ? DsInterface::direct_sound : DsInterface::buffer);
        if (o.kind == Obj::Kind::device) {
            if (guid_is(riid, guid_ids[0])) return grant(DsInterface::direct_sound);
            return hr::no_interface;   // IDirectSound8, IKsPropertySet and the rest: not offered
        }
        if (guid_is(riid, guid_ids[1])) return grant(DsInterface::buffer);
        if (guid_is(riid, guid_ids[2]) && (o.flags & buffer_flag::ctrl_3d) && o.kind == Obj::Kind::buffer) return grant(DsInterface::buffer_3d);
        if (guid_is(riid, guid_ids[3]) && (o.flags & buffer_flag::ctrl_3d) && o.kind == Obj::Kind::primary) return grant(DsInterface::listener_3d);
        if (guid_is(riid, guid_ids[4]) && (o.flags & buffer_flag::ctrl_position_notify) && o.kind == Obj::Kind::buffer) return grant(DsInterface::notify);
        return hr::no_interface;
    }

    Hr create_sound_buffer(Obj& dev, uint32_t desc, uint32_t out, uint32_t outer) {
        if (!out) return hr::invalid_param;
        wr32(out, 0);
        if (outer) return hr::no_aggregation;
        const uint8_t* d = at(desc, 20);
        if (!d) return hr::invalid_param;
        uint32_t size, flags, bytes, reserved, fmt_ptr;
        std::memcpy(&size, d, 4);
        std::memcpy(&flags, d + 4, 4);
        std::memcpy(&bytes, d + 8, 4);
        std::memcpy(&reserved, d + 12, 4);
        std::memcpy(&fmt_ptr, d + 16, 4);
        if (size != 20 && size != 36) return hr::invalid_param;
        (void)reserved;
        BufferDesc bd;
        bd.flags = flags;
        bd.bytes = bytes;
        if (!(flags & buffer_flag::primary)) {
            if (!fmt_ptr) return hr::invalid_param;
            if (const Hr h = read_format(fmt_ptr, bd.format, false); h < 0) return h;
            bd.has_format = true;
        } else if (fmt_ptr) {
            return hr::invalid_param;
        }
        const CreateResult r = device.create_buffer(bd);
        if (r.hr < 0) return r.hr;
        if (r.existing_primary && primary_obj) {
            add_ref(*primary_obj);
            wr32(out, view_of(*primary_obj, DsInterface::buffer));
            return hr::ok;
        }
        Obj* o = new_obj((flags & buffer_flag::primary) ? Obj::Kind::primary : Obj::Kind::buffer);
        o->id = r.id;
        o->flags = flags & ~buffer_flag::primary;
        if (flags & buffer_flag::primary) {
            o->flags = flags;
            primary_obj = o;
        } else {
            BufferCaps caps;
            device.get_caps(r.id, caps);
            o->bytes = caps.bytes;
            o->mirror = memory.alloc(caps.bytes);
            if (!o->mirror) {
                device.destroy_buffer(r.id);
                std::erase_if(objects, [&](const std::unique_ptr<Obj>& p) { return p.get() == o; });
                return hr::out_of_memory;
            }
            if (bd.format.bits == 8) std::memset(at(o->mirror, caps.bytes), 0x80, caps.bytes);
        }
        o->owner = &dev;
        ++dev.children;
        const uint32_t v = view_of(*o, DsInterface::buffer);
        if (!v) {
            destroy(o);
            return hr::out_of_memory;
        }
        wr32(out, v);
        return hr::ok;
    }

    Hr duplicate_buffer(Obj& dev, uint32_t src, uint32_t out) {
        if (!out) return hr::invalid_param;
        wr32(out, 0);
        auto it = views.find(src);
        if (it == views.end() || it->second.obj->kind != Obj::Kind::buffer) return hr::invalid_param;
        Obj& s = *it->second.obj;
        const CreateResult r = device.duplicate_buffer(s.id);
        if (r.hr < 0) return r.hr;
        Obj* o = new_obj(Obj::Kind::buffer);
        o->id = r.id;
        o->flags = s.flags;
        o->bytes = s.bytes;
        o->mirror = memory.alloc(s.bytes);
        if (!o->mirror) {
            device.destroy_buffer(r.id);
            std::erase_if(objects, [&](const std::unique_ptr<Obj>& p) { return p.get() == o; });
            return hr::out_of_memory;
        }
        if (s.mirror) std::memcpy(at(o->mirror, s.bytes), at(s.mirror, s.bytes), s.bytes);
        o->owner = &dev;
        ++dev.children;
        wr32(out, view_of(*o, DsInterface::buffer));
        return hr::ok;
    }

    Hr get_device_caps(uint32_t out) {
        const uint8_t* p = at(out, 4);
        if (!p) return hr::invalid_param;
        uint32_t size;
        std::memcpy(&size, p, 4);
        if (size != 96 || !at(out, 96)) return hr::invalid_param;
        const Caps c = device.caps();
        uint32_t words[24] = {};
        words[0] = 96;
        words[1] = c.flags;
        words[2] = c.min_secondary_rate;
        words[3] = c.max_secondary_rate;
        words[4] = c.primary_buffers;
        std::memcpy(at(out, 96), words, 96);
        return hr::ok;
    }

    Hr device_call(Obj& o, uint32_t slot, const uint32_t* a) {
        switch (slot) {
            case 0: return query_interface(o, a[1], a[2]);
            case 3: return create_sound_buffer(o, a[1], a[2], a[3]);
            case 4: return get_device_caps(a[1]);
            case 5: return duplicate_buffer(o, a[1], a[2]);
            case 6:
                if (!host.is_window(a[1])) return hr::invalid_param;
                return device.set_cooperative_level(a[2]);
            case 7: return hr::ok;   // Compact
            case 8: {                // GetSpeakerConfig
                if (!a[1]) return hr::invalid_param;
                const uint32_t channels = device.output().channels;
                const uint32_t config = channels >= 8 ? 7u : channels >= 6 ? 6u : channels >= 4 ? 3u : 4u;   // DSSPEAKER_7POINT1 / 5POINT1 / QUAD / STEREO
                wr32(a[1], config | (config == 4 ? 20u << 16 : 0u));
                return hr::ok;
            }
            case 9: return hr::ok;   // SetSpeakerConfig: accepted, the output follows the device and the primary format
            case 10: return hr::already_initialized;
            default: return hr::unsupported;
        }
    }

    Hr buffer_call(Obj& o, uint32_t slot, const uint32_t* a) {
        switch (slot) {
            case 0: return query_interface(o, a[1], a[2]);
            case 3: {   // GetCaps
                const uint8_t* p = at(a[1], 4);
                uint32_t size = 0;
                if (p) std::memcpy(&size, p, 4);
                if (!p || size != 20 || !at(a[1], 20)) return hr::invalid_param;
                BufferCaps c;
                if (const Hr h = device.get_caps(o.id, c); h < 0) return h;
                const uint32_t words[5] = {20, c.flags, c.bytes, 0, 0};
                std::memcpy(at(a[1], 20), words, 20);
                return hr::ok;
            }
            case 4: {   // GetCurrentPosition
                uint32_t play = 0, write = 0;
                if (a[1] && !at(a[1], 4)) return hr::invalid_param;
                if (a[2] && !at(a[2], 4)) return hr::invalid_param;
                if (const Hr h = device.get_current_position(o.id, a[1] ? &play : nullptr, a[2] ? &write : nullptr); h < 0) return h;
                if (a[1]) wr32(a[1], play);
                if (a[2]) wr32(a[2], write);
                return hr::ok;
            }
            case 5: {   // GetFormat(pwfx, allocated, pwritten)
                WaveFormat f;
                if (const Hr h = device.get_format(o.id, f); h < 0) return h;
                if (!a[1] && !a[3]) return hr::invalid_param;
                return Hr(write_format(a[1], a[2], f, a[3]));
            }
            case 6: case 7: case 8: {   // GetVolume / GetPan / GetFrequency
                if (!a[1] || !at(a[1], 4)) return hr::invalid_param;
                int32_t v = 0;
                uint32_t hz = 0;
                Hr h;
                if (slot == 6) h = device.get_volume(o.id, v);
                else if (slot == 7) h = device.get_pan(o.id, v);
                else { h = device.get_frequency(o.id, hz); v = int32_t(hz); }
                if (h >= 0) wr32(a[1], uint32_t(v));
                return h;
            }
            case 9: {   // GetStatus
                if (!a[1] || !at(a[1], 4)) return hr::invalid_param;
                uint32_t status = 0;
                const Hr h = device.get_status(o.id, status);
                if (h >= 0) wr32(a[1], status);
                return h;
            }
            case 10: return hr::already_initialized;
            case 11: return lock(o, a);
            case 12:
                if (a[1] != 0) return hr::invalid_param;   // dwReserved1
                return device.play(o.id, a[2], a[3]);
            case 13: return device.set_current_position(o.id, a[1]);
            case 14: {   // SetFormat
                WaveFormat f;
                bool ignored = false;
                if (const Hr h = read_format(a[1], f, true, &ignored); h < 0) return h;
                if (ignored) return hr::ok;
                return device.set_format(o.id, f);
            }
            case 15: return device.set_volume(o.id, int32_t(a[1]));
            case 16: return device.set_pan(o.id, int32_t(a[1]));
            case 17: return device.set_frequency(o.id, a[1]);
            case 18: return device.stop(o.id);
            case 19: return unlock(o, a);
            case 20: return device.restore(o.id);
            default: return hr::unsupported;
        }
    }

    Hr lock(Obj& o, const uint32_t* a) {
        // Lock(this, offset, bytes, ppv1, pcb1, ppv2, pcb2, flags)
        const uint32_t offset = a[1], bytes = a[2], ppv1 = a[3], pcb1 = a[4], ppv2 = a[5], pcb2 = a[6], flags = a[7];
        if (!ppv1 || !pcb1 || !at(ppv1, 4) || !at(pcb1, 4)) return hr::invalid_param;
        if ((ppv2 && !at(ppv2, 4)) || (pcb2 && !at(pcb2, 4))) return hr::invalid_param;
        if (o.kind != Obj::Kind::buffer || !o.mirror) return hr::invalid_call;
        wr32(ppv1, 0);
        wr32(pcb1, 0);
        if (ppv2) wr32(ppv2, 0);
        if (pcb2) wr32(pcb2, 0);
        Regions r;
        if (const Hr h = device.lock(o.id, offset, bytes, flags, r); h < 0) return h;
        if (o.locks.size() >= 32) return hr::invalid_call;
        if (r.bytes2 && (!ppv2 || !pcb2)) r.bytes2 = 0;   // measured: without a second pointer Windows still succeeds (the wrapped part is simply not offered)
        o.locks.push_back(r);
        wr32(ppv1, o.mirror + r.offset1);
        wr32(pcb1, r.bytes1);
        if (ppv2) wr32(ppv2, r.bytes2 ? o.mirror + r.offset2 : 0);
        if (pcb2) wr32(pcb2, r.bytes2);
        return hr::ok;
    }

    Hr unlock(Obj& o, const uint32_t* a) {
        // Unlock(this, pv1, cb1, pv2, cb2)
        if (o.kind != Obj::Kind::buffer || o.locks.empty()) return hr::invalid_param;
        const uint32_t pv1 = a[1], cb1 = a[2], pv2 = a[3], cb2 = a[4];
        size_t match = o.locks.size();
        for (size_t i = 0; i < o.locks.size(); ++i) {   // the oldest outstanding Lock that these pointers belong to
            const Regions& c = o.locks[i];
            if (pv1 != o.mirror + c.offset1 || cb1 > c.bytes1) continue;
            if (cb2 && (pv2 != o.mirror + c.offset2 || cb2 > c.bytes2)) continue;
            match = i;
            break;
        }
        if (match == o.locks.size()) return hr::invalid_param;
        const Regions r = o.locks[match];
        o.locks.erase(o.locks.begin() + std::ptrdiff_t(match));
        Regions written = r;
        written.bytes1 = cb1;
        written.bytes2 = cb2;
        const uint8_t* p1 = at(pv1, cb1 ? cb1 : 1);
        const uint8_t* p2 = cb2 ? at(pv2, cb2) : nullptr;
        if ((cb1 && !p1) || (cb2 && !p2)) return hr::invalid_param;
        return device.unlock(o.id, written, Bytes(static_cast<const std::byte*>(static_cast<const void*>(p1)), cb1), Bytes(static_cast<const std::byte*>(static_cast<const void*>(p2)), cb2));
    }

    // ---- 3D buffer / listener / notify
    Hr read_3d_buffer(uint32_t a, Buffer3D& b) {
        if (!at(a, 64)) return hr::invalid_param;
        uint32_t size;
        rd32(a, size);
        if (size != 64) return hr::invalid_param;
        uint32_t inside, outside, mode, vol;
        float minimum, maximum;
        rd_vec(a + 4, b.position);
        rd_vec(a + 16, b.velocity);
        rd32(a + 28, inside);
        rd32(a + 32, outside);
        rd_vec(a + 36, b.cone_orientation);
        rd32(a + 48, vol);
        rd_float(a + 52, minimum);
        rd_float(a + 56, maximum);
        rd32(a + 60, mode);
        b.inside_cone_angle = inside;
        b.outside_cone_angle = outside;
        b.cone_outside_volume = int32_t(vol);
        b.min_distance = minimum;
        b.max_distance = maximum;
        if (mode > 2) return hr::invalid_param;
        b.mode = Mode3D(mode);
        return hr::ok;
    }
    Hr apply_3d(Obj& o, const Buffer3D& b, uint32_t apply) {
        if (apply == 1) {   // DS3D_DEFERRED
            if (!o.has_pending_3d) device.get_buffer_3d(o.id, o.pending_3d);
            o.pending_3d = b;
            o.has_pending_3d = true;
            return hr::ok;
        }
        o.has_pending_3d = false;
        return device.set_buffer_3d(o.id, b);
    }
    // The 3D parameters a setter starts from: the deferred copy when one is pending.
    Hr current_3d(Obj& o, Buffer3D& b) {
        if (o.has_pending_3d) { b = o.pending_3d; return hr::ok; }
        return device.get_buffer_3d(o.id, b);
    }

    Hr buffer_3d_call(Obj& o, uint32_t slot, const uint32_t* a) {
        if (slot == 0) return query_interface(o, a[1], a[2]);
        Buffer3D b;
        if (slot >= 3 && slot <= 11) {   // getters
            if (!a[1] || !at(a[1], 4)) return hr::invalid_param;
            if (const Hr h = current_3d(o, b); h < 0) return h;
            switch (slot) {
                case 3: {
                    if (!at(a[1], 64)) return hr::invalid_param;
                    uint32_t size;
                    rd32(a[1], size);
                    if (size != 64) return hr::invalid_param;
                    wr_vec(a[1] + 4, b.position);
                    wr_vec(a[1] + 16, b.velocity);
                    wr32(a[1] + 28, b.inside_cone_angle);
                    wr32(a[1] + 32, b.outside_cone_angle);
                    wr_vec(a[1] + 36, b.cone_orientation);
                    wr32(a[1] + 48, uint32_t(b.cone_outside_volume));
                    wr_float(a[1] + 52, b.min_distance);
                    wr_float(a[1] + 56, b.max_distance);
                    wr32(a[1] + 60, uint32_t(b.mode));
                    return hr::ok;
                }
                case 4:
                    if (!a[2] || !at(a[2], 4)) return hr::invalid_param;
                    wr32(a[1], b.inside_cone_angle);
                    wr32(a[2], b.outside_cone_angle);
                    return hr::ok;
                case 5: return wr_vec(a[1], b.cone_orientation) ? hr::ok : hr::invalid_param;
                case 6: wr32(a[1], uint32_t(b.cone_outside_volume)); return hr::ok;
                case 7: wr_float(a[1], b.max_distance); return hr::ok;
                case 8: wr_float(a[1], b.min_distance); return hr::ok;
                case 9: wr32(a[1], uint32_t(b.mode)); return hr::ok;
                case 10: return wr_vec(a[1], b.position) ? hr::ok : hr::invalid_param;
                default: return wr_vec(a[1], b.velocity) ? hr::ok : hr::invalid_param;
            }
        }
        if (slot == 12) {   // SetAllParameters(this, pds3db, apply)
            if (const Hr h = read_3d_buffer(a[1], b); h < 0) return h;
            return apply_3d(o, b, a[2]);
        }
        if (const Hr h = current_3d(o, b); h < 0) return h;
        uint32_t apply = 0;
        switch (slot) {
            case 13: b.inside_cone_angle = a[1]; b.outside_cone_angle = a[2]; apply = a[3]; break;
            case 14: b.cone_orientation = {as_float(a[1]), as_float(a[2]), as_float(a[3])}; apply = a[4]; break;
            case 15: b.cone_outside_volume = int32_t(a[1]); apply = a[2]; break;
            case 16: b.max_distance = as_float(a[1]); apply = a[2]; break;
            case 17: b.min_distance = as_float(a[1]); apply = a[2]; break;
            case 18:
                if (a[1] > 2) return hr::invalid_param;
                b.mode = Mode3D(a[1]);
                apply = a[2];
                break;
            case 19: b.position = {as_float(a[1]), as_float(a[2]), as_float(a[3])}; apply = a[4]; break;
            case 20: b.velocity = {as_float(a[1]), as_float(a[2]), as_float(a[3])}; apply = a[4]; break;
            default: return hr::unsupported;
        }
        return apply_3d(o, b, apply);
    }

    Hr apply_listener(const Listener3D& l, uint32_t apply) {
        if (apply == 1) {
            pending_listener = l;
            has_pending_listener = true;
            return hr::ok;
        }
        has_pending_listener = false;
        return device.set_listener(l);
    }
    Hr listener_call(Obj& o, uint32_t slot, const uint32_t* a) {
        if (slot == 0) return query_interface(o, a[1], a[2]);
        Listener3D l = has_pending_listener ? pending_listener : device.listener();
        if (slot >= 3 && slot <= 9) {
            if (!a[1] || !at(a[1], 4)) return hr::invalid_param;
            switch (slot) {
                case 3: {
                    if (!at(a[1], 64)) return hr::invalid_param;
                    uint32_t size;
                    rd32(a[1], size);
                    if (size != 64) return hr::invalid_param;
                    wr_vec(a[1] + 4, l.position);
                    wr_vec(a[1] + 16, l.velocity);
                    wr_vec(a[1] + 28, l.front);
                    wr_vec(a[1] + 40, l.top);
                    wr_float(a[1] + 52, l.distance_factor);
                    wr_float(a[1] + 56, l.rolloff_factor);
                    wr_float(a[1] + 60, l.doppler_factor);
                    return hr::ok;
                }
                case 4: wr_float(a[1], l.distance_factor); return hr::ok;
                case 5: wr_float(a[1], l.doppler_factor); return hr::ok;
                case 6:
                    if (!a[2] || !at(a[2], 12)) return hr::invalid_param;
                    wr_vec(a[1], l.front);
                    wr_vec(a[2], l.top);
                    return hr::ok;
                case 7: return wr_vec(a[1], l.position) ? hr::ok : hr::invalid_param;
                case 8: wr_float(a[1], l.rolloff_factor); return hr::ok;
                default: return wr_vec(a[1], l.velocity) ? hr::ok : hr::invalid_param;
            }
        }
        uint32_t apply = 0;
        switch (slot) {
            case 10: {
                if (!at(a[1], 64)) return hr::invalid_param;
                uint32_t size;
                rd32(a[1], size);
                if (size != 64) return hr::invalid_param;
                rd_vec(a[1] + 4, l.position);
                rd_vec(a[1] + 16, l.velocity);
                rd_vec(a[1] + 28, l.front);
                rd_vec(a[1] + 40, l.top);
                rd_float(a[1] + 52, l.distance_factor);
                rd_float(a[1] + 56, l.rolloff_factor);
                rd_float(a[1] + 60, l.doppler_factor);
                apply = a[2];
                break;
            }
            case 11: l.distance_factor = as_float(a[1]); apply = a[2]; break;
            case 12: l.doppler_factor = as_float(a[1]); apply = a[2]; break;
            case 13: l.front = {as_float(a[1]), as_float(a[2]), as_float(a[3])}; l.top = {as_float(a[4]), as_float(a[5]), as_float(a[6])}; apply = a[7]; break;
            case 14: l.position = {as_float(a[1]), as_float(a[2]), as_float(a[3])}; apply = a[4]; break;
            case 15: l.rolloff_factor = as_float(a[1]); apply = a[2]; break;
            case 16: l.velocity = {as_float(a[1]), as_float(a[2]), as_float(a[3])}; apply = a[4]; break;
            case 17: {   // CommitDeferredSettings: listener and every 3D buffer
                Hr result = hr::ok;
                if (has_pending_listener) {
                    has_pending_listener = false;
                    result = device.set_listener(pending_listener);
                }
                for (auto& obj : objects)
                    if (obj->has_pending_3d) {
                        obj->has_pending_3d = false;
                        if (const Hr h = device.set_buffer_3d(obj->id, obj->pending_3d); h < 0) result = h;
                    }
                return result;
            }
            default: return hr::unsupported;
        }
        return apply_listener(l, apply);
    }

    Hr notify_call(Obj& o, uint32_t slot, const uint32_t* a) {
        if (slot == 0) return query_interface(o, a[1], a[2]);
        if (slot != 3) return hr::unsupported;
        const uint32_t count = a[1];
        if (count > 0 && (!a[2] || !at(a[2], count * 8))) return hr::invalid_param;
        std::vector<Notify> list;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t off, handle;
            rd32(a[2] + i * 8, off);
            rd32(a[2] + i * 8 + 4, handle);
            list.push_back({off, handle});
        }
        return device.set_notifications(o.id, list);
    }

    uint32_t dispatch(size_t index, const uint32_t* a) {
        std::lock_guard lock(mutex);
        const MethodDesc& m = method_table[index];
        ++counts[index];
        auto it = views.find(a[0]);
        Hr result = hr::invalid_param;
        uint32_t returned = 0;
        bool is_hresult = true;
        if (it != views.end() && it->second.iface == m.iface) {
            Obj& o = *it->second.obj;
            if (m.slot == 1) {
                returned = add_ref(o);
                is_hresult = false;
            } else if (m.slot == 2) {
                returned = release(&o);
                is_hresult = false;
            } else {
                switch (m.iface) {
                    case DsInterface::direct_sound: result = device_call(o, m.slot, a); break;
                    case DsInterface::buffer: result = buffer_call(o, m.slot, a); break;
                    case DsInterface::buffer_3d: result = buffer_3d_call(o, m.slot, a); break;
                    case DsInterface::listener_3d: result = listener_call(o, m.slot, a); break;
                    case DsInterface::notify: result = notify_call(o, m.slot, a); break;
                    default: break;
                }
            }
        }
        if (is_hresult) {
            returned = u32(result);
            if (result < 0) {
                ++failures[index];
                if (first_failure.empty()) {
                    std::ostringstream s;
                    s << m.name << " -> 0x" << std::hex << returned << " (this=0x" << a[0];
                    for (uint8_t i = 1; i < m.words; ++i) s << ", 0x" << a[i];
                    s << ")";
                    first_failure = s.str();
                }
            }
        }
        return returned;
    }

    uint32_t create(uint32_t guid, uint32_t out, uint32_t outer) {
        std::lock_guard lock(mutex);
        ++create_calls;
        if (!out || !at(out, 4)) return u32(hr::invalid_param);
        wr32(out, 0);
        if (outer) return u32(hr::no_aggregation);
        if (guid && !guid_is(guid, guid_default_playback) && !guid_is(guid, guid_default_voice)) {
            const uint8_t* g = at(guid, 16);
            bool zero = g != nullptr;
            for (int i = 0; g && i < 16; ++i) zero = zero && g[i] == 0;
            if (!zero) return u32(hr::no_driver);   // the only device is the default playback device
        }
        if (live_devices == 0) device.reset();
        Obj* o = new_obj(Obj::Kind::device);
        const uint32_t v = view_of(*o, DsInterface::direct_sound);
        if (!v) {
            std::erase_if(objects, [&](const std::unique_ptr<Obj>& p) { return p.get() == o; });
            return u32(hr::out_of_memory);
        }
        ++live_devices;
        wr32(out, v);
        return u32(hr::ok);
    }
    uint64_t create_calls = 0;
};

DsoundGuestFacade::DsoundGuestFacade(Memory& memory, HostServices& host, Device& device) : impl_(std::make_unique<Impl>(memory, host, device)) {}
DsoundGuestFacade::~DsoundGuestFacade() = default;

const std::vector<MethodDesc>& DsoundGuestFacade::methods() { return method_table; }

void DsoundGuestFacade::set_token(size_t index, uint32_t token) {
    std::lock_guard lock(impl_->mutex);
    if (index < impl_->tokens.size()) impl_->tokens[index] = token;
}

uint32_t DsoundGuestFacade::direct_sound_create(uint32_t guid, uint32_t out, uint32_t outer) { return impl_->create(guid, out, outer); }

uint32_t DsoundGuestFacade::call(size_t index, const uint32_t* args) {
    if (index >= method_table.size()) return u32(hr::unsupported);
    return impl_->dispatch(index, args);
}

uint32_t DsoundGuestFacade::live_objects() const {
    std::lock_guard lock(impl_->mutex);
    return uint32_t(impl_->objects.size());
}

std::string DsoundGuestFacade::report() const {
    std::lock_guard lock(impl_->mutex);
    std::ostringstream s;
    s << "DirectSoundCreate calls: " << impl_->create_calls << ", live objects: " << impl_->objects.size();
    for (size_t i = 0; i < method_table.size(); ++i)
        if (impl_->counts[i]) {
            s << "\n  " << method_table[i].name << ": " << impl_->counts[i] << " calls";
            if (impl_->failures[i]) s << ", " << impl_->failures[i] << " failed";
        }
    if (!impl_->first_failure.empty()) s << "\n  first failure: " << impl_->first_failure;
    return s.str();
}

}  // namespace nue::dsound::guest
