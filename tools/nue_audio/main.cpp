// nue_audio: command line helper for the audio module.
//   nue_audio info <file>                       container, codec, format, length, loop points
//   nue_audio decode <in> <out.wav>             decode to 16-bit PCM WAV (loops are not applied)
//   nue_audio loudness <file>                   EBU R128 integrated loudness, range, sample and true peak
//   nue_audio raw <codec> <ch> <rate> <in> <out.wav> [interleave]   decode a headerless stream (psx, xbox, ima:<block>, ms:<block>, xas, eaxa, pcm16)
#include "nue/audio_codec.h"
#include "nue/audio_dsp.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static bool read_file(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }

static bool decode_pcm16(nue_adec d, std::vector<int16_t>& pcm, nue_adec_info& info) {
    NUE_STRUCT_INIT(&info);
    nue_adec_get_info(d, &info);
    nue_adec_set_loop(d, 0);
    std::vector<int16_t> buf(size_t(4096) * info.spec.channels);
    for (;;) {
        uint32_t got = 0;
        const nue_result r = nue_adec_read_s16(d, buf.data(), 4096, &got);
        pcm.insert(pcm.end(), buf.begin(), buf.begin() + size_t(got) * info.spec.channels);
        if (r == NUE_ERR_END_OF_DATA || got < 4096) return r != NUE_ERR_CORRUPT;
    }
}

static bool write_wav(const char* path, const std::vector<int16_t>& pcm, uint32_t ch, uint32_t rate) {
    std::vector<uint8_t> w;
    const uint32_t bytes = uint32_t(pcm.size() * 2);
    w.insert(w.end(), {'R', 'I', 'F', 'F'}); put32(w, 36 + bytes); w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(w, 16); put16(w, 1); put16(w, ch); put32(w, rate); put32(w, rate * ch * 2); put16(w, ch * 2); put16(w, 16);
    w.insert(w.end(), {'d', 'a', 't', 'a'}); put32(w, bytes);
    w.resize(w.size() + bytes);
    std::memcpy(&w[44], pcm.data(), bytes);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
    return bool(f);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: nue_audio info|decode|loudness|raw ... (see tools/nue_audio/main.cpp)\n"); return 2; }
    const std::string cmd = argv[1];
    std::vector<uint8_t> data;
    nue_adec d = nullptr;
    if (cmd == "raw") {
        if (argc < 7) { std::fprintf(stderr, "raw <codec> <ch> <rate> <in> <out.wav> [interleave]\n"); return 2; }
        if (!read_file(argv[5], data)) { std::fprintf(stderr, "cannot read %s\n", argv[5]); return 1; }
        nue_adec_raw_desc r;
        NUE_STRUCT_INIT(&r);
        const std::string c = argv[2];
        r.channels = uint32_t(std::atoi(argv[3])); r.sample_rate = uint32_t(std::atoi(argv[4]));
        if (argc > 7) r.interleave = uint32_t(std::strtoul(argv[7], nullptr, 0));
        if (c == "psx") r.codec = NUE_ACODEC_PSX_ADPCM;
        else if (c == "xbox") r.codec = NUE_ACODEC_IMA_XBOX;
        else if (c == "xas") r.codec = NUE_ACODEC_EA_XAS;
        else if (c == "eaxa") r.codec = NUE_ACODEC_EA_XA;
        else if (c == "pcm16") r.codec = NUE_ACODEC_PCM_S16LE;
        else if (c.rfind("ima:", 0) == 0) { r.codec = NUE_ACODEC_IMA_WAV; r.block_size = uint32_t(std::strtoul(c.c_str() + 4, nullptr, 0)); }
        else if (c.rfind("ms:", 0) == 0) { r.codec = NUE_ACODEC_MS_ADPCM; r.block_size = uint32_t(std::strtoul(c.c_str() + 3, nullptr, 0)); }
        else { std::fprintf(stderr, "unknown codec %s\n", argv[2]); return 2; }
        if (NUE_FAILED(nue_adec_open_raw_memory(data.data(), data.size(), &r, &d))) { std::fprintf(stderr, "cannot open the stream\n"); return 1; }
        std::vector<int16_t> pcm;
        nue_adec_info info;
        const bool ok = decode_pcm16(d, pcm, info);
        std::printf("%zu frames, %u ch, %u Hz%s\n", pcm.size() / info.spec.channels, info.spec.channels, info.spec.sample_rate, ok ? "" : " (damaged data)");
        const bool wrote = write_wav(argv[6], pcm, info.spec.channels, info.spec.sample_rate);
        nue_adec_destroy(d);
        return wrote ? 0 : 1;
    }
    if (!read_file(argv[2], data)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (NUE_FAILED(nue_adec_open_memory(data.data(), data.size(), &d))) { std::fprintf(stderr, "%s: not a supported audio file\n", argv[2]); return 1; }
    nue_adec_info info;
    NUE_STRUCT_INIT(&info);
    nue_adec_get_info(d, &info);
    int rc = 0;
    if (cmd == "info") {
        std::printf("codec %s, %u ch, %u Hz, ", nue_acodec_name(info.codec), info.spec.channels, info.spec.sample_rate);
        if (info.frame_count == UINT64_MAX) std::printf("length unknown"); else std::printf("%llu frames (%.3f s)", (unsigned long long)info.frame_count, double(info.frame_count) / info.spec.sample_rate);
        if (info.has_loop) std::printf(", loop %llu..%llu", (unsigned long long)info.loop_start, (unsigned long long)info.loop_end);
        if (info.title[0]) std::printf(", title \"%s\"", info.title);
        std::printf("\n");
    } else if (cmd == "decode") {
        if (argc < 4) { std::fprintf(stderr, "decode <in> <out.wav>\n"); rc = 2; }
        else {
            std::vector<int16_t> pcm;
            const bool ok = decode_pcm16(d, pcm, info);
            rc = write_wav(argv[3], pcm, info.spec.channels, info.spec.sample_rate) && ok ? 0 : 1;
            std::printf("%zu frames written%s\n", pcm.size() / info.spec.channels, ok ? "" : " (damaged data)");
        }
    } else if (cmd == "loudness") {
        std::vector<float> all, buf(size_t(4096) * info.spec.channels);
        for (;;) {
            uint32_t got = 0;
            nue_adec_set_loop(d, 0);
            const nue_result r = nue_adec_read_f32(d, buf.data(), 4096, &got);
            all.insert(all.end(), buf.begin(), buf.begin() + size_t(got) * info.spec.channels);
            if (r == NUE_ERR_END_OF_DATA || got < 4096) break;
        }
        nue_loudness_result lr;
        NUE_STRUCT_INIT(&lr);
        if (NUE_FAILED(nue_loudness_analyze(all.data(), uint32_t(all.size() / info.spec.channels), info.spec.channels, 0, info.spec.sample_rate, &lr))) { std::fprintf(stderr, "analysis failed\n"); rc = 1; }
        else std::printf("integrated %.2f LUFS, range %.2f LU, sample peak %.2f dBFS, true peak %.2f dBTP\n", lr.integrated_lufs, lr.range_lu,
                         20.0 * std::log10(std::max(lr.sample_peak, 1e-9)), 20.0 * std::log10(std::max(lr.true_peak, 1e-9)));
    } else {
        std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
        rc = 2;
    }
    nue_adec_destroy(d);
    return rc;
}
