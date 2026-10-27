// nue_dsound_trace: look at a Level A trace and replay it on a fresh Device (see include/nue_legacy/dsound_trace.hpp).
//   nue_dsound_trace info   <trace.ndst>      totals: records, renders, frames, PCM bytes and the two hashes
//   nue_dsound_trace replay <trace.ndst>      replays on a default Device; exit 0 when every answer and every mixed block equals the record
//   nue_dsound_trace pcm    <trace.ndst>      prints only the PCM hash (compare two clients: equal hash = equal bytes written to the buffers, in order)
#include <cstdio>
#include <cstring>
#include <vector>

#include "nue_legacy/dsound_trace.hpp"

static bool load(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s info|replay|pcm <trace.ndst>\n", argv[0]);
        return 2;
    }
    std::vector<uint8_t> data;
    if (!load(argv[2], data)) {
        std::printf("cannot read %s\n", argv[2]);
        return 2;
    }
    const nue::dsound::TraceInfo info = nue::dsound::trace_info(data.data(), data.size());
    if (!info.valid) {
        std::printf("invalid trace: %s\n", info.error.c_str());
        return 1;
    }
    if (!std::strcmp(argv[1], "pcm")) {
        std::printf("0x%016llx\n", static_cast<unsigned long long>(info.pcm_hash));
        return 0;
    }
    std::printf("version %u, %llu records, %llu renders (%llu frames), %llu unlocks (%llu PCM bytes)\npcm hash 0x%016llx, mixed hash 0x%016llx\n", info.version,
                static_cast<unsigned long long>(info.records), static_cast<unsigned long long>(info.renders), static_cast<unsigned long long>(info.rendered_frames),
                static_cast<unsigned long long>(info.unlocks), static_cast<unsigned long long>(info.pcm_bytes), static_cast<unsigned long long>(info.pcm_hash),
                static_cast<unsigned long long>(info.mixed_hash));
    if (!std::strcmp(argv[1], "replay")) {
        auto device = nue::dsound::make_device();
        const nue::dsound::ReplayResult r = nue::dsound::replay_trace(data.data(), data.size(), *device);
        std::printf("replay: %s (%llu records)%s%s\n", r.ok ? "identical" : "DIVERGED", static_cast<unsigned long long>(r.records), r.ok ? "" : ": ", r.error.c_str());
        return r.ok ? 0 : 1;
    }
    return 0;
}
