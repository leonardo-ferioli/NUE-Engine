// WASAPI render stream, device enumeration and hot-plug handling.
#include <initguid.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <mutex>
#include <thread>

#include "nue_win/audio.hpp"
#include "nue_win/power.hpp"

#pragma comment(lib, "ole32.lib")

namespace nue::win {

namespace {

constexpr REFERENCE_TIME kHnsPerSecond = 10000000;

// PKEY_AudioEngine_DeviceFormat: the format set in Windows Sound settings ("Default Format"); it is the one exclusive mode is guaranteed to take.
const PROPERTYKEY kDeviceFormatKey = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

template <class T> struct ComRelease { void operator()(T* p) const { if (p) p->Release(); } };
template <class T> using Com = std::unique_ptr<T, ComRelease<T>>;
template <class T> struct ComPtrOut {
    Com<T>& target; T* raw = nullptr;
    explicit ComPtrOut(Com<T>& t) : target(t) {}
    ~ComPtrOut() { target.reset(raw); }
    operator T**() { return &raw; }
    operator void**() { return reinterpret_cast<void**>(&raw); }
};
template <class T> ComPtrOut<T> out(Com<T>& c) { return ComPtrOut<T>(c); }

struct ScopedCom {
    HRESULT hr;
    ScopedCom() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

uint32_t default_channel_mask(uint16_t ch) {
    switch (ch) {
        case 1: return SPEAKER_FRONT_CENTER;
        case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
        default: return 0;
    }
}

WAVEFORMATEXTENSIBLE make_wfx(const AudioFormat& f) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = f.channels;
    w.Format.nSamplesPerSec = f.sample_rate;
    w.Format.wBitsPerSample = WORD(sample_bytes(f.format) * 8);
    w.Format.nBlockAlign = WORD(f.channels * sample_bytes(f.format));
    w.Format.nAvgBytesPerSec = w.Format.nSamplesPerSec * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = w.Format.wBitsPerSample;
    w.dwChannelMask = default_channel_mask(f.channels);
    w.SubFormat = f.format == SampleFormat::F32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

bool parse_wfx(const WAVEFORMATEX* w, AudioFormat& f, uint32_t* mask = nullptr) {
    if (!w) return false;
    bool is_float = false, is_pcm = false;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) is_float = true;
    else if (w->wFormatTag == WAVE_FORMAT_PCM) is_pcm = true;
    else if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        is_float = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        is_pcm = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
        if (mask) *mask = x->dwChannelMask;
    }
    f.sample_rate = w->nSamplesPerSec;
    f.channels = w->nChannels;
    if (is_float && w->wBitsPerSample == 32) f.format = SampleFormat::F32;
    else if (is_pcm && w->wBitsPerSample == 16) f.format = SampleFormat::S16;
    else if (is_pcm && w->wBitsPerSample == 24) f.format = SampleFormat::S24;
    else if (is_pcm && w->wBitsPerSample == 32) f.format = SampleFormat::S32;
    else return false;
    return f.channels > 0 && f.sample_rate > 0;
}

REFERENCE_TIME frames_to_hns(uint32_t frames, uint32_t rate) { return REFERENCE_TIME(double(frames) * double(kHnsPerSecond) / double(rate) + 0.5); }
double hns_to_ms(REFERENCE_TIME h) { return double(h) / 10000.0; }

std::string device_name_of(IMMDevice* dev) {
    Com<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, out(props)))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = to_utf8(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

std::string device_id_of(IMMDevice* dev) {
    LPWSTR id = nullptr;
    std::string s;
    if (SUCCEEDED(dev->GetId(&id)) && id) s = to_utf8(id);
    if (id) CoTaskMemFree(id);
    return s;
}

Status create_enumerator(Com<IMMDeviceEnumerator>& e) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), out(e));
    return SUCCEEDED(hr) ? Status::success() : Status::hresult("CoCreateInstance(MMDeviceEnumerator)", hr);
}

void fill_device_info(IMMDevice* dev, AudioDeviceInfo& info) {
    info.id = device_id_of(dev);
    info.name = device_name_of(dev);
    Com<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, out(client)))) return;
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
        parse_wfx(mix, info.mix_format, &info.channel_mask);
        Com<IAudioClient3> c3;
        if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), out(c3)))) {
            UINT32 def = 0, fund = 0, minp = 0, maxp = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &minp, &maxp))) {
                info.supports_low_latency_shared = minp < def;
                info.shared_default_period_ms = 1000.0 * def / mix->nSamplesPerSec;
                info.shared_minimum_period_ms = 1000.0 * minp / mix->nSamplesPerSec;
            }
        }
        CoTaskMemFree(mix);
    }
    REFERENCE_TIME def = 0, minp = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&def, &minp))) { info.default_period_ms = hns_to_ms(def); info.minimum_period_ms = hns_to_ms(minp); }
}

// ---- Notifications ---------------------------------------------------------------------------------------------------------------------------

class NotificationClient final : public IMMNotificationClient {
public:
    NotificationClient(HANDLE wake, bool follow_default) : wake_(wake), follow_(follow_default) {}
    void set_current_device(const std::wstring& id) { std::lock_guard<std::mutex> g(m_); current_ = id; }
    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --refs_; if (r == 0) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** obj) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) { *obj = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK; }
        *obj = nullptr;
        return E_NOINTERFACE;
    }
    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (follow_ && flow == eRender && role == eConsole) SetEvent(wake_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { if (follow_) SetEvent(wake_); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override { if (is_current(id)) SetEvent(wake_); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override { if (is_current(id) || follow_) SetEvent(wake_); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
private:
    bool is_current(LPCWSTR id) { std::lock_guard<std::mutex> g(m_); return id && current_ == id; }
    std::atomic<ULONG> refs_{1};
    HANDLE wake_;
    bool follow_;
    std::mutex m_;
    std::wstring current_;
};

// ---- The stream ---------------------------------------------------------------------------------------------------------------------------------

struct Session {
    Com<IMMDevice> device;
    Com<IAudioClient> client;
    Com<IAudioRenderClient> render;
    Com<ISimpleAudioVolume> volume;
    HANDLE audio_event = nullptr;
    AudioFormat device_format;
    uint32_t buffer_frames = 0, period_frames = 0;
    bool exclusive = false, low_latency = false, engine_conversion = false;
    double stream_latency_ms = 0;
    std::string name;
    std::unique_ptr<AudioConverter> converter;
    ~Session() { if (audio_event) CloseHandle(audio_event); }
};

class WasapiStream final : public AudioStream {
public:
    explicit WasapiStream(const AudioStreamConfig& cfg) : cfg_(cfg) {
        cb_format_ = cfg.format;
        cb_format_.format = SampleFormat::F32;
        volume_ = cfg.session_volume;
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~WasapiStream() override {
        stop();
        CloseHandle(stop_event_);
        CloseHandle(wake_event_);
    }

    Status start() override {
        if (thread_.joinable()) return Status::success();
        ResetEvent(stop_event_);
        std::promise<Status> first;
        auto first_result = first.get_future();
        thread_ = std::thread([this, p = std::move(first)]() mutable { run(std::move(p)); });
        Status s = first_result.get();
        if (!s.ok) { thread_.join(); }
        return s;
    }

    void stop() override {
        if (!thread_.joinable()) return;
        SetEvent(stop_event_);
        thread_.join();
        std::lock_guard<std::mutex> g(m_);
        stats_.running = false;
    }

    AudioStreamStats stats() const override { std::lock_guard<std::mutex> g(m_); return stats_; }
    void set_volume(float v) override { volume_ = std::clamp(v, 0.0f, 1.0f); volume_dirty_ = true; }

private:
    void note_error(const std::string& s) { std::lock_guard<std::mutex> g(m_); stats_.last_error = s; }

    Status open(Session& s, NotificationClient* notifier, Com<IMMDeviceEnumerator>& enumerator);
    bool init_shared(Session& s, const WAVEFORMATEX* mix, Status& err);
    bool init_exclusive(Session& s, Status& err);
    bool activate(Session& s, Com<IAudioClient>& c) { return SUCCEEDED(s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, out(c))); }
    enum class Pump { Stop, Restart };
    Pump pump(Session& s);
    void run(std::promise<Status> first);

    AudioStreamConfig cfg_;
    AudioFormat cb_format_;
    std::thread thread_;
    HANDLE stop_event_ = nullptr, wake_event_ = nullptr;
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> volume_dirty_{true};
    mutable std::mutex m_;
    AudioStreamStats stats_;
    double callback_sum_ms_ = 0;
    uint64_t callback_count_ = 0;
};

bool WasapiStream::init_shared(Session& s, const WAVEFORMATEX* mix, Status& err) {
    AudioFormat mix_fmt;
    const bool mix_ok = parse_wfx(mix, mix_fmt);
    REFERENCE_TIME def = 0, minp = 0;
    s.client->GetDevicePeriod(&def, &minp);

    // A: IAudioClient3 low-latency shared stream in the engine's own format.
    if (cfg_.low_latency && mix_ok) {
        Com<IAudioClient3> c3;
        if (SUCCEEDED(s.client->QueryInterface(__uuidof(IAudioClient3), out(c3)))) {
            UINT32 defp = 0, fund = 0, minf = 0, maxf = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &defp, &fund, &minf, &maxf))) {
                UINT32 want = minf;
                if (cfg_.period_frames) {
                    const uint32_t scaled = uint32_t(uint64_t(cfg_.period_frames) * mix->nSamplesPerSec / std::max<uint32_t>(1, cfg_.format.sample_rate));
                    want = std::clamp(scaled, minf, maxf);
                    if (fund) want = ((want + fund - 1) / fund) * fund;
                    want = std::min(want, maxf);
                }
                HRESULT hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want, mix, nullptr);
                if (SUCCEEDED(hr)) {
                    s.device_format = mix_fmt;
                    s.period_frames = want;
                    s.low_latency = true;
                    return true;
                }
                err = Status::hresult("IAudioClient3::InitializeSharedAudioStream", hr);
                Com<IAudioClient> fresh;
                if (!activate(s, fresh)) return false;
                s.client = std::move(fresh);   // an IAudioClient cannot be initialised twice
            }
        }
    }

    const REFERENCE_TIME buffer = cfg_.period_frames ? frames_to_hns(cfg_.period_frames, cfg_.format.sample_rate) : def;
    // B: the Windows audio engine converts between our format and the mix format.
    if (cfg_.allow_engine_conversion) {
        WAVEFORMATEXTENSIBLE ours = make_wfx(cb_format_);
        HRESULT hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, buffer, 0,
                                          &ours.Format, nullptr);
        if (SUCCEEDED(hr)) {
            s.device_format = cb_format_;
            s.engine_conversion = cb_format_ != mix_fmt;
            return true;
        }
        err = Status::hresult("IAudioClient::Initialize(shared, auto-convert)", hr);
        Com<IAudioClient> fresh;
        if (!activate(s, fresh)) return false;
        s.client = std::move(fresh);
    }
    // C: engine format, converted by us.
    HRESULT hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer, 0, mix, nullptr);
    if (FAILED(hr)) { err = Status::hresult("IAudioClient::Initialize(shared, mix format)", hr); return false; }
    s.device_format = mix_fmt;
    return mix_ok;
}

bool WasapiStream::init_exclusive(Session& s, Status& err) {
    s.exclusive = true;
    // Candidate formats: the game's own rate/channels in float, then integer formats; then the device's default format; then the mix format's rate.
    std::vector<AudioFormat> candidates;
    for (SampleFormat sf : {SampleFormat::F32, SampleFormat::S32, SampleFormat::S24, SampleFormat::S16}) candidates.push_back({cfg_.format.sample_rate, cfg_.format.channels, sf});
    Com<IPropertyStore> props;
    if (SUCCEEDED(s.device->OpenPropertyStore(STGM_READ, out(props)))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kDeviceFormatKey, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
            AudioFormat dev;
            if (parse_wfx(reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData), dev)) {
                candidates.push_back(dev);
                for (SampleFormat sf : {SampleFormat::S32, SampleFormat::S24, SampleFormat::S16, SampleFormat::F32}) candidates.push_back({dev.sample_rate, dev.channels, sf});
            }
        }
        PropVariantClear(&v);
    }
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(s.client->GetMixFormat(&mix)) && mix) {
        AudioFormat m;
        if (parse_wfx(mix, m)) for (SampleFormat sf : {SampleFormat::S24, SampleFormat::S16, SampleFormat::S32}) candidates.push_back({m.sample_rate, m.channels, sf});
        CoTaskMemFree(mix);
    }
    REFERENCE_TIME def = 0, minp = 0;
    s.client->GetDevicePeriod(&def, &minp);
    for (const AudioFormat& f : candidates) {
        WAVEFORMATEXTENSIBLE w = make_wfx(f);
        if (s.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) != S_OK) continue;
        REFERENCE_TIME period = cfg_.period_frames ? std::max(minp, frames_to_hns(cfg_.period_frames, cfg_.format.sample_rate)) : minp;
        HRESULT hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &w.Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames = 0;
            s.client->GetBufferSize(&frames);
            period = frames_to_hns(frames, f.sample_rate);
            Com<IAudioClient> fresh;
            if (!activate(s, fresh)) return false;
            s.client = std::move(fresh);
            hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &w.Format, nullptr);
        }
        if (SUCCEEDED(hr)) { s.device_format = f; return true; }
        err = Status::hresult("IAudioClient::Initialize(exclusive)", hr);
        Com<IAudioClient> fresh;
        if (!activate(s, fresh)) return false;
        s.client = std::move(fresh);
        if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || hr == AUDCLNT_E_DEVICE_IN_USE) return false;   // retrying other formats cannot help
    }
    if (err.ok) err = Status::fail("no exclusive-mode format accepted by the device", uint32_t(AUDCLNT_E_UNSUPPORTED_FORMAT));
    return false;
}

Status WasapiStream::open(Session& s, NotificationClient* notifier, Com<IMMDeviceEnumerator>& enumerator) {
    HRESULT hr;
    if (cfg_.device_id.empty()) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, out(s.device));
    else hr = enumerator->GetDevice(to_wide(cfg_.device_id).c_str(), out(s.device));
    if (FAILED(hr)) return Status::hresult(cfg_.device_id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice", hr);
    s.name = device_name_of(s.device.get());
    notifier->set_current_device(to_wide(device_id_of(s.device.get())));
    if (!activate(s, s.client)) return Status::fail("IMMDevice::Activate(IAudioClient)", uint32_t(E_FAIL));

    Status err = Status::success();
    bool ok;
    if (cfg_.share == ShareMode::Exclusive) {
        ok = init_exclusive(s, err);
    } else {
        WAVEFORMATEX* mix = nullptr;
        hr = s.client->GetMixFormat(&mix);
        if (FAILED(hr)) return Status::hresult("GetMixFormat", hr);
        ok = init_shared(s, mix, err);
        CoTaskMemFree(mix);
    }
    if (!ok) return err.ok ? Status::fail("could not initialise the audio client", uint32_t(E_FAIL)) : err;

    UINT32 frames = 0;
    hr = s.client->GetBufferSize(&frames);
    if (FAILED(hr)) return Status::hresult("GetBufferSize", hr);
    s.buffer_frames = frames;
    if (!s.period_frames) {
        REFERENCE_TIME def = 0, minp = 0;
        s.client->GetDevicePeriod(&def, &minp);
        s.period_frames = s.exclusive ? frames : uint32_t(double(def) * s.device_format.sample_rate / double(kHnsPerSecond) + 0.5);
    }
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(s.client->GetStreamLatency(&latency))) s.stream_latency_ms = hns_to_ms(latency);

    s.audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = s.client->SetEventHandle(s.audio_event);
    if (FAILED(hr)) return Status::hresult("SetEventHandle", hr);
    hr = s.client->GetService(__uuidof(IAudioRenderClient), out(s.render));
    if (FAILED(hr)) return Status::hresult("GetService(IAudioRenderClient)", hr);
    s.client->GetService(__uuidof(ISimpleAudioVolume), out(s.volume));   // absent for exclusive streams on some drivers: not an error
    // The session volume is set BEFORE Start so that a silent test stream never makes a sound.
    if (s.volume) { s.volume->SetMasterVolume(volume_, nullptr); volume_dirty_ = false; } else volume_dirty_ = true;

    s.converter.reset(new AudioConverter(cb_format_, s.device_format, [this](float* out_buf, uint32_t n) {
        const int64_t t0 = qpc_now();
        if (cfg_.callback) cfg_.callback(out_buf, n, cb_format_);
        else std::memset(out_buf, 0, size_t(n) * cb_format_.channels * sizeof(float));
        const double ms = qpc_to_ms(qpc_now() - t0);
        std::lock_guard<std::mutex> g(m_);
        callback_sum_ms_ += ms;
        ++callback_count_;
        stats_.avg_callback_ms = callback_sum_ms_ / double(callback_count_);
        stats_.max_callback_ms = std::max(stats_.max_callback_ms, ms);
        if (ms > stats_.period_ms && stats_.period_ms > 0) ++stats_.callback_overruns;
    }));

    {
        std::lock_guard<std::mutex> g(m_);
        stats_.exclusive = s.exclusive;
        stats_.low_latency_path = s.low_latency;
        stats_.engine_conversion = s.engine_conversion;
        stats_.our_conversion = !s.converter->passthrough() && !s.engine_conversion;
        stats_.device_format = s.device_format;
        stats_.period_frames = s.period_frames;
        stats_.buffer_frames = s.buffer_frames;
        stats_.period_ms = 1000.0 * s.period_frames / s.device_format.sample_rate;
        stats_.buffer_ms = 1000.0 * s.buffer_frames / s.device_format.sample_rate;
        stats_.stream_latency_ms = s.stream_latency_ms;
        stats_.device_name = s.name;
    }
    return Status::success();
}

WasapiStream::Pump WasapiStream::pump(Session& s) {
    // First buffer: render real audio into the whole device buffer before Start so that the engine never starts on silence.
    {
        BYTE* data = nullptr;
        if (SUCCEEDED(s.render->GetBuffer(s.buffer_frames, &data))) {
            s.converter->render(data, s.buffer_frames);
            s.render->ReleaseBuffer(s.buffer_frames, 0);
        }
    }
    if (FAILED(s.client->Start())) return Pump::Restart;
    { std::lock_guard<std::mutex> g(m_); stats_.running = true; }

    const HANDLE handles[3] = {stop_event_, wake_event_, s.audio_event};
    uint32_t wakeups = 0;
    for (;;) {
        const DWORD w = WaitForMultipleObjects(3, handles, FALSE, 2000);
        if (w == WAIT_OBJECT_0) { s.client->Stop(); return Pump::Stop; }
        if (w == WAIT_OBJECT_0 + 1) { s.client->Stop(); return Pump::Restart; }
        if (w == WAIT_TIMEOUT) {
            UINT32 pad;
            if (FAILED(s.client->GetCurrentPadding(&pad))) { s.client->Stop(); return Pump::Restart; }   // device went away without a notification
            continue;
        }
        if (w != WAIT_OBJECT_0 + 2) { s.client->Stop(); return Pump::Restart; }

        if (volume_dirty_.exchange(false) && s.volume) s.volume->SetMasterVolume(volume_, nullptr);

        UINT32 frames = s.buffer_frames;
        if (!s.exclusive) {
            UINT32 pad = 0;
            HRESULT hr = s.client->GetCurrentPadding(&pad);
            if (FAILED(hr)) { s.client->Stop(); return Pump::Restart; }
            if (pad == 0 && wakeups > 2) { std::lock_guard<std::mutex> g(m_); ++stats_.glitches; }   // the device ran dry before we got here
            frames = s.buffer_frames > pad ? s.buffer_frames - pad : 0;
        }
        ++wakeups;
        if (frames == 0) continue;
        BYTE* data = nullptr;
        HRESULT hr = s.render->GetBuffer(frames, &data);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_BUFFER_TOO_LARGE) continue;
            s.client->Stop();
            return Pump::Restart;
        }
        s.converter->render(data, frames);
        s.render->ReleaseBuffer(frames, 0);
        std::lock_guard<std::mutex> g(m_);
        stats_.frames_rendered += frames;
    }
}

void WasapiStream::run(std::promise<Status> first) {
    ScopedCom com;
    MmcssScope mmcss;
    if (cfg_.mmcss && !mmcss.begin(L"Pro Audio").ok) mmcss.begin(L"Audio");

    Com<IMMDeviceEnumerator> enumerator;
    Status es = com.ok() ? create_enumerator(enumerator) : Status::hresult("CoInitializeEx", com.hr);
    if (!es.ok) { first.set_value(es); return; }
    auto* notifier = new NotificationClient(wake_event_, cfg_.follow_default_device);
    enumerator->RegisterEndpointNotificationCallback(notifier);

    bool reported = false;
    bool stopping = false;
    while (!stopping) {
        Session s;
        Status st = open(s, notifier, enumerator);
        if (!reported) {
            first.set_value(st);
            reported = true;
            if (!st.ok) break;
        }
        if (!st.ok) {
            note_error(std::string(st.what) + ": " + error_message(st.code));
            // no device (unplugged): wait for a hot-plug notification, retry at least every 500 ms
            const HANDLE h[2] = {stop_event_, wake_event_};
            if (WaitForMultipleObjects(2, h, FALSE, 500) == WAIT_OBJECT_0) stopping = true;
            continue;
        }
        const Pump r = pump(s);
        s.client->Stop();
        s.client->Reset();
        if (r == Pump::Stop) { stopping = true; break; }
        std::lock_guard<std::mutex> g(m_);
        ++stats_.restarts;
    }
    enumerator->UnregisterEndpointNotificationCallback(notifier);
    notifier->Release();
    if (!reported) first.set_value(Status::fail("stopped before opening", ERROR_CANCELLED));
}

}  // namespace

// ---- Public enumeration and factory ------------------------------------------------------------------------------------------------------------

Status enumerate_audio_devices(AudioFlow flow, std::vector<AudioDeviceInfo>& result) {
    result.clear();
    ScopedCom com;
    if (!com.ok()) return Status::hresult("CoInitializeEx", com.hr);
    Com<IMMDeviceEnumerator> e;
    if (Status s = create_enumerator(e); !s.ok) return s;
    const EDataFlow df = flow == AudioFlow::Render ? eRender : eCapture;
    std::string default_id, comm_id;
    Com<IMMDevice> d;
    if (SUCCEEDED(e->GetDefaultAudioEndpoint(df, eConsole, out(d)))) default_id = device_id_of(d.get());
    d.reset();
    if (SUCCEEDED(e->GetDefaultAudioEndpoint(df, eCommunications, out(d)))) comm_id = device_id_of(d.get());
    Com<IMMDeviceCollection> coll;
    HRESULT hr = e->EnumAudioEndpoints(df, DEVICE_STATE_ACTIVE, out(coll));
    if (FAILED(hr)) return Status::hresult("EnumAudioEndpoints", hr);
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        Com<IMMDevice> dev;
        if (FAILED(coll->Item(i, out(dev)))) continue;
        AudioDeviceInfo info;
        info.flow = flow;
        fill_device_info(dev.get(), info);
        info.is_default = info.id == default_id;
        info.is_default_communications = info.id == comm_id;
        result.push_back(std::move(info));
    }
    std::stable_sort(result.begin(), result.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) { return a.is_default && !b.is_default; });
    return Status::success();
}

bool default_audio_device(AudioFlow flow, AudioDeviceInfo& info) {
    std::vector<AudioDeviceInfo> all;
    if (!enumerate_audio_devices(flow, all).ok) return false;
    for (auto& d : all) if (d.is_default) { info = d; return true; }
    return false;
}

std::unique_ptr<AudioStream> create_wasapi_stream(const AudioStreamConfig& cfg, Status* status) {
    if (cfg.format.channels == 0 || cfg.format.sample_rate == 0) {
        if (status) *status = Status::fail("invalid audio format", ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (status) *status = Status::success();
    return std::unique_ptr<AudioStream>(new WasapiStream(cfg));
}

}  // namespace nue::win
