// A client of the separate asset library (nue::assets): creates an asset context and round-trips bytes through the built-in JDLZ codec.
#include "nue/assets.h"
#include "nue/nue.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    std::printf("consume_assets: nue_assets %s\n", nue_assets_version_string());
    nue_assets_context ctx = nullptr;
    nue_assets_context_desc desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.flags = NUE_ASSETS_CTX_NO_BUDGET;   // the engine itself is not initialised here
    if (nue_assets_context_create(&desc, &ctx) < 0) { std::printf("context_create failed\n"); return 1; }
    std::vector<unsigned char> src(5000);
    for (size_t i = 0; i < src.size(); ++i) src[i] = (unsigned char)((i / 7) & 0xFF);
    void* packed = nullptr; size_t packed_size = 0;
    void* back = nullptr; size_t back_size = 0;
    int rc = 1;
    if (nue_assets_codec_encode(ctx, "jdlz", src.data(), src.size(), 6, &packed, &packed_size) >= 0 &&
        nue_assets_codec_decode(ctx, "jdlz", packed, packed_size, &back, &back_size) >= 0 &&
        back_size == src.size() && std::memcmp(back, src.data(), src.size()) == 0) {
        std::printf("consume_assets: ok (%zu -> %zu bytes)\n", src.size(), packed_size);
        rc = 0;
    } else {
        std::printf("consume_assets: jdlz round trip failed\n");
    }
    nue_free(packed);
    nue_free(back);
    nue_assets_context_destroy(ctx);
    return rc;
}
