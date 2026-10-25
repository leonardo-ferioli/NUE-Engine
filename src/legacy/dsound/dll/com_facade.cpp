#include "com_facade.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

namespace nue::dsound::com {
namespace {

constexpr HRESULT DS_E_POINTER = HRESULT(0x80004003L);   // E_POINTER

bool guid_eq(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

HRESULT as_hresult(Hr h) { return HRESULT(h); }

}  // namespace

struct Facade::Impl {
    struct Obj;

    // One view per COM interface. Every view of an object shares the object's reference count (COM rules for one identity).
    struct DeviceView final : IDirectSound {
        Obj* o = nullptr;
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer) override;
        STDMETHOD(GetCaps)(LPDSCAPS caps) override;
        STDMETHOD(DuplicateSoundBuffer)(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER* duplicate) override;
        STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD level) override;
        STDMETHOD(Compact)() override;
        STDMETHOD(GetSpeakerConfig)(LPDWORD config) override;
        STDMETHOD(SetSpeakerConfig)(DWORD config) override;
        STDMETHOD(Initialize)(LPCGUID device) override;
    };
    struct BufferView final : IDirectSoundBuffer {
        Obj* o = nullptr;
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(GetCaps)(LPDSBCAPS caps) override;
        STDMETHOD(GetCurrentPosition)(LPDWORD play, LPDWORD write) override;
        STDMETHOD(GetFormat)(LPWAVEFORMATEX format, DWORD allocated, LPDWORD written) override;
        STDMETHOD(GetVolume)(LPLONG volume) override;
        STDMETHOD(GetPan)(LPLONG pan) override;
        STDMETHOD(GetFrequency)(LPDWORD frequency) override;
        STDMETHOD(GetStatus)(LPDWORD status) override;
        STDMETHOD(Initialize)(LPDIRECTSOUND ds, LPCDSBUFFERDESC desc) override;
        STDMETHOD(Lock)(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) override;
        STDMETHOD(Play)(DWORD reserved1, DWORD priority, DWORD flags) override;
        STDMETHOD(SetCurrentPosition)(DWORD position) override;
        STDMETHOD(SetFormat)(LPCWAVEFORMATEX format) override;
        STDMETHOD(SetVolume)(LONG volume) override;
        STDMETHOD(SetPan)(LONG pan) override;
        STDMETHOD(SetFrequency)(DWORD frequency) override;
        STDMETHOD(Stop)() override;
        STDMETHOD(Unlock)(LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) override;
        STDMETHOD(Restore)() override;
    };
    struct Buffer3DView final : IDirectSound3DBuffer {
        Obj* o = nullptr;
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(GetAllParameters)(LPDS3DBUFFER p) override;
        STDMETHOD(GetConeAngles)(LPDWORD inside, LPDWORD outside) override;
        STDMETHOD(GetConeOrientation)(D3DVECTOR* v) override;
        STDMETHOD(GetConeOutsideVolume)(LPLONG volume) override;
        STDMETHOD(GetMaxDistance)(D3DVALUE* v) override;
        STDMETHOD(GetMinDistance)(D3DVALUE* v) override;
        STDMETHOD(GetMode)(LPDWORD mode) override;
        STDMETHOD(GetPosition)(D3DVECTOR* v) override;
        STDMETHOD(GetVelocity)(D3DVECTOR* v) override;
        STDMETHOD(SetAllParameters)(LPCDS3DBUFFER p, DWORD apply) override;
        STDMETHOD(SetConeAngles)(DWORD inside, DWORD outside, DWORD apply) override;
        STDMETHOD(SetConeOrientation)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override;
        STDMETHOD(SetConeOutsideVolume)(LONG volume, DWORD apply) override;
        STDMETHOD(SetMaxDistance)(D3DVALUE v, DWORD apply) override;
        STDMETHOD(SetMinDistance)(D3DVALUE v, DWORD apply) override;
        STDMETHOD(SetMode)(DWORD mode, DWORD apply) override;
        STDMETHOD(SetPosition)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override;
        STDMETHOD(SetVelocity)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override;
    };
    struct ListenerView final : IDirectSound3DListener {
        Obj* o = nullptr;
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(GetAllParameters)(LPDS3DLISTENER p) override;
        STDMETHOD(GetDistanceFactor)(D3DVALUE* v) override;
        STDMETHOD(GetDopplerFactor)(D3DVALUE* v) override;
        STDMETHOD(GetOrientation)(D3DVECTOR* front, D3DVECTOR* top) override;
        STDMETHOD(GetPosition)(D3DVECTOR* v) override;
        STDMETHOD(GetRolloffFactor)(D3DVALUE* v) override;
        STDMETHOD(GetVelocity)(D3DVECTOR* v) override;
        STDMETHOD(SetAllParameters)(LPCDS3DLISTENER p, DWORD apply) override;
        STDMETHOD(SetDistanceFactor)(D3DVALUE v, DWORD apply) override;
        STDMETHOD(SetDopplerFactor)(D3DVALUE v, DWORD apply) override;
        STDMETHOD(SetOrientation)(D3DVALUE xf, D3DVALUE yf, D3DVALUE zf, D3DVALUE xt, D3DVALUE yt, D3DVALUE zt, DWORD apply) override;
        STDMETHOD(SetPosition)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override;
        STDMETHOD(SetRolloffFactor)(D3DVALUE v, DWORD apply) override;
        STDMETHOD(SetVelocity)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) override;
        STDMETHOD(CommitDeferredSettings)() override;
    };
    struct NotifyView final : IDirectSoundNotify {
        Obj* o = nullptr;
        STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(SetNotificationPositions)(DWORD count, LPCDSBPOSITIONNOTIFY positions) override;
    };

    enum class Kind { device, buffer, primary };
    struct Obj {
        Impl* impl = nullptr;
        Kind kind = Kind::device;
        uint32_t refs = 1, children = 0;
        Obj* owner = nullptr;
        bool zombie = false;                      // released by the application but still has live children (device)
        BufferId id;
        uint32_t flags = 0;
        std::vector<uint8_t> mirror;              // the bytes Lock hands out (the application writes here; Unlock passes them to the Device)
        std::vector<Regions> locks;               // outstanding Locks (Windows allows several)
        bool has_pending_3d = false;
        Buffer3D pending_3d;
        DeviceView device_view;
        BufferView buffer_view;
        Buffer3DView buffer3d_view;
        ListenerView listener_view;
        NotifyView notify_view;
        Obj() {
            device_view.o = buffer_view.o = buffer3d_view.o = listener_view.o = notify_view.o = this;
        }
    };

    explicit Impl(DeviceProvider& p) : provider(p) {}

    DeviceProvider& provider;
    mutable std::mutex mutex;
    Device* device = nullptr;
    std::vector<Obj*> objects;
    uint32_t live_devices = 0;
    Obj* primary_obj = nullptr;
    Listener3D pending_listener;
    bool has_pending_listener = false;
    struct Counter {
        uint64_t calls = 0, failures = 0;
    };
    std::unordered_map<const char*, Counter> counters;   // keyed by the (literal) name of the method: no allocation per call
    std::string first_failure;
    uint64_t create_calls = 0;

    // ---- bookkeeping
    HRESULT note(const char* name, HRESULT h) {
        Counter& c = counters[name];
        ++c.calls;
        if (FAILED(h)) {
            ++c.failures;
            if (first_failure.empty()) {
                std::ostringstream s;
                s << name << " -> 0x" << std::hex << uint32_t(h);
                first_failure = s.str();
            }
        }
        return h;
    }
    Obj* new_obj(Kind kind) {
        Obj* o = new Obj;
        o->impl = this;
        o->kind = kind;
        objects.push_back(o);
        return o;
    }
    void destroy(Obj* o) {
        Obj* owner = o->owner;
        if (o->kind != Kind::device) {
            device->destroy_buffer(o->id);
            if (o == primary_obj) primary_obj = nullptr;
        }
        objects.erase(std::remove(objects.begin(), objects.end(), o), objects.end());
        delete o;
        if (owner) {
            --owner->children;
            maybe_free_device(owner);
        }
        if (device && objects.empty() && live_devices == 0) {
            device->set_notify_callback(nullptr, nullptr);
            provider.release();
            device = nullptr;
        }
    }
    void maybe_free_device(Obj* d) {
        if (d->zombie && d->children == 0) {
            if (live_devices > 0) --live_devices;
            destroy(d);
        }
    }
    ULONG add_ref(Obj& o) { return ++o.refs; }
    ULONG release(Obj* o) {
        if (o->refs == 0) return 0;
        const uint32_t refs = --o->refs;
        if (refs == 0) {
            if (o->kind == Kind::device && o->children > 0) {
                o->zombie = true;   // buffers keep the device alive, as in DirectSound
            } else {
                if (o->kind == Kind::device && live_devices > 0) --live_devices;
                destroy(o);
            }
        }
        return refs;
    }

    static void on_notify(void*, uint64_t token) { SetEvent(reinterpret_cast<HANDLE>(uintptr_t(token))); }

    // ---- WAVEFORMATEX
    // Returns S_OK, or the error to give the application. `ignored` (SetFormat only): the format tag is unknown to us but valid in shape; Windows
    // accepts such a SetFormat and does nothing.
    static HRESULT read_format(const WAVEFORMATEX* w, WaveFormat& out, bool for_set_format, bool* ignored = nullptr) {
        if (!w) return E_INVALIDARG;
        const HRESULT bad = E_INVALIDARG;   // measured: malformed formats are E_INVALIDARG, for SetFormat as well as for CreateSoundBuffer
        out = WaveFormat{};
        out.channels = w->nChannels;
        out.sample_rate = w->nSamplesPerSec;
        out.bits = w->wBitsPerSample;
        if (w->wFormatTag == WAVE_FORMAT_PCM) {
            out.type = SampleType::pcm;
        } else if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            out.type = SampleType::ieee_float;
        } else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            if (w->cbSize < 22) return DSERR_BADFORMAT;
            const auto* e = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
            out.channel_mask = e->dwChannelMask;
            if (guid_eq(e->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) out.type = SampleType::pcm;
            else if (guid_eq(e->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) out.type = SampleType::ieee_float;
            else return DSERR_BADFORMAT;
        } else {
            // Unknown tag. Measured: SetFormat on the primary buffer returns S_OK and ignores it, CreateSoundBuffer returns E_NOTIMPL.
            if (for_set_format && ignored) {
                *ignored = true;
                return S_OK;
            }
            return E_NOTIMPL;
        }
        if (w->nChannels == 0 || w->wBitsPerSample == 0 || w->wBitsPerSample % 8 != 0) return bad;
        if (w->nBlockAlign != w->nChannels * (w->wBitsPerSample / 8u)) return bad;
        // Measured: more than two channels need WAVEFORMATEXTENSIBLE; a plain format with more is E_INVALIDARG for a buffer and DSERR_ALLOCATED for SetFormat.
        if (w->nChannels > 2 && w->wFormatTag != WAVE_FORMAT_EXTENSIBLE) return for_set_format ? DSERR_ALLOCATED : bad;
        return S_OK;
    }
    // GetFormat: the blob Windows would write. Plain WAVEFORMATEX (18 bytes with cbSize) or WAVEFORMATEXTENSIBLE (40 bytes) above 2 channels / 16 bits.
    static HRESULT write_format(const WaveFormat& f, LPWAVEFORMATEX dst, DWORD allocated, LPDWORD written) {
        WAVEFORMATEXTENSIBLE blob = {};
        const bool extensible = f.channels > 2 || (f.channel_mask && f.channel_mask != speaker::default_mask(f.channels)) || (f.bits > 16);
        blob.Format.wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE : (f.type == SampleType::ieee_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
        blob.Format.nChannels = f.channels;
        blob.Format.nSamplesPerSec = f.sample_rate;
        blob.Format.nAvgBytesPerSec = f.sample_rate * f.frame_bytes();
        blob.Format.nBlockAlign = WORD(f.frame_bytes());
        blob.Format.wBitsPerSample = f.bits;
        DWORD size = 18;
        if (extensible) {
            blob.Format.cbSize = 22;
            blob.Samples.wValidBitsPerSample = f.bits;
            blob.dwChannelMask = f.effective_mask();
            blob.SubFormat = f.type == SampleType::ieee_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
            size = 40;
        }
        const DWORD n = std::min<DWORD>(size, allocated);
        if (dst && n) std::memcpy(dst, &blob, n);
        if (written) *written = dst ? n : size;
        return S_OK;
    }

    // ---- interface lookup
    HRESULT query_interface(Obj& o, REFIID riid, LPVOID* ppv) {
        if (!ppv) return DS_E_POINTER;
        *ppv = nullptr;
        auto grant = [&](IUnknown* view) {
            add_ref(o);
            *ppv = view;
            return S_OK;
        };
        if (IsEqualIID(riid, IID_IUnknown)) return o.kind == Kind::device ? grant(&o.device_view) : grant(&o.buffer_view);
        if (o.kind == Kind::device) {
            if (IsEqualIID(riid, IID_IDirectSound)) return grant(&o.device_view);
            return E_NOINTERFACE;   // IDirectSound8, IKsPropertySet and the rest: not offered
        }
        if (IsEqualIID(riid, IID_IDirectSoundBuffer)) return grant(&o.buffer_view);
        if (IsEqualIID(riid, IID_IDirectSound3DBuffer) && (o.flags & buffer_flag::ctrl_3d) && o.kind == Kind::buffer) return grant(&o.buffer3d_view);
        if (IsEqualIID(riid, IID_IDirectSound3DListener) && (o.flags & buffer_flag::ctrl_3d) && o.kind == Kind::primary) return grant(&o.listener_view);
        if (IsEqualIID(riid, IID_IDirectSoundNotify) && (o.flags & buffer_flag::ctrl_position_notify) && o.kind == Kind::buffer) return grant(&o.notify_view);
        return E_NOINTERFACE;
    }

    // ---- IDirectSound
    HRESULT create_sound_buffer(Obj& dev, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer) {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!d) return E_INVALIDARG;
        if (d->dwSize != sizeof(DSBUFFERDESC1) && d->dwSize != sizeof(DSBUFFERDESC)) return E_INVALIDARG;
        BufferDesc bd;
        bd.flags = d->dwFlags;
        bd.bytes = d->dwBufferBytes;
        if (!(d->dwFlags & DSBCAPS_PRIMARYBUFFER)) {
            if (!d->lpwfxFormat) return E_INVALIDARG;
            if (const HRESULT h = read_format(d->lpwfxFormat, bd.format, false); FAILED(h)) return h;
            bd.has_format = true;
        } else if (d->lpwfxFormat) {
            return E_INVALIDARG;
        }
        const CreateResult r = device->create_buffer(bd);
        if (r.hr < 0) return as_hresult(r.hr);
        if (r.existing_primary && primary_obj) {
            add_ref(*primary_obj);
            *out = &primary_obj->buffer_view;
            return S_OK;
        }
        Obj* o = new_obj((d->dwFlags & DSBCAPS_PRIMARYBUFFER) ? Kind::primary : Kind::buffer);
        o->id = r.id;
        if (d->dwFlags & DSBCAPS_PRIMARYBUFFER) {
            o->flags = d->dwFlags;
            primary_obj = o;
        } else {
            o->flags = d->dwFlags & ~DWORD(DSBCAPS_PRIMARYBUFFER);
            BufferCaps caps;
            device->get_caps(r.id, caps);
            o->mirror.assign(caps.bytes, bd.format.bits == 8 ? uint8_t(0x80) : uint8_t(0));
        }
        o->owner = &dev;
        ++dev.children;
        *out = &o->buffer_view;
        return S_OK;
    }

    HRESULT duplicate_buffer(Obj& dev, LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER* out) {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        Obj* s = nullptr;
        for (Obj* o : objects)
            if (o->kind == Kind::buffer && static_cast<IDirectSoundBuffer*>(&o->buffer_view) == src) s = o;
        if (!s) return E_INVALIDARG;
        const CreateResult r = device->duplicate_buffer(s->id);
        if (r.hr < 0) return as_hresult(r.hr);
        Obj* o = new_obj(Kind::buffer);
        o->id = r.id;
        o->flags = s->flags;
        o->mirror = s->mirror;
        o->owner = &dev;
        ++dev.children;
        *out = &o->buffer_view;
        return S_OK;
    }

    HRESULT get_device_caps(LPDSCAPS caps) {
        if (!caps || caps->dwSize != sizeof(DSCAPS)) return E_INVALIDARG;
        const Caps c = device->caps();
        DSCAPS out = {};
        out.dwSize = sizeof(DSCAPS);
        out.dwFlags = c.flags;
        out.dwMinSecondarySampleRate = c.min_secondary_rate;
        out.dwMaxSecondarySampleRate = c.max_secondary_rate;
        out.dwPrimaryBuffers = c.primary_buffers;
        *caps = out;
        return S_OK;
    }

    // ---- IDirectSoundBuffer
    HRESULT buffer_get_caps(Obj& o, LPDSBCAPS caps) {
        if (!caps || caps->dwSize != sizeof(DSBCAPS)) return E_INVALIDARG;
        BufferCaps c;
        if (const Hr h = device->get_caps(o.id, c); h < 0) return as_hresult(h);
        DSBCAPS out = {};
        out.dwSize = sizeof(DSBCAPS);
        out.dwFlags = c.flags;
        out.dwBufferBytes = c.bytes;
        *caps = out;
        return S_OK;
    }

    HRESULT lock(Obj& o, DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) {
        if (!p1 || !n1) return E_INVALIDARG;
        if (o.kind != Kind::buffer || o.mirror.empty()) return DSERR_INVALIDCALL;
        *p1 = nullptr;
        *n1 = 0;
        if (p2) *p2 = nullptr;
        if (n2) *n2 = 0;
        Regions r;
        if (const Hr h = device->lock(o.id, offset, bytes, flags, r); h < 0) return as_hresult(h);
        if (o.locks.size() >= 32) return DSERR_INVALIDCALL;
        if (r.bytes2 && (!p2 || !n2)) r.bytes2 = 0;   // measured: without a second pointer Windows still succeeds (the wrapped part is simply not offered)
        o.locks.push_back(r);
        *p1 = o.mirror.data() + r.offset1;
        *n1 = r.bytes1;
        if (p2) *p2 = r.bytes2 ? o.mirror.data() + r.offset2 : nullptr;
        if (n2) *n2 = r.bytes2;
        return S_OK;
    }

    HRESULT unlock(Obj& o, LPVOID pv1, DWORD cb1, LPVOID pv2, DWORD cb2) {
        if (o.kind != Kind::buffer || o.locks.empty()) return E_INVALIDARG;
        uint8_t* base = o.mirror.data();
        size_t match = o.locks.size();
        for (size_t i = 0; i < o.locks.size(); ++i) {   // the oldest outstanding Lock that these pointers belong to
            const Regions& c = o.locks[i];
            if (pv1 != base + c.offset1 || cb1 > c.bytes1) continue;
            if (cb2 && (pv2 != base + c.offset2 || cb2 > c.bytes2)) continue;
            match = i;
            break;
        }
        if (match == o.locks.size()) return E_INVALIDARG;
        const Regions r = o.locks[match];
        o.locks.erase(o.locks.begin() + std::ptrdiff_t(match));
        Regions written = r;
        written.bytes1 = cb1;
        written.bytes2 = cb2;
        return as_hresult(device->unlock(o.id, written, Bytes(static_cast<const std::byte*>(pv1), cb1), Bytes(static_cast<const std::byte*>(pv2), cb2)));
    }

    // ---- 3D buffer / listener
    static Vec3 vec(const D3DVECTOR& v) { return {v.x, v.y, v.z}; }
    static D3DVECTOR vec(const Vec3& v) { return {v.x, v.y, v.z}; }

    HRESULT read_3d_buffer(LPCDS3DBUFFER p, Buffer3D& b) {
        if (!p || p->dwSize != sizeof(DS3DBUFFER)) return E_INVALIDARG;
        b.position = vec(p->vPosition);
        b.velocity = vec(p->vVelocity);
        b.inside_cone_angle = p->dwInsideConeAngle;
        b.outside_cone_angle = p->dwOutsideConeAngle;
        b.cone_orientation = vec(p->vConeOrientation);
        b.cone_outside_volume = p->lConeOutsideVolume;
        b.min_distance = p->flMinDistance;
        b.max_distance = p->flMaxDistance;
        if (p->dwMode > 2) return E_INVALIDARG;
        b.mode = Mode3D(p->dwMode);
        return S_OK;
    }
    HRESULT apply_3d(Obj& o, const Buffer3D& b, DWORD apply) {
        if (apply == DS3D_DEFERRED) {
            if (!o.has_pending_3d) device->get_buffer_3d(o.id, o.pending_3d);
            o.pending_3d = b;
            o.has_pending_3d = true;
            return S_OK;
        }
        o.has_pending_3d = false;
        return as_hresult(device->set_buffer_3d(o.id, b));
    }
    // The 3D parameters a setter starts from: the deferred copy when one is pending.
    HRESULT current_3d(Obj& o, Buffer3D& b) {
        if (o.has_pending_3d) {
            b = o.pending_3d;
            return S_OK;
        }
        return as_hresult(device->get_buffer_3d(o.id, b));
    }
    template <class F>
    HRESULT with_3d_get(Obj& o, F&& f) {
        Buffer3D b;
        if (const HRESULT h = current_3d(o, b); FAILED(h)) return h;
        return f(b);
    }
    template <class F>
    HRESULT with_3d_set(Obj& o, DWORD apply, F&& f) {
        Buffer3D b;
        if (const HRESULT h = current_3d(o, b); FAILED(h)) return h;
        if (const HRESULT h = f(b); FAILED(h)) return h;
        return apply_3d(o, b, apply);
    }

    HRESULT apply_listener(const Listener3D& l, DWORD apply) {
        if (apply == DS3D_DEFERRED) {
            pending_listener = l;
            has_pending_listener = true;
            return S_OK;
        }
        has_pending_listener = false;
        return as_hresult(device->set_listener(l));
    }
    Listener3D current_listener() { return has_pending_listener ? pending_listener : device->listener(); }
    template <class F>
    HRESULT listener_set(DWORD apply, F&& f) {
        Listener3D l = current_listener();
        f(l);
        return apply_listener(l, apply);
    }

    HRESULT commit_deferred() {
        HRESULT result = S_OK;
        if (has_pending_listener) {
            has_pending_listener = false;
            result = as_hresult(device->set_listener(pending_listener));
        }
        for (Obj* obj : objects)
            if (obj->has_pending_3d) {
                obj->has_pending_3d = false;
                if (const Hr h = device->set_buffer_3d(obj->id, obj->pending_3d); h < 0) result = as_hresult(h);
            }
        return result;
    }

    HRESULT create(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer) {
        ++create_calls;
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (guid && !IsEqualGUID(*guid, DSDEVID_DefaultPlayback) && !IsEqualGUID(*guid, DSDEVID_DefaultVoicePlayback)) {
            bool zero = true;
            const auto* g = reinterpret_cast<const uint8_t*>(guid);
            for (int i = 0; i < 16; ++i) zero = zero && g[i] == 0;
            if (!zero) return DSERR_NODRIVER;   // the only device is the default playback device
        }
        if (!device) {
            device = provider.acquire();
            if (!device) return DSERR_NODRIVER;
        }
        if (live_devices == 0) {
            device->reset();
            device->set_notify_callback(&Impl::on_notify, nullptr);
        }
        Obj* o = new_obj(Kind::device);
        ++live_devices;
        *out = &o->device_view;
        return S_OK;
    }
};

// ---- the COM methods: lock, count, forward ---------------------------------------------------------------------------------------------------

#define LOCKED(name, expr)                                    \
    std::lock_guard<std::mutex> lock(o->impl->mutex);         \
    return o->impl->note(name, (expr))

using Impl = Facade::Impl;

// QueryInterface / AddRef / Release of every view are the same: one identity.
#define IUNKNOWN_METHODS(View)                                                                    \
    HRESULT STDMETHODCALLTYPE Impl::View::QueryInterface(REFIID riid, LPVOID* ppv) {              \
        LOCKED(#View "::QueryInterface", o->impl->query_interface(*o, riid, ppv));                \
    }                                                                                             \
    ULONG STDMETHODCALLTYPE Impl::View::AddRef() {                                                \
        std::lock_guard<std::mutex> lock(o->impl->mutex);                                         \
        return o->impl->add_ref(*o);                                                              \
    }                                                                                             \
    ULONG STDMETHODCALLTYPE Impl::View::Release() {                                               \
        Impl* impl = o->impl;                                                                     \
        std::lock_guard<std::mutex> lock(impl->mutex);                                            \
        return impl->release(o);                                                                  \
    }
IUNKNOWN_METHODS(DeviceView)
IUNKNOWN_METHODS(BufferView)
IUNKNOWN_METHODS(Buffer3DView)
IUNKNOWN_METHODS(ListenerView)
IUNKNOWN_METHODS(NotifyView)

// IDirectSound
HRESULT Impl::DeviceView::CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN outer) { LOCKED("IDirectSound::CreateSoundBuffer", o->impl->create_sound_buffer(*o, desc, out, outer)); }
HRESULT Impl::DeviceView::GetCaps(LPDSCAPS caps) { LOCKED("IDirectSound::GetCaps", o->impl->get_device_caps(caps)); }
HRESULT Impl::DeviceView::DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER* duplicate) { LOCKED("IDirectSound::DuplicateSoundBuffer", o->impl->duplicate_buffer(*o, original, duplicate)); }
HRESULT Impl::DeviceView::SetCooperativeLevel(HWND hwnd, DWORD level) {
    LOCKED("IDirectSound::SetCooperativeLevel", !IsWindow(hwnd) ? E_INVALIDARG : as_hresult(o->impl->device->set_cooperative_level(level)));
}
HRESULT Impl::DeviceView::Compact() { LOCKED("IDirectSound::Compact", S_OK); }
HRESULT Impl::DeviceView::GetSpeakerConfig(LPDWORD config) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    if (!config) return o->impl->note("IDirectSound::GetSpeakerConfig", E_INVALIDARG);
    const uint32_t channels = o->impl->device->output().channels;
    const DWORD c = channels >= 8 ? DSSPEAKER_7POINT1_SURROUND : channels >= 6 ? DSSPEAKER_5POINT1 : channels >= 4 ? DSSPEAKER_QUAD : DSSPEAKER_STEREO;
    *config = c | (c == DSSPEAKER_STEREO ? (DSSPEAKER_GEOMETRY_WIDE << 16) : 0u);
    return o->impl->note("IDirectSound::GetSpeakerConfig", S_OK);
}
HRESULT Impl::DeviceView::SetSpeakerConfig(DWORD) { LOCKED("IDirectSound::SetSpeakerConfig", S_OK); }   // accepted: the output follows the device and the primary format
HRESULT Impl::DeviceView::Initialize(LPCGUID) { LOCKED("IDirectSound::Initialize", DSERR_ALREADYINITIALIZED); }

// IDirectSoundBuffer
HRESULT Impl::BufferView::GetCaps(LPDSBCAPS caps) { LOCKED("IDirectSoundBuffer::GetCaps", o->impl->buffer_get_caps(*o, caps)); }
HRESULT Impl::BufferView::GetCurrentPosition(LPDWORD play, LPDWORD write) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    uint32_t p = 0, w = 0;
    const Hr h = o->impl->device->get_current_position(o->id, play ? &p : nullptr, write ? &w : nullptr);
    if (h >= 0) {
        if (play) *play = p;
        if (write) *write = w;
    }
    return o->impl->note("IDirectSoundBuffer::GetCurrentPosition", as_hresult(h));
}
HRESULT Impl::BufferView::GetFormat(LPWAVEFORMATEX format, DWORD allocated, LPDWORD written) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    WaveFormat f;
    HRESULT r = as_hresult(o->impl->device->get_format(o->id, f));
    if (SUCCEEDED(r)) r = (!format && !written) ? E_INVALIDARG : Impl::write_format(f, format, allocated, written);
    return o->impl->note("IDirectSoundBuffer::GetFormat", r);
}
HRESULT Impl::BufferView::GetVolume(LPLONG volume) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    int32_t v = 0;
    HRESULT r = !volume ? E_INVALIDARG : as_hresult(o->impl->device->get_volume(o->id, v));
    if (SUCCEEDED(r)) *volume = v;
    return o->impl->note("IDirectSoundBuffer::GetVolume", r);
}
HRESULT Impl::BufferView::GetPan(LPLONG pan) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    int32_t v = 0;
    HRESULT r = !pan ? E_INVALIDARG : as_hresult(o->impl->device->get_pan(o->id, v));
    if (SUCCEEDED(r)) *pan = v;
    return o->impl->note("IDirectSoundBuffer::GetPan", r);
}
HRESULT Impl::BufferView::GetFrequency(LPDWORD frequency) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    uint32_t v = 0;
    HRESULT r = !frequency ? E_INVALIDARG : as_hresult(o->impl->device->get_frequency(o->id, v));
    if (SUCCEEDED(r)) *frequency = v;
    return o->impl->note("IDirectSoundBuffer::GetFrequency", r);
}
HRESULT Impl::BufferView::GetStatus(LPDWORD status) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    uint32_t v = 0;
    HRESULT r = !status ? E_INVALIDARG : as_hresult(o->impl->device->get_status(o->id, v));
    if (SUCCEEDED(r)) *status = v;
    return o->impl->note("IDirectSoundBuffer::GetStatus", r);
}
HRESULT Impl::BufferView::Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) { LOCKED("IDirectSoundBuffer::Initialize", DSERR_ALREADYINITIALIZED); }
HRESULT Impl::BufferView::Lock(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) {
    LOCKED("IDirectSoundBuffer::Lock", o->impl->lock(*o, offset, bytes, p1, n1, p2, n2, flags));
}
HRESULT Impl::BufferView::Play(DWORD reserved1, DWORD priority, DWORD flags) {
    LOCKED("IDirectSoundBuffer::Play", reserved1 != 0 ? E_INVALIDARG : as_hresult(o->impl->device->play(o->id, priority, flags)));
}
HRESULT Impl::BufferView::SetCurrentPosition(DWORD position) { LOCKED("IDirectSoundBuffer::SetCurrentPosition", as_hresult(o->impl->device->set_current_position(o->id, position))); }
HRESULT Impl::BufferView::SetFormat(LPCWAVEFORMATEX format) {
    std::unique_lock<std::mutex> lock(o->impl->mutex);
    WaveFormat f;
    bool ignored = false;
    HRESULT r = Impl::read_format(format, f, true, &ignored);
    Device* device = o->impl->device;
    const BufferId id = o->id;
    Impl* impl = o->impl;
    lock.unlock();   // Device::set_format may reopen the output (it calls back into the engine): never under the facade lock
    if (SUCCEEDED(r) && !ignored) r = as_hresult(device->set_format(id, f));
    lock.lock();
    return impl->note("IDirectSoundBuffer::SetFormat", r);
}
HRESULT Impl::BufferView::SetVolume(LONG volume) { LOCKED("IDirectSoundBuffer::SetVolume", as_hresult(o->impl->device->set_volume(o->id, volume))); }
HRESULT Impl::BufferView::SetPan(LONG pan) { LOCKED("IDirectSoundBuffer::SetPan", as_hresult(o->impl->device->set_pan(o->id, pan))); }
HRESULT Impl::BufferView::SetFrequency(DWORD frequency) { LOCKED("IDirectSoundBuffer::SetFrequency", as_hresult(o->impl->device->set_frequency(o->id, frequency))); }
HRESULT Impl::BufferView::Stop() { LOCKED("IDirectSoundBuffer::Stop", as_hresult(o->impl->device->stop(o->id))); }
HRESULT Impl::BufferView::Unlock(LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) { LOCKED("IDirectSoundBuffer::Unlock", o->impl->unlock(*o, p1, n1, p2, n2)); }
HRESULT Impl::BufferView::Restore() { LOCKED("IDirectSoundBuffer::Restore", as_hresult(o->impl->device->restore(o->id))); }

// IDirectSound3DBuffer
HRESULT Impl::Buffer3DView::GetAllParameters(LPDS3DBUFFER p) {
    LOCKED("IDirectSound3DBuffer::GetAllParameters", (!p || p->dwSize != sizeof(DS3DBUFFER)) ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) {
        p->vPosition = Impl::vec(b.position);
        p->vVelocity = Impl::vec(b.velocity);
        p->dwInsideConeAngle = b.inside_cone_angle;
        p->dwOutsideConeAngle = b.outside_cone_angle;
        p->vConeOrientation = Impl::vec(b.cone_orientation);
        p->lConeOutsideVolume = b.cone_outside_volume;
        p->flMinDistance = b.min_distance;
        p->flMaxDistance = b.max_distance;
        p->dwMode = DWORD(b.mode);
        return S_OK;
    }));
}
HRESULT Impl::Buffer3DView::GetConeAngles(LPDWORD inside, LPDWORD outside) {
    LOCKED("IDirectSound3DBuffer::GetConeAngles", (!inside || !outside) ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) {
        *inside = b.inside_cone_angle;
        *outside = b.outside_cone_angle;
        return S_OK;
    }));
}
HRESULT Impl::Buffer3DView::GetConeOrientation(D3DVECTOR* v) {
    LOCKED("IDirectSound3DBuffer::GetConeOrientation", !v ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) { *v = Impl::vec(b.cone_orientation); return S_OK; }));
}
HRESULT Impl::Buffer3DView::GetConeOutsideVolume(LPLONG volume) {
    LOCKED("IDirectSound3DBuffer::GetConeOutsideVolume", !volume ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) { *volume = b.cone_outside_volume; return S_OK; }));
}
HRESULT Impl::Buffer3DView::GetMaxDistance(D3DVALUE* v) {
    LOCKED("IDirectSound3DBuffer::GetMaxDistance", !v ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) { *v = b.max_distance; return S_OK; }));
}
HRESULT Impl::Buffer3DView::GetMinDistance(D3DVALUE* v) {
    LOCKED("IDirectSound3DBuffer::GetMinDistance", !v ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) { *v = b.min_distance; return S_OK; }));
}
HRESULT Impl::Buffer3DView::GetMode(LPDWORD mode) {
    LOCKED("IDirectSound3DBuffer::GetMode", !mode ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) { *mode = DWORD(b.mode); return S_OK; }));
}
HRESULT Impl::Buffer3DView::GetPosition(D3DVECTOR* v) {
    LOCKED("IDirectSound3DBuffer::GetPosition", !v ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) { *v = Impl::vec(b.position); return S_OK; }));
}
HRESULT Impl::Buffer3DView::GetVelocity(D3DVECTOR* v) {
    LOCKED("IDirectSound3DBuffer::GetVelocity", !v ? E_INVALIDARG : o->impl->with_3d_get(*o, [&](const Buffer3D& b) { *v = Impl::vec(b.velocity); return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetAllParameters(LPCDS3DBUFFER p, DWORD apply) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    Buffer3D b;
    HRESULT r = o->impl->read_3d_buffer(p, b);
    if (SUCCEEDED(r)) r = o->impl->apply_3d(*o, b, apply);
    return o->impl->note("IDirectSound3DBuffer::SetAllParameters", r);
}
HRESULT Impl::Buffer3DView::SetConeAngles(DWORD inside, DWORD outside, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetConeAngles", o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.inside_cone_angle = inside; b.outside_cone_angle = outside; return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetConeOrientation(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetConeOrientation", o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.cone_orientation = {x, y, z}; return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetConeOutsideVolume(LONG volume, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetConeOutsideVolume", o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.cone_outside_volume = volume; return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetMaxDistance(D3DVALUE v, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetMaxDistance", o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.max_distance = v; return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetMinDistance(D3DVALUE v, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetMinDistance", o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.min_distance = v; return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetMode(DWORD mode, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetMode", mode > 2 ? E_INVALIDARG : o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.mode = Mode3D(mode); return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetPosition", o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.position = {x, y, z}; return S_OK; }));
}
HRESULT Impl::Buffer3DView::SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
    LOCKED("IDirectSound3DBuffer::SetVelocity", o->impl->with_3d_set(*o, apply, [&](Buffer3D& b) { b.velocity = {x, y, z}; return S_OK; }));
}

// IDirectSound3DListener
HRESULT Impl::ListenerView::GetAllParameters(LPDS3DLISTENER p) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    if (!p || p->dwSize != sizeof(DS3DLISTENER)) return o->impl->note("IDirectSound3DListener::GetAllParameters", E_INVALIDARG);
    const Listener3D l = o->impl->current_listener();
    p->vPosition = Impl::vec(l.position);
    p->vVelocity = Impl::vec(l.velocity);
    p->vOrientFront = Impl::vec(l.front);
    p->vOrientTop = Impl::vec(l.top);
    p->flDistanceFactor = l.distance_factor;
    p->flRolloffFactor = l.rolloff_factor;
    p->flDopplerFactor = l.doppler_factor;
    return o->impl->note("IDirectSound3DListener::GetAllParameters", S_OK);
}
#define LISTENER_GET(name, param, expr)                                                       \
    HRESULT Impl::ListenerView::name(param) {                                                 \
        std::lock_guard<std::mutex> lock(o->impl->mutex);                                     \
        if (!v) return o->impl->note("IDirectSound3DListener::" #name, E_INVALIDARG);         \
        const Listener3D l = o->impl->current_listener();                                     \
        *v = (expr);                                                                          \
        return o->impl->note("IDirectSound3DListener::" #name, S_OK);                         \
    }
LISTENER_GET(GetDistanceFactor, D3DVALUE* v, l.distance_factor)
LISTENER_GET(GetDopplerFactor, D3DVALUE* v, l.doppler_factor)
LISTENER_GET(GetPosition, D3DVECTOR* v, Impl::vec(l.position))
LISTENER_GET(GetRolloffFactor, D3DVALUE* v, l.rolloff_factor)
LISTENER_GET(GetVelocity, D3DVECTOR* v, Impl::vec(l.velocity))
HRESULT Impl::ListenerView::GetOrientation(D3DVECTOR* front, D3DVECTOR* top) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    if (!front || !top) return o->impl->note("IDirectSound3DListener::GetOrientation", E_INVALIDARG);
    const Listener3D l = o->impl->current_listener();
    *front = Impl::vec(l.front);
    *top = Impl::vec(l.top);
    return o->impl->note("IDirectSound3DListener::GetOrientation", S_OK);
}
HRESULT Impl::ListenerView::SetAllParameters(LPCDS3DLISTENER p, DWORD apply) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    if (!p || p->dwSize != sizeof(DS3DLISTENER)) return o->impl->note("IDirectSound3DListener::SetAllParameters", E_INVALIDARG);
    Listener3D l;
    l.position = Impl::vec(p->vPosition);
    l.velocity = Impl::vec(p->vVelocity);
    l.front = Impl::vec(p->vOrientFront);
    l.top = Impl::vec(p->vOrientTop);
    l.distance_factor = p->flDistanceFactor;
    l.rolloff_factor = p->flRolloffFactor;
    l.doppler_factor = p->flDopplerFactor;
    return o->impl->note("IDirectSound3DListener::SetAllParameters", o->impl->apply_listener(l, apply));
}
HRESULT Impl::ListenerView::SetDistanceFactor(D3DVALUE v, DWORD apply) {
    LOCKED("IDirectSound3DListener::SetDistanceFactor", o->impl->listener_set(apply, [&](Listener3D& l) { l.distance_factor = v; }));
}
HRESULT Impl::ListenerView::SetDopplerFactor(D3DVALUE v, DWORD apply) {
    LOCKED("IDirectSound3DListener::SetDopplerFactor", o->impl->listener_set(apply, [&](Listener3D& l) { l.doppler_factor = v; }));
}
HRESULT Impl::ListenerView::SetOrientation(D3DVALUE xf, D3DVALUE yf, D3DVALUE zf, D3DVALUE xt, D3DVALUE yt, D3DVALUE zt, DWORD apply) {
    LOCKED("IDirectSound3DListener::SetOrientation", o->impl->listener_set(apply, [&](Listener3D& l) { l.front = {xf, yf, zf}; l.top = {xt, yt, zt}; }));
}
HRESULT Impl::ListenerView::SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
    LOCKED("IDirectSound3DListener::SetPosition", o->impl->listener_set(apply, [&](Listener3D& l) { l.position = {x, y, z}; }));
}
HRESULT Impl::ListenerView::SetRolloffFactor(D3DVALUE v, DWORD apply) {
    LOCKED("IDirectSound3DListener::SetRolloffFactor", o->impl->listener_set(apply, [&](Listener3D& l) { l.rolloff_factor = v; }));
}
HRESULT Impl::ListenerView::SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply) {
    LOCKED("IDirectSound3DListener::SetVelocity", o->impl->listener_set(apply, [&](Listener3D& l) { l.velocity = {x, y, z}; }));
}
HRESULT Impl::ListenerView::CommitDeferredSettings() { LOCKED("IDirectSound3DListener::CommitDeferredSettings", o->impl->commit_deferred()); }

// IDirectSoundNotify
HRESULT Impl::NotifyView::SetNotificationPositions(DWORD count, LPCDSBPOSITIONNOTIFY positions) {
    std::lock_guard<std::mutex> lock(o->impl->mutex);
    if (count > 0 && !positions) return o->impl->note("IDirectSoundNotify::SetNotificationPositions", E_INVALIDARG);
    std::vector<Notify> list;
    for (DWORD i = 0; i < count; ++i) list.push_back({positions[i].dwOffset, uint64_t(reinterpret_cast<uintptr_t>(positions[i].hEventNotify))});
    return o->impl->note("IDirectSoundNotify::SetNotificationPositions", as_hresult(o->impl->device->set_notifications(o->id, list)));
}

// ---- Facade ---------------------------------------------------------------------------------------------------------------------------------

Facade::Facade(DeviceProvider& provider) : impl_(new Impl(provider)) {}
Facade::~Facade() {
    // Objects the application never released are still alive; the facade cannot outlive them in practice (the dll keeps it for the process).
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (Impl::Obj* o : impl_->objects) delete o;
    impl_->objects.clear();
    if (impl_->device) {
        impl_->device->set_notify_callback(nullptr, nullptr);
        impl_->provider.release();
    }
}

HRESULT Facade::create(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->note("DirectSoundCreate", impl_->create(guid, out, outer));
}

uint32_t Facade::live_objects() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return uint32_t(impl_->objects.size());
}

std::string Facade::report() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::ostringstream s;
    s << "DirectSoundCreate calls: " << impl_->create_calls << ", live objects: " << impl_->objects.size();
    std::map<std::string, Impl::Counter> sorted;
    for (const auto& [name, c] : impl_->counters) {
        Impl::Counter& t = sorted[name];
        t.calls += c.calls;
        t.failures += c.failures;
    }
    for (const auto& [name, c] : sorted) {
        s << "\n  " << name << ": " << c.calls << " calls";
        if (c.failures) s << ", " << c.failures << " failed";
    }
    if (!impl_->first_failure.empty()) s << "\n  first failure: " << impl_->first_failure;
    return s.str();
}

}  // namespace nue::dsound::com
