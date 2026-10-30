// The Direct3D 9 layer on a MANAGED nue_gfx context (include/nue_legacy/d3d9_gfx.hpp), on bgfx's Noop renderer: no window, no GPU. Frames go through
// nue_gfx_present; a resize reaches the layer through nue_gfx; a suspended context is a lost device; a device re-created by nue_gfx (what an Android
// surface change does) makes the layer re-create its GPU objects, with the texture contents restored from the layer's copies.
#include "nue_legacy/d3d9_gfx.hpp"
#include "nue/core.h"
#include "nue/d3d9_layer.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace nue;
using namespace nue::d3d9;

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL: %s\n", what); } }

constexpr uint32_t kFmtA8R8G8B8 = 21, kFmtX8R8G8B8 = 22, kFmtD24S8 = 75, kPoolManaged = 1, kFvfXyzrhwDiffuse = 0x044, kTriangleList = 4, kClearTarget = 1;

void draw_frame(Device& d, TextureId texture) {
    d.begin_scene();
    const int32_t none[1] = {0};
    d.clear({none, 0}, kClearTarget, 0xFF204080u, 1.0f, 0);
    d.set_fvf(kFvfXyzrhwDiffuse);
    d.set_texture(0, texture);
    struct V { float x, y, z, rhw; uint32_t color; } v[3] = {{10, 10, 0, 1, 0xFFFF0000u}, {200, 10, 0, 1, 0xFF00FF00u}, {10, 200, 0, 1, 0xFF0000FFu}};
    d.draw_up(kTriangleList, 1, {reinterpret_cast<const std::byte*>(v), sizeof v}, sizeof(V));
    d.end_scene();
}
}  // namespace

int main() {
    const std::string root = (std::filesystem::temp_directory_path() / "nue_d3d9_gfx_tests").string();
    std::filesystem::create_directories(root + "/data");
    nue_init_desc init;
    NUE_STRUCT_INIT(&init);
    init.api_version = NUE_API_VERSION;
    init.app_name = "nue_d3d9_gfx_tests"; init.app_id = "nue-d3d9-gfx-tests"; init.org_name = "NUE";
    init.backend = NUE_BACKEND_NULL;
    init.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_LOG_FILE | NUE_INIT_DETERMINISTIC;
    const std::string data = root + "/data", user = root + "/user";
    init.game_data_path = data.c_str(); init.user_data_path = user.c_str();
    init.log_level = NUE_LOG_WARN;
    if (!NUE_SUCCEEDED(nue_init(&init))) { std::printf("FAIL: nue_init: %s\n", nue_get_last_error_message()); return 1; }

    {
        nue_gfx_desc gd;
        NUE_STRUCT_INIT(&gd);
        gd.renderer = NUE_RENDERER_NOOP;   // MANAGED: nue_gfx initialises bgfx (Noop), no window
        gd.shader_cache_dir = "cache:shaders/d3d9";
        nue_gfx gfx = nullptr;
        check(nue_gfx_create(&gd, &gfx) == NUE_OK && gfx, "nue_gfx_create (MANAGED, Noop)");

        // a PREPARE context is refused: bgfx is not initialised there
        nue_gfx_desc pd = gd;
        pd.flags = NUE_GFX_PREPARE_ONLY;
        pd.shader_cache_dir = nullptr;
        nue_gfx prepare = nullptr;
        if (nue_gfx_create(&pd, &prepare) == NUE_OK) {
            std::string error;
            check(!create_gfx_device(prepare, LayerOptions{}, error) && !error.empty(), "a PREPARE context is refused");
            nue_gfx_destroy(prepare);
        }

        std::string error;
        auto dev = create_gfx_device(gfx, LayerOptions{}, error);
        check(dev != nullptr, ("create_gfx_device: " + error).c_str());
        if (dev) {
            Device& d = dev->device();
            DeviceParams params;
            params.back_width = 640; params.back_height = 480; params.back_format = kFmtX8R8G8B8; params.depth_format = kFmtD24S8;
            check(d.reset(params) == Status::ok, "reset (CreateDevice)");
            check(d.test_cooperative_level() == Status::ok, "a new device is usable");

            // a managed texture with known texels
            auto tex = d.create_texture(4, 4, 1, 0, kFmtA8R8G8B8, kPoolManaged);
            check(bool(tex), "create_texture");
            auto locked = d.lock_texture(tex.value, 0, 0, nullptr, 0);
            check(bool(locked) && locked.value.data, "lock_texture");
            if (locked) for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) { const uint32_t c = 0xFF000000u | (y << 8) | x; std::memcpy(locked.value.data + y * locked.value.pitch + x * 4, &c, 4); }
            check(d.unlock_texture(tex.value, 0, 0) == Status::ok, "unlock_texture");

            draw_frame(d, tex.value);
            check(d.present() == Status::ok, "present through nue_gfx_present");
            const uint64_t draws = dev->layer().stats().draws;
            check(draws >= 1, "the layer drew");

            // the window size comes from nue_gfx (the layer only moves its letterbox)
            check(nue_gfx_resize(gfx, 800, 600) == NUE_OK, "nue_gfx_resize");
            draw_frame(d, tex.value);
            check(d.present() == Status::ok, "present after a resize");

            // suspended (Android: the surface is gone): a lost device, nothing is drawn
            check(nue_gfx_suspend(gfx) == NUE_OK, "nue_gfx_suspend");
            check(d.test_cooperative_level() == Status::device_lost, "suspended: D3DERR_DEVICELOST");
            const uint64_t before = dev->layer().stats().draws;
            draw_frame(d, tex.value);
            check(dev->layer().stats().draws == before && dev->stats().lost_draws >= 2, "draws and clears of a lost device do nothing");
            check(d.present() == Status::device_lost && dev->stats().lost_presents == 1, "present of a lost device fails");
            check(d.reset(params) == Status::device_lost, "reset fails while suspended");
            check(nue_gfx_resume(gfx) == NUE_OK, "nue_gfx_resume");
            check(d.test_cooperative_level() == Status::ok, "resumed with the same surface: usable again, no reset needed");
            draw_frame(d, tex.value);
            check(d.present() == Status::ok, "present after resume");

            // nue_gfx re-creates its device (a new surface on Android, a removed GPU): every bgfx object is gone
            const uint32_t generation = dev->stats().generation;
            check(nue_gfx_reset_device(gfx, NUE_TRUE) == NUE_OK, "nue_gfx_reset_device (forced)");
            check(d.test_cooperative_level() == Status::device_not_reset, "a re-created device: D3DERR_DEVICENOTRESET");
            check(dev->stats().generation == generation + 1 && dev->stats().device_resets == 1, "the layer follows the new generation");
            check(dev->layer().stats().gpu_resets == 1 && dev->layer().stats().levels_restored >= 1, "the texture was uploaded again from its copy");
            check(d.reset(params) == Status::ok, "the game's Reset");
            check(d.test_cooperative_level() == Status::ok, "usable after Reset");
            draw_frame(d, tex.value);
            check(d.present() == Status::ok, "present on the new device");
            check(dev->layer().stats().draws > before, "drawing again on the new device");
            auto again = d.lock_texture(tex.value, 0, 0, nullptr, 0x10 /* read only */);
            uint32_t texel = 0;
            if (again) std::memcpy(&texel, again.value.data + 2 * again.value.pitch + 3 * 4, 4);
            check(bool(again) && texel == (0xFF000000u | (2u << 8) | 3u), "the texture keeps its texels across the device reset");
            d.unlock_texture(tex.value, 0, 0);

            // a device loss the renderer reports: NEEDS_RESET, and Reset re-creates the device through nue_gfx_reset_device
            if (nue_gfx_report_device_lost(gfx) == NUE_OK && nue_gfx_get_state(gfx) == NUE_GFX_STATE_NEEDS_RESET) {
                check(d.test_cooperative_level() == Status::device_not_reset, "reported loss: D3DERR_DEVICENOTRESET");
                check(d.reset(params) == Status::ok && dev->stats().device_resets == 2, "Reset re-creates the device and the layer's objects");
                draw_frame(d, tex.value);
                check(d.present() == Status::ok, "present after a reported loss and Reset");
            }
            d.destroy(tex.value);
            dev.reset();
        }
        nue_gfx_destroy(gfx);
    }
    nue_shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("PASS the Direct3D 9 layer on nue_gfx (Noop): frames, resize, suspend, device re-creation\n");
    return 0;
}
