// hello_audio: plays a short chord through the software mixer on the default audio device (it makes sound: never run by the tests).
// Usage: nue_sample_hello_audio [seconds]
#include "nuepp/nue.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    int seconds = argc > 1 ? std::atoi(argv[1]) : 3;
    nue_init_desc d = nuepp::init<nue_init_desc>();
    d.app_name = "NUE hello_audio"; d.app_id = "hello-audio";
    d.subsystems = NUE_SUBSYSTEM_AUDIO | NUE_SUBSYSTEM_EVENTS;
    nuepp::Context ctx(d);
    if (!ctx.ok()) { std::fprintf(stderr, "init: %s\n", ctx.result().describe().c_str()); return 1; }

    nue_mixer_desc md = nuepp::init<nue_mixer_desc>();
    nuepp::Mixer mixer(md);
    if (!mixer.ok()) { std::fprintf(stderr, "mixer: %s\n", mixer.result().describe().c_str()); return 1; }
    nue_audio_spec spec;
    nue_mixer_get_spec(mixer.handle(), &spec);

    // three sine tones, 1 second each, looped, panned left / centre / right
    const double freqs[3] = {261.63, 329.63, 392.00};
    const float pans[3] = {-0.8f, 0.0f, 0.8f};
    for (int i = 0; i < 3; ++i) {
        std::vector<int16_t> pcm(spec.sample_rate);
        for (uint32_t k = 0; k < pcm.size(); ++k) pcm[k] = int16_t(6000.0 * std::sin(2.0 * 3.14159265358979 * freqs[i] * double(k) / double(spec.sample_rate)));
        auto buf = mixer.make_buffer({NUE_AUDIO_S16, 1, spec.sample_rate}, pcm.data(), uint32_t(pcm.size()));
        if (buf) mixer.play(buf.value(), true, 0.5f, pans[i]);
    }
    NUE_LOGI("sample", "playing at %u Hz, %u channels for %d s", spec.sample_rate, spec.channels, seconds);
    nuepp::sleep_ns(int64_t(seconds) * 1000000000ll, false);
    nue_mixer_stats st = mixer.stats();
    std::printf("mixed %llu blocks, %u voices, cpu %.1f%%\n", (unsigned long long)st.blocks_mixed, st.active_voices, double(st.cpu_load) * 100.0);
    return 0;
}
