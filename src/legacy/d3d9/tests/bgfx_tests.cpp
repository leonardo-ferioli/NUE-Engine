// nue_bgfx_tests: the bgfx adapter on bgfx's Noop renderer (no window, no GPU): BgfxOptions::external_init, where bgfx belongs to someone else
// (the engine's nue_gfx in MANAGED mode) and the adapter must neither initialise nor shut it down, and must give back every object it made.
#include "nue/bgfx_gpu.hpp"
#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace nue;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static gpu::BgfxOptions noop_options(bool external) {
    gpu::BgfxOptions o;
    o.renderer = gpu::Renderer::noop;
    o.window = nullptr;
    o.window_width = 64; o.window_height = 64;
    o.single_threaded = true;
    o.external_init = external;
    return o;
}

// some work through the adapter: a texture with data, a static vertex buffer, the back buffer, a presented frame
static void use(gpu::BgfxGpu& g) {
    auto t = g.create_texture(16, 16, 1, gpu::Format::rgba8, false, false, 1);
    CHECK(bool(t));
    if (t) {
        std::vector<std::byte> pixels(16 * 16 * 4, std::byte{0x7F});
        g.update_texture(t.value, 0, 0, nullptr, Bytes(pixels.data(), pixels.size()), 16 * 4);
    }
    std::vector<std::byte> vertices(256, std::byte{1});
    auto b = g.create_static_buffer(Bytes(vertices.data(), vertices.size()), gpu::BufferKind::vertex);
    CHECK(bool(b));
    CHECK(g.resize(64, 64) == Status::ok);
    g.set_window_size(96, 96);
    g.present(0);
}

static void test_external_init_leaves_bgfx_alive() {
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.swapChain.width = 64; init.swapChain.height = 64;
    bgfx::renderFrame();   // single threaded, as the adapter does it
    CHECK(bgfx::init(init));
    const bgfx::Stats before = *bgfx::getStats();
    const uint16_t textures_before = before.numTextures, programs_before = before.numPrograms;
    for (int round = 0; round < 2; ++round) {   // two adapters in a row on the same bgfx (a device released and created again)
        std::string error;
        auto g = gpu::create_bgfx_gpu(noop_options(true), error);
        CHECK(g != nullptr);
        if (!g) std::printf("  error: %s\n", error.c_str());
        if (!g) break;
        CHECK(std::strcmp(g->stats().renderer_name, "Noop") == 0);
        use(*g);
        g.reset();   // must not shut bgfx down
        CHECK(bgfx::getRendererType() == bgfx::RendererType::Noop);
        bgfx::frame();   // destructions are applied at the next frame
        CHECK(bgfx::getStats()->numTextures == textures_before);
        CHECK(bgfx::getStats()->numPrograms == programs_before);
        const bgfx::Stats* now = bgfx::getStats();
        std::printf("  round %d: textures %u (before %u), programs %u (%u), vertex buffers %u (%u), frame buffers %u (%u), uniforms %u (%u), shaders %u (%u), layouts %u (%u)\n", round,
                    now->numTextures, before.numTextures, now->numPrograms, before.numPrograms, now->numVertexBuffers, before.numVertexBuffers, now->numFrameBuffers, before.numFrameBuffers,
                    now->numUniforms, before.numUniforms, now->numShaders, before.numShaders, now->numVertexLayouts, before.numVertexLayouts);
        CHECK(now->numVertexBuffers == before.numVertexBuffers);
        CHECK(now->numFrameBuffers == before.numFrameBuffers);
    }
    bgfx::shutdown();
}

static void test_own_init_still_works() {
    std::string error;
    auto g = gpu::create_bgfx_gpu(noop_options(false), error);   // the dll's own path: the adapter initialises and shuts down bgfx
    CHECK(g != nullptr);
    if (!g) std::printf("  error: %s\n", error.c_str());
    if (g) use(*g);
    g.reset();   // shuts bgfx down: a new init must work again
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    bgfx::renderFrame();
    CHECK(bgfx::init(init));
    bgfx::shutdown();
}

int main() {
    test_external_init_leaves_bgfx_alive();
    test_own_init_still_works();
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("PASS nue_bgfx_tests\n");
    return 0;
}
