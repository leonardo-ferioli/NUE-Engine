// Smallest useful client of the installed SDK with its own main(): headless engine on the null backend, files, mapping, hashing, the mixer through
// the C++ wrapper. Exit code 0 = everything the SDK promises worked. Console only: no window, no sound device.
#include "nuepp/nue.hpp"
#include "nue_legacy/guest_memory.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;
#define EXPECT(cond) do { if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main() {
    // the header and the library must agree on the API version (the SDK is one unit)
    EXPECT(nue_get_version() == NUE_API_VERSION);
    std::printf("consume_core: NUE %s\n", nue_get_version_string());

    std::string root = (std::filesystem::temp_directory_path() / ("nue_consume_core_" + std::to_string(unsigned(nue_get_version())))).generic_string();
    std::filesystem::remove_all(root);

    nue_init_desc d = nuepp::init<nue_init_desc>();
    d.app_name = "consume_core"; d.app_id = "consume-core"; d.org_name = "NUE";
    d.backend = NUE_BACKEND_NULL;
    d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_DETERMINISTIC;
    d.user_data_path = root.c_str();
    d.log_level = NUE_LOG_WARN;
    {
        nuepp::Context ctx(d);
        EXPECT(ctx.ok());
        if (!ctx.ok()) { std::printf("init failed: %s\n", ctx.result().describe().c_str()); return 1; }

        nue_build_info bi = nuepp::init<nue_build_info>();
        EXPECT(NUE_SUCCEEDED(nue_get_build_info(&bi)));
        std::printf("  build: %s, backends %s, platform layer %s, %u-bit\n", bi.version_string, bi.backends, bi.platform_layer, bi.pointer_size * 8u);
        EXPECT(bi.pointer_size == sizeof(void*));

        // storage: atomic save, case-insensitive load, real mapping
        const char text[] = "installed SDK works";
        EXPECT(NUE_SUCCEEDED(nue_file_save("user:Saves/Slot1.dat", text, sizeof(text) - 1)));
        void* data = nullptr; size_t size = 0;
        EXPECT(NUE_SUCCEEDED(nue_file_load("user:saves/SLOT1.DAT", &data, &size)));
        EXPECT(size == sizeof(text) - 1 && data && std::memcmp(data, text, size) == 0);
        nue_free(data);
        nue_file f = 0;
        EXPECT(NUE_SUCCEEDED(nue_file_open("user:Saves/Slot1.dat", NUE_OPEN_READ, &f)));
        nue_file_map_info map = nuepp::init<nue_file_map_info>();
        EXPECT(NUE_SUCCEEDED(nue_file_map(f, 0, 0, &map)) && map.size == size && std::memcmp(map.data, text, size) == 0);
        EXPECT(NUE_SUCCEEDED(nue_file_unmap(f, &map)));
        nue_file_close(f);
        uint32_t crc = 0;
        EXPECT(NUE_SUCCEEDED(nue_file_hash_crc32("user:saves/slot1.dat", &crc)) && crc == nue_crc32(0, text, sizeof(text) - 1));

        // guest memory (legacy-layer header, header only) on top of the virtual address space service: a small masked arena, skipped on 32-bit hosts
        if (sizeof(void*) == 8) {
            nue::legacy::ArenaConfig ac;
            ac.arena_size = 64ull << 20;
            ac.alloc_begin = 0x01000000u;
            ac.alloc_end = 0x03000000u;
            nue_result ar = NUE_OK;
            auto arena = nue::legacy::ArenaMemory::create(ac, &ar);
            EXPECT(arena != nullptr);
            if (arena) {
                const uint32_t block = arena->alloc(256);
                EXPECT(block != 0 && arena->host(block, 256) == arena->base() + block && arena->address_of(arena->host(block, 1)) == block);
                EXPECT(arena->write_u32(block, 0xA5A5A5A5u));
            }
        }

        // mixer: one voice, deterministic output
        nuepp::Mixer mixer = nuepp::Mixer::manual(48000, 2, 64);
        EXPECT(mixer.ok());
        std::vector<int16_t> pcm(256, 16384);
        auto buffer = mixer.make_buffer({NUE_AUDIO_S16, 1, 48000}, pcm.data(), 256);
        EXPECT(buffer.ok());
        auto voice = mixer.play(buffer.value(), false, 1.0f, -1.0f);
        EXPECT(voice.ok());
        std::vector<float> out(2 * 64);
        EXPECT(mixer.render(out.data(), 64).ok());
        EXPECT(out[0] > 0.49f && out[0] < 0.51f);
    }
    std::filesystem::remove_all(root);
    if (g_failures) { std::printf("consume_core: %d failure(s)\n", g_failures); return 1; }
    std::printf("consume_core: ok\n");
    return 0;
}
