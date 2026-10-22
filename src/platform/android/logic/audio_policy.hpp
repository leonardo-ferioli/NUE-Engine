// Audio output planning, route classification, audio-focus state machine, test tone.  Pure logic.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace nue::android {

enum class AudioRoute : int32_t { Unknown = 0, Speaker, Wired, Bluetooth, Usb, Hdmi };

// AudioDeviceInfo.TYPE_*: 1 earpiece, 2 speaker, 3 wired headset, 4 wired headphones, 5 line analog, 6 line digital,
// 7 BT SCO, 8 BT A2DP, 9 HDMI, 10 HDMI ARC, 11 USB device, 12 USB accessory, 13 dock, 22 USB headset,
// 23 hearing aid, 26 BLE headset, 27 BLE speaker, 30 BLE broadcast, 25 HDMI eARC (16 extra values omitted).
AudioRoute route_from_device_type(int type);
// Bluetooth adds 100-250 ms (A2DP) which breaks the feel of engine sounds; the engine may offset visuals or warn.
bool route_has_high_latency(AudioRoute r, int device_type);

struct AudioProps {
    int native_sample_rate = 48000;  // AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE
    int frames_per_buffer = 192;     // PROPERTY_OUTPUT_FRAMES_PER_BUFFER (the burst size)
    bool low_latency = false;        // FEATURE_AUDIO_LOW_LATENCY
    bool pro = false;                // FEATURE_AUDIO_PRO
};

struct AudioPlan {
    int sample_rate = 48000;
    int buffer_frames = 960;
    bool needs_resample = false;     // engine rate differs from the device rate: resample in the engine, not in AAudio
    bool low_latency_mode = false;   // open the stream with the low-latency performance mode
};

// `desired_rate` is the rate the game mixes at (DirectSound ring buffers, usually 22.05 / 44.1 kHz).  Opening the
// stream at the device's native rate keeps AAudio on the fast (MMAP) path; the engine converts when they differ.
// The buffer is a multiple of the burst size covering `target_latency_ms` (minimum 2 bursts, maximum 1 s).
AudioPlan plan_audio(const AudioProps& p, int desired_rate, int target_latency_ms);

// AudioManager.OnAudioFocusChangeListener values: GAIN 1, LOSS -1, LOSS_TRANSIENT -2, LOSS_TRANSIENT_CAN_DUCK -3.
enum class FocusAction : int32_t { None = 0, Resume, Pause, Duck, Unduck };

class AudioFocusModel {
public:
    FocusAction on_change(int focus_change);
    bool paused() const { return paused_; }
    bool ducked() const { return ducked_; }
    bool permanently_lost() const { return lost_permanently_; }

private:
    bool paused_ = false, ducked_ = false, lost_permanently_ = false;
};

// Phase-continuous sine for the sample app / self test.
class ToneGenerator {
public:
    ToneGenerator(float freq_hz, int sample_rate, float amplitude = 0.2f)
        : step_(2.0 * 3.14159265358979323846 * double(freq_hz) / sample_rate), amp_(amplitude) {}
    void fill(float* out, int frames) {
        for (int i = 0; i < frames; ++i) {
            out[i] = static_cast<float>(std::sin(phase_)) * amp_;
            phase_ += step_;
            if (phase_ > 6.283185307179586) phase_ -= 6.283185307179586;
        }
    }
    double phase() const { return phase_; }

private:
    double phase_ = 0;
    double step_;
    float amp_;
};

} // namespace nue::android
