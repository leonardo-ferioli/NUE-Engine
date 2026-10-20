// Unit tests of nue_d3d9: formats, generated fixed-function programs, the layer on a null Gpu (no window, no GPU, no game data).
#include "layer/fixed_function.hpp"
#include "layer/formats.hpp"
#include "nue/d3d9_layer.hpp"
#include "nue/null_gpu.hpp"
#include "nue/shader/d3d9_shader.hpp"
#include "nue/shader/d3d9_to_spirv.hpp"
#include "nue/window_fit.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace nue;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// A Gpu that remembers the last draw.
class RecordingGpu : public NullGpu {
public:
    void draw(const gpu::DrawDesc& d) override { last = d; layout.assign(d.layout.begin(), d.layout.end()); last.layout = layout; NullGpu::draw(d); ++seen; }
    gpu::DrawDesc last;
    std::vector<gpu::VertexAttribute> layout;
    int seen = 0;
};

static void test_formats() {
    using namespace d3d9;
    const FormatInfo& dxt1 = format_info(0x31545844);   // 'DXT1'
    CHECK(dxt1.valid && dxt1.compressed && dxt1.block_bytes == 8);
    CHECK(level_bytes(dxt1, 5, 5) == 32);              // 2x2 blocks of 8 bytes
    CHECK(level_pitch(format_info(21), 100) == 400);
    CHECK(mip_count(256, 64) == 9);
    // one BC1 block: colour0 = pure red (565 0xF800), colour1 = pure blue, every index 0 -> every texel is colour0
    const uint8_t block[8] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
    std::vector<std::byte> out;
    uint32_t pitch = 0;
    convert_pixels(dxt1, gpu::Format::rgba8, block, 8, 4, 4, out, pitch);
    CHECK(pitch == 16 && out.size() == 64);
    CHECK(uint8_t(out[0]) == 255 && uint8_t(out[1]) == 0 && uint8_t(out[2]) == 0 && uint8_t(out[3]) == 255);
    // X8R8G8B8 -> BGRA8 forces alpha to 255
    const uint8_t xrgb[4] = {1, 2, 3, 0};
    convert_pixels(format_info(22), gpu::Format::bgra8, xrgb, 4, 1, 1, out, pitch);
    CHECK(uint8_t(out[0]) == 1 && uint8_t(out[2]) == 3 && uint8_t(out[3]) == 255);
    // L8 -> RGBA8
    const uint8_t l8[1] = {77};
    convert_pixels(format_info(50), gpu::Format::rgba8, l8, 1, 1, 1, out, pitch);
    CHECK(uint8_t(out[0]) == 77 && uint8_t(out[1]) == 77 && uint8_t(out[3]) == 255);
}

static void test_fixed_function_programs() {
    using namespace d3d9;
    FfVertexKey vk;
    vk.in.diffuse = true; vk.in.texcoord_present_mask = 1; vk.stages = 2; vk.fog_mode = 1;
    vk.transform_count[0] = 2;
    shader::Program vs = build_ff_vertex(vk);
    shader::TranslateOptions options;
    shader::TranslateResult result;
    CHECK(shader::translate_to_spirv(vs, options, result));
    CHECK(!result.spirv.empty() && result.spirv[0] == 0x07230203);
    FfPixelKey pk;
    pk.stage_count = 2;
    pk.stage[0].color_op = 4; pk.stage[0].color_arg1 = 2; pk.stage[0].color_arg2 = 0; pk.stage[0].alpha_op = 2; pk.stage[0].alpha_arg1 = 2; pk.stage[0].texture = true;
    pk.stage[1].color_op = 7; pk.stage[1].color_arg1 = 1; pk.stage[1].color_arg2 = 3; pk.stage[1].alpha_op = 1;
    shader::Program ps = build_ff_pixel(pk);
    shader::TranslateResult pr;
    CHECK(shader::translate_to_spirv(ps, options, pr));
    CHECK((pr.sampler_mask & 1) != 0);
    CHECK(vk.hash() != FfVertexKey{}.hash());
}

struct Scene {
    std::unique_ptr<d3d9::Layer> layer;
    d3d9::BufferId vb, ib;
};

static Scene make_scene(RecordingGpu& gpu) {
    Scene s;
    s.layer = d3d9::create_layer(gpu);
    d3d9::DeviceParams p;
    p.back_width = 640; p.back_height = 480; p.back_format = 22; p.depth_format = 75;
    s.layer->reset(p);
    s.layer->back_buffer(0, 0);
    // three vertices: XYZ + DIFFUSE (FVF 0x42), 16 bytes each
    s.vb = s.layer->create_vertex_buffer(48, 8, 0x42, 1).value;
    auto lock = s.layer->lock_buffer(s.vb, 0, 0, 0);
    float v[3][3] = {{0, 0, 0.5f}, {1, 0, 0.5f}, {0, 1, 0.5f}};
    for (int i = 0; i < 3; ++i) { std::memcpy(lock.value.data + i * 16, v[i], 12); const uint32_t c = 0xFF00FF00; std::memcpy(lock.value.data + i * 16 + 12, &c, 4); }
    s.layer->unlock_buffer(s.vb);
    s.ib = s.layer->create_index_buffer(6, 8, 101, 1).value;
    auto il = s.layer->lock_buffer(s.ib, 0, 0, 0);
    const uint16_t idx[3] = {0, 1, 2};
    std::memcpy(il.value.data, idx, 6);
    s.layer->unlock_buffer(s.ib);
    return s;
}

static void draw_triangle(Scene& s, bool depth_test) {
    s.layer->set_render_state(7, depth_test ? 1 : 0);   // D3DRS_ZENABLE
    s.layer->set_stream_source(0, s.vb, 0, 16);
    s.layer->set_indices(s.ib);
    s.layer->set_fvf(0x42);
    s.layer->clear({}, 3, 0xFF000000, 1.0f, 0);
    s.layer->draw_indexed(4, 0, 0, 3, 0, 1);
    s.layer->present();
}

static void test_layer_null_gpu() {
    RecordingGpu g1, g2;
    Scene a = make_scene(g1), b = make_scene(g2);
    draw_triangle(a, true);
    draw_triangle(b, true);
    CHECK(g1.seen == 1);
    CHECK(g1.draws == 1 && g1.frames == 1 && g1.passes == 2);   // the frame, plus the pass that clears the alpha of the X8R8G8B8 backbuffer at reset
    CHECK(g1.last.topology == gpu::Topology::triangles);
    CHECK(g1.last.index_count == 3 && g1.last.vertex_stride[0] == 16);
    CHECK(g1.layout.size() == 2 && g1.layout[0].semantic == 0 && g1.layout[1].semantic == 10);   // position, colour
    CHECK(g1.last.depth_stencil.depth_test);
    CHECK(a.layer->stats().draws_fixed_function == 1);   // no shaders bound: the generated fixed-function pipeline
    CHECK(g1.call_hash == g2.call_hash);                 // deterministic
    // a state change reaches the draw
    RecordingGpu g3;
    Scene c = make_scene(g3);
    draw_triangle(c, false);
    CHECK(!g3.last.depth_stencil.depth_test);
    CHECK(g3.call_hash != g1.call_hash);
}

static void test_state_blocks() {
    RecordingGpu g;
    Scene s = make_scene(g);
    s.layer->set_render_state(27, 0);                    // alpha blend off
    auto id = s.layer->begin_state_block();
    s.layer->set_render_state(27, 1);                    // recorded, not applied
    s.layer->end_state_block(id);
    draw_triangle(s, true);
    CHECK(!g.last.blend.enable);
    s.layer->apply(id);
    draw_triangle(s, true);
    CHECK(g.last.blend.enable);
}

static void test_dynamic_buffer_policy() {
    RecordingGpu g;
    Scene s = make_scene(g);
    draw_triangle(s, true);
    CHECK(s.layer->stats().static_buffer_uploads == 2);          // vertex and index buffer, once
    draw_triangle(s, true);
    CHECK(s.layer->stats().static_buffer_uploads == 2);          // unchanged: the same GPU buffers are reused
    // rewritten after being drawn from, repeatedly: the buffer becomes a transient-copy buffer
    for (int i = 0; i < 4; ++i) {
        auto lock = s.layer->lock_buffer(s.vb, 0, 0, d3d9::lock_discard);
        lock.value.data[0] = std::byte(i);
        s.layer->unlock_buffer(s.vb);
        draw_triangle(s, true);
    }
    CHECK(s.layer->stats().transient_copies >= 1);
}

// A picture is fitted into the visible area without distortion, never larger than the area, centred, for the usual desktop shapes.
static void test_window_fit() {
    struct Case { uint32_t cw, ch, aw, ah; uint32_t ew, eh; } cases[] = {
        {1280, 720, 2496, 1664, 2496, 1404},   // 16:9 picture on a 3:2 desktop (the reported case: 2958x1664 before): fit the width, bars above and below
        {1280, 720, 1920, 1080, 1920, 1080},   // 16:9 on 16:9: fills
        {1280, 720, 1920, 1200, 1920, 1080},   // 16:9 on 16:10
        {1280, 720, 3440, 1440, 2560, 1440},   // 16:9 on 21:9: bars left and right
        {1280, 720, 1024, 768, 1024, 576},     // 16:9 on 4:3
        {1024, 768, 1920, 1080, 1440, 1080},   // 4:3 on 16:9
        {2496, 1664, 2496, 1664, 2496, 1664},  // the desktop resolution itself
    };
    for (const Case& c : cases) {
        const nue::FitRect r = nue::fit_aspect(c.cw, c.ch, 0, 0, c.aw, c.ah);
        std::printf("fit %ux%u into %ux%u -> %ux%u at %d,%d%c", c.cw, c.ch, c.aw, c.ah, r.width, r.height, r.x, r.y, 10);
        CHECK(r.width == c.ew && r.height == c.eh);
        CHECK(r.width <= c.aw && r.height <= c.ah);
        CHECK(r.x == int32_t((c.aw - r.width) / 2) && r.y == int32_t((c.ah - r.height) / 2));
    }
    const nue::FitRect offset = nue::fit_aspect(1280, 720, 100, 50, 800, 800);   // an area that starts away from the origin
    CHECK(offset.width == 800 && offset.height == 450 && offset.x == 100 && offset.y == 50 + 175);
}

int main() {
    test_formats();
    test_fixed_function_programs();
    test_layer_null_gpu();
    test_state_blocks();
    test_dynamic_buffer_policy();
    test_window_fit();
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("PASS nue_d3d9 tests\n");
    return 0;
}
