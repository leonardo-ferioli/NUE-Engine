// Mips, image conversion, metrics, executor, context cache, asynchronous requests, import pipeline, budget, Direct3D 9 seam.
#include "tex_common.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

using namespace texttest;
namespace fs = std::filesystem;

static nue_tex_image make_rgba(uint32_t w, uint32_t h, const std::vector<uint8_t>& px, uint32_t mips = 1, nue_tex_format f = NUE_TEX_RGBA8_UNORM) {
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d); d.format = f; d.width = w; d.height = h; d.mip_levels = mips;
    nue_tex_image im = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &im), NUE_OK);
    nue_tex_level_view v; NUE_STRUCT_INIT(&v);
    nue_tex_image_get_level(im, 0, 0, 0, &v);
    std::memcpy(v.data, px.data(), std::min<size_t>(px.size(), size_t(v.size)));
    return im;
}
static const uint8_t* level_data(nue_tex_image im, uint32_t l, uint64_t* size = nullptr) {
    nue_tex_level_view v; NUE_STRUCT_INIT(&v);
    nue_tex_image_get_level(im, l, 0, 0, &v);
    if (size) *size = v.size;
    return static_cast<const uint8_t*>(v.data);
}
static nue_tex_image encode_image(nue_tex_format f, uint32_t w, uint32_t h, uint32_t mips, uint32_t seed = 1, bool alpha = true) {
    nue_tex_image src = make_rgba(w, h, synth_rgba(w, h, seed, alpha));
    nue_tex_image out = nullptr;
    nue_tex_encode_params ep; nue_tex_encode_params_default(&ep); ep.quality = NUE_TEX_QUALITY_FAST;
    nue_tex_mip_params mp; nue_tex_mip_params_default(&mp);
    CHECK_EQ(nue_tex_image_convert(src, f, mips, &ep, &mp, &out), NUE_OK);
    nue_tex_image_destroy(src);
    return out;
}
static std::string temp_dir(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("nue_tex_") + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p.string();
}

TEST_CASE(mips_box_and_known_answers) {
    // constant colour stays constant at every level for every filter
    std::vector<uint8_t> flat(8 * 8 * 4);
    for (size_t i = 0; i < 64; ++i) { flat[i * 4] = 200; flat[i * 4 + 1] = 100; flat[i * 4 + 2] = 50; flat[i * 4 + 3] = 255; }
    for (nue_tex_mip_filter f : {NUE_TEX_MIP_BOX, NUE_TEX_MIP_TRIANGLE, NUE_TEX_MIP_KAISER, NUE_TEX_MIP_LANCZOS}) {
        nue_tex_image src = make_rgba(8, 8, flat), out = nullptr;
        nue_tex_mip_params mp; nue_tex_mip_params_default(&mp); mp.filter = f;
        CHECK_EQ(nue_tex_generate_mips(src, &mp, &out), NUE_OK);
        nue_tex_image_desc d; NUE_STRUCT_INIT(&d); nue_tex_image_get_desc(out, &d);
        CHECK_EQ(d.mip_levels, 4u);
        for (uint32_t l = 0; l < 4; ++l) {
            const uint8_t* p = level_data(out, l);
            CHECK(std::abs(int(p[0]) - 200) <= 1 && std::abs(int(p[1]) - 100) <= 1 && std::abs(int(p[2]) - 50) <= 1 && p[3] == 255);
        }
        nue_tex_image_destroy(src); nue_tex_image_destroy(out);
    }
    // 2x2 of black and white: gamma-space average is 128, linear-light average is 188 (sRGB of 0.5)
    std::vector<uint8_t> bw = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    nue_tex_image src = make_rgba(2, 2, bw), out = nullptr;
    nue_tex_mip_params mp; nue_tex_mip_params_default(&mp);
    CHECK_EQ(nue_tex_generate_mips(src, &mp, &out), NUE_OK);
    CHECK(std::abs(int(level_data(out, 1)[0]) - 128) <= 1);
    nue_tex_image_destroy(out);
    mp.flags |= NUE_TEX_MIP_SRGB;
    CHECK_EQ(nue_tex_generate_mips(src, &mp, &out), NUE_OK);
    CHECK(std::abs(int(level_data(out, 1)[0]) - 188) <= 1);
    nue_tex_image_destroy(out); nue_tex_image_destroy(src);
    // non power of two: 5x3 -> 2x1 -> 1x1
    src = make_rgba(5, 3, synth_rgba(5, 3));
    mp.flags = 0; mp.filter = NUE_TEX_MIP_KAISER;
    CHECK_EQ(nue_tex_generate_mips(src, &mp, &out), NUE_OK);
    nue_tex_level_view v; NUE_STRUCT_INIT(&v);
    nue_tex_image_get_level(out, 1, 0, 0, &v); CHECK(v.width == 2 && v.height == 1);
    nue_tex_image_get_level(out, 2, 0, 0, &v); CHECK(v.width == 1 && v.height == 1);
    nue_tex_image_destroy(src); nue_tex_image_destroy(out);
}

TEST_CASE(mips_preserved_and_generated) {
    // an image that already has level 1 keeps it when converted; level 2.. are generated from it
    nue_tex_image src = make_rgba(8, 8, synth_rgba(8, 8), 2);
    uint64_t sz; const uint8_t* l1 = level_data(src, 1, &sz);
    std::memset(const_cast<uint8_t*>(l1), 0, size_t(sz));
    for (size_t i = 0; i < sz; i += 4) const_cast<uint8_t*>(l1)[i] = 77;            // marker: R = 77
    nue_tex_image out = nullptr;
    CHECK_EQ(nue_tex_image_convert(src, NUE_TEX_BGRA8_UNORM, 4, nullptr, nullptr, &out), NUE_OK);
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d); nue_tex_image_get_desc(out, &d);
    CHECK_EQ(d.mip_levels, 4u);
    CHECK_EQ(level_data(out, 1)[2], 77);                                             // BGRA: R is the third byte
    CHECK_EQ(level_data(out, 2)[2], 77);                                             // generated from the marker level
    // drop the top mips
    nue_tex_image small = nullptr;
    CHECK_EQ(nue_tex_image_drop_top_mips(out, 1, &small), NUE_OK);
    nue_tex_image_get_desc(small, &d);
    CHECK(d.width == 4 && d.height == 4 && d.mip_levels == 3);
    CHECK_EQ(level_data(small, 0)[2], 77);
    nue_tex_image_destroy(src); nue_tex_image_destroy(out); nue_tex_image_destroy(small);
}

TEST_CASE(alpha_coverage_preserved) {
    // 16x16 alpha ramp: plain box filtering shrinks the share of texels above 0.5; the coverage option keeps it
    std::vector<uint8_t> px(16 * 16 * 4, 255);
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) px[(y * 16 + x) * 4 + 3] = uint8_t(x < 8 ? 0 : (x < 12 ? 200 : 255) - (y & 1) * 150);
    nue_tex_image src = make_rgba(16, 16, px), out = nullptr;
    nue_tex_mip_params mp; nue_tex_mip_params_default(&mp); mp.flags = NUE_TEX_MIP_ALPHA_COVERAGE;
    CHECK_EQ(nue_tex_generate_mips(src, &mp, &out), NUE_OK);
    auto cover = [](const uint8_t* p, size_t n) { size_t c = 0; for (size_t i = 0; i < n; ++i) c += p[i * 4 + 3] > 127; return double(c) / n; };
    CHECK_NEAR(cover(level_data(out, 1), 64), cover(level_data(src, 0), 256), 0.07);
    nue_tex_image_destroy(src); nue_tex_image_destroy(out);
}

TEST_CASE(image_convert_between_families_and_metrics) {
    const uint32_t w = 64, h = 64;
    nue_tex_image ref = make_rgba(w, h, synth_rgba(w, h, 4));
    for (nue_tex_format f : {NUE_TEX_BC7_UNORM, NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_ETC2_RGBA8}) {
        nue_tex_image a = nullptr, b = nullptr;
        nue_tex_encode_params ep; nue_tex_encode_params_default(&ep); ep.quality = NUE_TEX_QUALITY_FAST;
        CHECK_EQ(nue_tex_image_convert(ref, f, 1, &ep, nullptr, &a), NUE_OK);
        // transcode the compressed image to every other family and compare with the original
        for (nue_tex_format g : {NUE_TEX_BC3_UNORM, NUE_TEX_ASTC_UNORM(3), NUE_TEX_ETC2_RGBA8, NUE_TEX_RGBA8_UNORM}) {
            CHECK_EQ(nue_tex_image_convert(a, g, 1, &ep, nullptr, &b), NUE_OK);
            nue_tex_metrics m; NUE_STRUCT_INIT(&m);
            CHECK_EQ(nue_tex_compare_images(ref, b, &m), NUE_OK);
            std::printf("  %s -> %s : psnr %.1f dB ssim %.3f dE %.2f\n", nue_tex_format_name(f), nue_tex_format_name(g), m.psnr_db, m.ssim, m.delta_e_mean);
            CHECK(m.psnr_db > 18.0 && m.ssim > 0.6);
            nue_tex_image_destroy(b);
        }
        nue_tex_image_destroy(a);
    }
    nue_tex_image_destroy(ref);
    // metric formulas
    std::vector<uint8_t> a = synth_rgba(16, 16, 2), b = a;
    nue_tex_metrics m; NUE_STRUCT_INIT(&m);
    CHECK_EQ(nue_tex_compare_level(NUE_TEX_RGBA8_UNORM, a.data(), NUE_TEX_RGBA8_UNORM, b.data(), 16, 16, 1, &m), NUE_OK);
    CHECK_NEAR(m.psnr_db, 99.0, 1e-9); CHECK_NEAR(m.ssim, 1.0, 1e-9); CHECK_NEAR(m.delta_e_max, 0.0, 1e-9);
    for (auto& v : b) v = uint8_t(v == 255 ? 254 : v + 1);                      // every channel off by one: mse = 1
    CHECK_EQ(nue_tex_compare_level(NUE_TEX_RGBA8_UNORM, a.data(), NUE_TEX_RGBA8_UNORM, b.data(), 16, 16, 1, &m), NUE_OK);
    CHECK_NEAR(m.psnr_db, 48.1308, 0.01); CHECK_NEAR(m.max_abs_error, 1.0, 1e-9); CHECK(m.ssim > 0.99);
    // CIEDE2000 reference pair from the Sharma et al. test data: (50, 2.6772, -79.7751) vs (50, 0, -82.7485) = 2.0425 (needs Lab input; here a sanity check on sRGB)
    uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    CHECK_EQ(nue_tex_compare_level(NUE_TEX_RGBA8_UNORM, red, NUE_TEX_RGBA8_UNORM, blue, 1, 1, 1, &m), NUE_OK);
    CHECK(m.delta_e_mean > 30 && m.delta_e_mean < 60);
}

TEST_CASE(executor_priorities_and_host_mode) {
    nue_tex_executor_desc d; NUE_STRUCT_INIT(&d); d.threads = 1;
    nue_tex_executor ex = nullptr;
    CHECK_EQ(nue_tex_executor_create(&d, &ex), NUE_OK);
    CHECK_EQ(nue_tex_executor_thread_count(ex), 1u);
    static std::atomic<bool> release; release = false;
    static std::mutex order_mutex; static std::string order;
    order.clear();
    nue_tex_executor_submit(ex, [](void*) { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }, nullptr, NUE_TEX_JOB_NORMAL);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));             // the blocker now occupies the only worker
    auto job = [](void* u) { std::lock_guard<std::mutex> l(order_mutex); order += static_cast<char>(reinterpret_cast<uintptr_t>(u)); };
    nue_tex_executor_submit(ex, job, reinterpret_cast<void*>('l'), NUE_TEX_JOB_LOW);
    nue_tex_executor_submit(ex, job, reinterpret_cast<void*>('n'), NUE_TEX_JOB_NORMAL);
    nue_tex_executor_submit(ex, job, reinterpret_cast<void*>('c'), NUE_TEX_JOB_CRITICAL);
    nue_tex_executor_submit(ex, job, reinterpret_cast<void*>('h'), NUE_TEX_JOB_HIGH);
    release = true;
    nue_tex_executor_wait_idle(ex);
    CHECK_STR(order.c_str(), "chnl");
    nue_tex_executor_destroy(ex);
    // host job system: jobs are handed over, nothing runs on our threads
    struct Host { std::vector<std::pair<nue_tex_job_fn, void*>> jobs; std::vector<int> prios; } host;
    nue_tex_executor_desc hd; NUE_STRUCT_INIT(&hd);
    hd.host_userdata = &host;
    hd.submit = [](void* hu, nue_tex_job_fn fn, void* ju, nue_tex_job_priority p) { auto* h = static_cast<Host*>(hu); h->jobs.push_back({fn, ju}); h->prios.push_back((int)p); };
    nue_tex_executor hx = nullptr;
    CHECK_EQ(nue_tex_executor_create(&hd, &hx), NUE_OK);
    static std::atomic<int> ran; ran = 0;
    nue_tex_executor_submit(hx, [](void*) { ++ran; }, nullptr, NUE_TEX_JOB_HIGH);
    CHECK_EQ(host.jobs.size(), 1u); CHECK_EQ(host.prios[0], (int)NUE_TEX_JOB_HIGH); CHECK_EQ(ran.load(), 0);
    host.jobs[0].first(host.jobs[0].second);
    CHECK_EQ(ran.load(), 1);
    nue_tex_executor_wait_idle(hx);
    nue_tex_executor_destroy(hx);
    // parallel encode gives the same bytes as the single thread encode
    nue_tex_executor_desc md; NUE_STRUCT_INIT(&md); md.threads = 4;
    nue_tex_executor mt = nullptr;
    nue_tex_executor_create(&md, &mt);
    const auto px = synth_rgba(130, 70, 6);
    for (nue_tex_format f : {NUE_TEX_ETC2_RGBA8, NUE_TEX_BC7_UNORM, NUE_TEX_ASTC_UNORM(4), NUE_TEX_BC1_UNORM}) {
        std::vector<uint8_t> one(nue_tex_level_size(f, 130, 70, 1)), many(one.size());
        nue_tex_encode_params ep; nue_tex_encode_params_default(&ep); ep.quality = NUE_TEX_QUALITY_FAST;
        CHECK_EQ(nue_tex_encode_level(f, 130, 70, NUE_TEX_RGBA8_UNORM, px.data(), 130 * 4, one.data(), one.size(), &ep), NUE_OK);
        ep.executor = mt;
        CHECK_EQ(nue_tex_encode_level(f, 130, 70, NUE_TEX_RGBA8_UNORM, px.data(), 130 * 4, many.data(), many.size(), &ep), NUE_OK);
        CHECK(one == many);
    }
    nue_tex_executor_destroy(mt);
}

TEST_CASE(context_cache_hit_miss_corrupt_trim) {
    const std::string dir = temp_dir("cache");
    nue_tex_caps caps; nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &caps);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.cache_dir = dir.c_str(); cd.worker_threads = 2; cd.flags = NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE;
    nue_tex_context cx = nullptr;
    CHECK_EQ(nue_tex_context_create(&cd, &cx), NUE_OK);
    nue_tex_image src = encode_image(NUE_TEX_BC3_UNORM, 64, 64, 4);
    nue_tex_convert_request rq; NUE_STRUCT_INIT(&rq); rq.source = src;
    nue_tex_image a = nullptr, b = nullptr; nue_bool hit = 1;
    nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
    CHECK_EQ(nue_tex_context_convert(cx, &rq, &a, &plan, &hit), NUE_OK);
    CHECK(!hit);
    CHECK_EQ(plan.action, NUE_TEX_ACTION_TRANSCODE);
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d); nue_tex_image_get_desc(a, &d);
    CHECK_EQ(d.format, plan.target_format); CHECK_EQ(d.mip_levels, 4u);
    CHECK_EQ(nue_tex_context_convert(cx, &rq, &b, nullptr, &hit), NUE_OK);
    CHECK(hit);
    CHECK(nue_tex_image_total_size(a) == nue_tex_image_total_size(b) && std::memcmp(level_data(a, 0), level_data(b, 0), size_t(nue_tex_image_total_size(a))) == 0);
    nue_tex_cache_stats st; NUE_STRUCT_INIT(&st); nue_tex_cache_get_stats(cx, &st);
    CHECK_EQ(st.hits, 1u); CHECK_EQ(st.misses, 1u); CHECK_EQ(st.stores, 1u); CHECK_EQ(st.entries, 1u); CHECK(st.bytes > 0);
    nue_tex_image_destroy(b);
    // a different policy produces a different key: miss
    nue_tex_policy pol; nue_tex_policy_default(&pol); pol.priority = NUE_TEX_PRIORITY_MEMORY;
    rq.policy = &pol;
    CHECK_EQ(nue_tex_context_convert(cx, &rq, &b, &plan, &hit), NUE_OK);
    CHECK(!hit); nue_tex_image_destroy(b);
    rq.policy = nullptr;
    // flip a byte inside the stored entry: it is detected, dropped and rebuilt
    std::string victim;
    for (auto& e : fs::recursive_directory_iterator(dir)) if (e.is_regular_file() && e.path().extension() == ".ktx2" && (victim.empty() || e.file_size() > fs::file_size(victim))) victim = e.path().string();
    CHECK(!victim.empty());
    { std::fstream f(victim, std::ios::in | std::ios::out | std::ios::binary); f.seekp(-5, std::ios::end); char c = 0x5A; f.write(&c, 1); }
    CHECK_EQ(nue_tex_context_convert(cx, &rq, &b, nullptr, &hit), NUE_OK);
    CHECK(!hit);
    nue_tex_cache_get_stats(cx, &st);
    CHECK_EQ(st.corrupt_dropped, 1u);
    CHECK_EQ(nue_tex_context_convert(cx, &rq, &b ? &b : &b, nullptr, &hit), NUE_OK);       // rebuilt entry is good again
    CHECK(hit);
    nue_tex_image_destroy(b);
    // trim and clear
    CHECK_EQ(nue_tex_cache_trim(cx, 1), NUE_OK);
    nue_tex_cache_get_stats(cx, &st);
    CHECK(st.entries <= 1u);
    CHECK_EQ(nue_tex_cache_clear(cx), NUE_OK);
    nue_tex_cache_scan(cx); nue_tex_cache_get_stats(cx, &st);
    CHECK_EQ(st.entries, 0u);
    // a desktop context needs no conversion for the same texture and does not touch the cache
    nue_tex_caps desk; nue_tex_caps_for_class(NUE_TEX_DEVICE_DESKTOP_BC, &desk);
    CHECK_EQ(nue_tex_context_set_caps(cx, &desk), NUE_OK);
    CHECK_EQ(nue_tex_context_convert(cx, &rq, &b, &plan, &hit), NUE_OK);
    CHECK_EQ(plan.action, NUE_TEX_ACTION_UPLOAD_AS_IS);
    nue_tex_image_destroy(a); nue_tex_image_destroy(b); nue_tex_image_destroy(src);
    nue_tex_context_destroy(cx);
}

struct AsyncResult { std::atomic<int> done{0}; nue_result r = 0; nue_tex_format fmt = NUE_TEX_FORMAT_UNKNOWN; nue_tex_request_id id = 0; nue_tex_image img = nullptr; };

TEST_CASE(async_requests_and_cancel) {
    nue_tex_caps caps; nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &caps);
    nue_tex_executor_desc ed; NUE_STRUCT_INIT(&ed); ed.threads = 1;
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.executor = &ed; cd.flags = NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE;
    nue_tex_context cx = nullptr;
    CHECK_EQ(nue_tex_context_create(&cd, &cx), NUE_OK);
    nue_tex_image src = encode_image(NUE_TEX_BC1_UNORM, 32, 32, 1, 3, false);
    nue_tex_convert_request rq; NUE_STRUCT_INIT(&rq); rq.source = src;
    auto cb = [](void* u, nue_tex_request_id id, nue_result r, nue_tex_image im, const nue_tex_plan* p) {
        auto* res = static_cast<AsyncResult*>(u);
        res->r = r; res->id = id; res->img = im; res->fmt = p ? p->target_format : NUE_TEX_FORMAT_UNKNOWN; res->done = 1;
    };
    AsyncResult first;
    nue_tex_request_id id = 0;
    CHECK_EQ(nue_tex_request_async(cx, &rq, NUE_TEX_JOB_HIGH, cb, &first, &id), NUE_OK_PENDING);
    CHECK(id != 0);
    nue_tex_wait_idle(cx);
    CHECK_EQ(first.done.load(), 1); CHECK_EQ(first.r, NUE_OK); CHECK_EQ(first.id, id); CHECK(first.img != nullptr);
    CHECK(nue_tex_format_is_compressed(first.fmt));
    nue_tex_image_destroy(first.img);
    // cancel a request that is still queued behind a blocker
    static std::atomic<bool> release; release = false;
    nue_tex_executor_submit(nue_tex_context_executor(cx), [](void*) { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }, nullptr, NUE_TEX_JOB_CRITICAL);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    AsyncResult second;
    nue_tex_request_id id2 = 0;
    CHECK_EQ(nue_tex_request_async(cx, &rq, NUE_TEX_JOB_LOW, cb, &second, &id2), NUE_OK_PENDING);
    CHECK_EQ(nue_tex_request_cancel(cx, id2), NUE_OK);
    CHECK_EQ(nue_tex_request_cancel(cx, 987654), NUE_OK_NOOP);
    release = true;
    nue_tex_wait_idle(cx);
    CHECK_EQ(second.done.load(), 1); CHECK_EQ(second.r, NUE_ERR_CANCELLED); CHECK(second.img == nullptr);
    nue_tex_image_destroy(src);
    nue_tex_context_destroy(cx);
}

struct ImportState { std::atomic<int> callbacks{0}; nue_tex_import_progress last; std::mutex m; };

TEST_CASE(import_pipeline_resume_pause_cancel) {
    const std::string dir = temp_dir("import_src"), cache = temp_dir("import_cache");
    for (int i = 0; i < 6; ++i) {
        nue_tex_image im = encode_image(NUE_TEX_BC3_UNORM, 64, 64, 3, 10 + i);
        CHECK_EQ(nue_tex_image_save_file(im, (dir + "/t" + std::to_string(i) + ".dds").c_str(), NUE_TEX_FILE_AUTO, 0), NUE_OK);
        nue_tex_image_destroy(im);
    }
    nue_tex_caps caps; nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &caps);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.cache_dir = cache.c_str(); cd.worker_threads = 2;
    nue_tex_context cx = nullptr;
    CHECK_EQ(nue_tex_context_create(&cd, &cx), NUE_OK);
    auto run = [&](bool pause_first, nue_tex_import_progress* out) {
        ImportState st; NUE_STRUCT_INIT(&st.last);
        nue_tex_importer imp = nullptr;
        CHECK_EQ(nue_tex_importer_create(cx, [](void* u, const nue_tex_import_progress* p) { auto* s = static_cast<ImportState*>(u); std::lock_guard<std::mutex> l(s->m); s->last = *p; ++s->callbacks; }, &st, &imp), NUE_OK);
        for (int i = 0; i < 6; ++i) {
            nue_tex_import_item it; NUE_STRUCT_INIT(&it);
            const std::string p = dir + "/t" + std::to_string(i) + ".dds";
            it.name = "tex"; it.path = p.c_str();
            CHECK_EQ(nue_tex_importer_add(imp, &it), NUE_OK);
        }
        nue_tex_import_item bad; NUE_STRUCT_INIT(&bad); std::string missing = dir + "/missing.dds"; bad.name = "missing"; bad.path = missing.c_str();
        CHECK_EQ(nue_tex_importer_add(imp, &bad), NUE_OK);
        if (pause_first) CHECK_EQ(nue_tex_importer_pause(imp, 1), NUE_OK);
        CHECK_EQ(nue_tex_importer_start(imp), NUE_OK);
        if (pause_first) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            nue_tex_import_progress p; NUE_STRUCT_INIT(&p); nue_tex_importer_get_progress(imp, &p);
            CHECK_EQ(p.done + p.failed, 0u); CHECK_EQ(p.total, 7u);
            CHECK_EQ(nue_tex_importer_wait(imp, 20), NUE_ERR_TIMEOUT);
            nue_tex_importer_pause(imp, 0);
        }
        CHECK_EQ(nue_tex_importer_wait(imp, 60000), NUE_OK);
        nue_tex_importer_get_progress(imp, out);
        CHECK(st.callbacks.load() >= 1);
        nue_tex_importer_destroy(imp);
    };
    nue_tex_import_progress p; NUE_STRUCT_INIT(&p);
    run(true, &p);
    CHECK_EQ(p.total, 7u); CHECK_EQ(p.done, 6u); CHECK_EQ(p.failed, 1u); CHECK_EQ(p.skipped_cached, 0u); CHECK(p.finished); CHECK(p.produced_bytes > 0);
    run(false, &p);                               // second start: everything is already in the cache
    CHECK_EQ(p.done, 6u); CHECK_EQ(p.skipped_cached, 6u);
    nue_tex_cache_stats st; NUE_STRUCT_INIT(&st); nue_tex_cache_get_stats(cx, &st);
    CHECK_EQ(st.entries, 6u);
    // cancel: running items finish, the rest is dropped, wait returns
    nue_tex_cache_clear(cx);
    nue_tex_importer imp = nullptr;
    nue_tex_importer_create(cx, nullptr, nullptr, &imp);
    for (int i = 0; i < 6; ++i) { nue_tex_import_item it; NUE_STRUCT_INIT(&it); const std::string pth = dir + "/t" + std::to_string(i) + ".dds"; it.path = pth.c_str(); it.name = "x"; nue_tex_importer_add(imp, &it); }
    nue_tex_importer_pause(imp, 1);
    nue_tex_importer_start(imp);
    nue_tex_importer_cancel(imp);
    CHECK_EQ(nue_tex_importer_wait(imp, 20000), NUE_OK);
    nue_tex_importer_get_progress(imp, &p);
    CHECK(p.cancelled); CHECK_EQ(p.done, 0u);
    CHECK_EQ(nue_tex_importer_add(imp, nullptr), NUE_ERR_INVALID_ARGUMENT);
    nue_tex_importer_destroy(imp);
    nue_tex_context_destroy(cx);
}

static uint64_t g_evicted_bytes;
static std::vector<nue_tex_resource_id> g_candidates;
static nue_tex_budget g_budget;

TEST_CASE(budget_lru_eviction_and_policies) {
    nue_tex_budget_policy pol; NUE_STRUCT_INIT(&pol);
    pol.mode = NUE_TEX_BUDGET_BYTES; pol.limit_bytes = 1000;
    g_evicted_bytes = 0; g_candidates.clear();
    nue_tex_budget b = nullptr;
    g_budget = nullptr;
    auto evict = [](void*, const nue_tex_resource_id* c, uint32_t n, uint64_t need) -> uint64_t {
        g_candidates.assign(c, c + n);
        // free the least recently used ones until `need` is met
        uint64_t freed = 0;
        for (uint32_t i = 0; i < n && freed < need; ++i) { freed += 400; nue_tex_budget_remove(g_budget, c[i]); }
        g_evicted_bytes += freed;
        return freed;
    };
    CHECK_EQ(nue_tex_budget_create(&pol, evict, nullptr, &b), NUE_OK);
    g_budget = b;
    nue_tex_resource_id a, c, d, e, pinned;
    CHECK_EQ(nue_tex_budget_add(b, 400, 0, &a), NUE_OK);
    CHECK_EQ(nue_tex_budget_add(b, 400, 0, &c), NUE_OK);
    CHECK_EQ(nue_tex_budget_add(b, 100, NUE_TEX_RES_PINNED, &pinned), NUE_OK);
    nue_tex_budget_touch(b, a);                                    // a is now more recent than c
    CHECK_EQ(nue_tex_budget_add(b, 400, 0, &d), NUE_OK);           // 1300 > 1000: over budget
    CHECK(g_candidates.size() >= 1 && g_candidates[0] == c);       // c is the least recently used, the pinned one is never offered
    for (auto id : g_candidates) CHECK(id != pinned);
    nue_tex_budget_stats st; NUE_STRUCT_INIT(&st); nue_tex_budget_get_stats(b, &st);
    CHECK_EQ(st.limit, 1000u); CHECK_EQ(st.used, 900u); CHECK_EQ(st.peak, 1300u); CHECK_EQ(st.over_budget_events, 1u); CHECK_EQ(st.pinned, 1u);
    CHECK_EQ(nue_tex_budget_remove(b, c), NUE_ERR_INVALID_HANDLE);          // stale ids fail cleanly
    CHECK_EQ(nue_tex_budget_resize(b, d, 100), NUE_OK);
    // lowering the limit at run time evicts
    pol.limit_bytes = 300;
    nue_tex_budget_set_policy(b, &pol);
    nue_tex_budget_get_stats(b, &st);
    CHECK(st.used <= 500u);
    (void)e;
    nue_tex_budget_destroy(b);
    // policy resolution: nothing is hard coded, everything derives from what the host reports
    nue_tex_budget_policy p; NUE_STRUCT_INIT(&p);
    CHECK_EQ(nue_tex_budget_resolve(&p), UINT64_MAX);
    p.mode = NUE_TEX_BUDGET_FRACTION; p.fraction = 0.25; p.available_bytes = 4000000;
    CHECK_EQ(nue_tex_budget_resolve(&p), 1000000u);
    p.min_bytes = 2000000;
    CHECK_EQ(nue_tex_budget_resolve(&p), 2000000u);
    nue_tex_budget_policy_preset(NUE_TEX_BUDGET_PRESET_LOW_MEMORY_MOBILE, 100ull << 20, &p);
    CHECK_EQ(nue_tex_budget_resolve(&p), 64ull << 20);                      // 12 % of 100 MiB is below the 64 MiB floor
    nue_tex_budget_policy_preset(NUE_TEX_BUDGET_PRESET_LOW_MEMORY_MOBILE, 4096ull << 20, &p);
    CHECK_NEAR(double(nue_tex_budget_resolve(&p)), 0.12 * (4096.0 * 1048576.0), 1e6);
    nue_tex_budget_policy_preset(NUE_TEX_BUDGET_PRESET_DESKTOP, 16384ull << 20, &p);
    CHECK_NEAR(double(nue_tex_budget_resolve(&p)), 0.35 * (16384.0 * 1048576.0), 1e6);
    nue_tex_budget_policy_preset(NUE_TEX_BUDGET_PRESET_UNLIMITED, 0, &p);
    CHECK_EQ(nue_tex_budget_resolve(&p), UINT64_MAX);
    // memory model: the numbers behind docs/TEXTURE.md
    nue_tex_memory_item items[] = {{NUE_TEX_BC1_UNORM, 512, 512, 0, 100}, {NUE_TEX_BC3_UNORM, 256, 256, 0, 200}};
    const uint64_t as_is = nue_tex_memory_estimate(items, 2, NUE_TEX_FORMAT_UNKNOWN), rgba8 = nue_tex_memory_estimate(items, 2, NUE_TEX_RGBA8_UNORM);
    CHECK(rgba8 > as_is * 5 && rgba8 < as_is * 8.2);
    std::printf("  300 textures: %.1f MB as DXT, %.1f MB as RGBA8, %.1f MB as ASTC 6x6\n", as_is / 1048576.0, rgba8 / 1048576.0, nue_tex_memory_estimate(items, 2, NUE_TEX_ASTC_UNORM(4)) / 1048576.0);
}

TEST_CASE(d3d9_seam) {
    const uint32_t DXT1 = 'D' | ('X' << 8) | ('T' << 16) | ('1' << 24), DXT5 = 'D' | ('X' << 8) | ('T' << 16) | ('5' << 24);
    nue_tex_caps caps; nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &caps);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.worker_threads = 2; cd.flags = NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE;      // development mode: encode on the spot
    nue_tex_context cx = nullptr;
    CHECK_EQ(nue_tex_context_create(&cd, &cx), NUE_OK);
    // DXT1 texture known to be opaque, full mip chain
    nue_tex_d3d9_texture_desc td; NUE_STRUCT_INIT(&td);
    td.d3d_format = DXT1; td.width = 128; td.height = 128; td.levels = 0; td.pool = 1; td.alpha_hint = NUE_TEX_ALPHA_INFO_OPAQUE;
    nue_tex_d3d9_texture t = nullptr; nue_tex_d3d9_texture_info info; NUE_STRUCT_INIT(&info); nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
    CHECK_EQ(nue_tex_d3d9_texture_create(cx, &td, &t, &info, &plan), NUE_OK);
    CHECK_EQ(info.source_format, NUE_TEX_BC1_UNORM); CHECK_EQ(info.gpu_format, NUE_TEX_ETC2_RGB8); CHECK_EQ(info.gpu_levels, 8u); CHECK_EQ(info.skip_mips, 0u);
    CHECK_EQ(info.gpu_bytes, nue_tex_chain_size(NUE_TEX_ETC2_RGB8, 128, 128, 1, 8, 1, 1));
    // upload level 0 the way UnlockRect would hand it over (D3D9 blocks, pitch in bytes)
    nue_tex_image full = encode_image(NUE_TEX_BC1_UNORM, 128, 128, 1, 8, false);
    std::vector<uint8_t> gpu(nue_tex_d3d9_level_gpu_size(t, 0, nullptr));
    CHECK_EQ(gpu.size(), size_t(nue_tex_level_size(NUE_TEX_ETC2_RGB8, 128, 128, 1)));
    uint32_t pitch = 0; size_t written = 0;
    CHECK_EQ(nue_tex_d3d9_convert_level(t, 0, 0, nullptr, level_data(full, 0), 128 / 4 * 8, gpu.data(), gpu.size(), &pitch, &written), NUE_OK);
    CHECK_EQ(written, gpu.size()); CHECK_EQ(pitch, 128u / 4 * 8);
    nue_tex_metrics m; NUE_STRUCT_INIT(&m);
    CHECK_EQ(nue_tex_compare_level(NUE_TEX_BC1_UNORM, level_data(full, 0), NUE_TEX_ETC2_RGB8, gpu.data(), 128, 128, 0, &m), NUE_OK);
    std::printf("  DXT1 -> ETC2 upload path: psnr %.1f dB\n", m.psnr_db);
    CHECK(m.psnr_db > 25.0);
    // a block aligned sub-rectangle (UpdateTexture / partial Lock)
    nue_rect_i r{32, 32, 32, 32};
    std::vector<uint8_t> part(nue_tex_d3d9_level_gpu_size(t, 0, &r));
    CHECK_EQ(part.size(), size_t(nue_tex_level_size(NUE_TEX_ETC2_RGB8, 32, 32, 1)));
    CHECK_EQ(nue_tex_d3d9_convert_level(t, 0, 0, &r, level_data(full, 0) + 8 * 32 / 4 * 8 + 32 / 4 * 8, 128 / 4 * 8, part.data(), part.size(), &pitch, &written), NUE_OK);
    nue_rect_i bad{3, 0, 32, 32};
    CHECK_EQ(nue_tex_d3d9_convert_level(t, 0, 0, &bad, level_data(full, 0), 128 / 4 * 8, part.data(), part.size(), &pitch, &written), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_tex_d3d9_convert_level(t, 20, 0, nullptr, level_data(full, 0), 8, part.data(), part.size(), &pitch, &written), NUE_ERR_INVALID_ARGUMENT);
    nue_tex_d3d9_texture_destroy(t);
    // unknown alpha: DXT1 may hold 1-bit alpha, so the plan picks a target that can keep it
    td.alpha_hint = NUE_TEX_ALPHA_INFO_UNKNOWN;
    CHECK_EQ(nue_tex_d3d9_texture_create(cx, &td, &t, &info, &plan), NUE_OK);
    CHECK_EQ(info.gpu_format, NUE_TEX_ASTC_UNORM(3));
    nue_tex_d3d9_texture_destroy(t);
    // DXT5 with sRGB sampling, ASTC target, slow encode flagged for a worker
    td.d3d_format = DXT5; td.alpha_hint = NUE_TEX_ALPHA_INFO_SMOOTH; td.srgb = NUE_TRUE; td.width = td.height = 512;
    CHECK_EQ(nue_tex_d3d9_texture_create(cx, &td, &t, &info, &plan), NUE_OK);
    CHECK_EQ(info.gpu_format, NUE_TEX_ASTC_4x4_SRGB); CHECK(info.defer_to_worker);
    nue_tex_d3d9_texture_destroy(t);
    // render targets and system memory textures
    td.d3d_format = 21; td.usage = 1; td.srgb = NUE_FALSE;
    CHECK_EQ(nue_tex_d3d9_texture_create(cx, &td, &t, &info, &plan), NUE_OK);
    CHECK_EQ(info.gpu_format, NUE_TEX_BGRA8_UNORM); CHECK_EQ(info.action, NUE_TEX_ACTION_UPLOAD_AS_IS);
    nue_tex_d3d9_texture_destroy(t);
    td.d3d_format = 22; td.usage = 0;                    // X8R8G8B8 (alpha padding)
    CHECK_EQ(nue_tex_d3d9_texture_create(cx, &td, &t, &info, &plan), NUE_OK);
    CHECK_EQ(info.source_format, NUE_TEX_BGRX8_UNORM);
    nue_tex_d3d9_texture_destroy(t);
    td.d3d_format = 9999;
    CHECK_EQ(nue_tex_d3d9_texture_create(cx, &td, &t, &info, &plan), NUE_ERR_UNSUPPORTED);
    td.d3d_format = DXT1; td.width = 0;
    CHECK_EQ(nue_tex_d3d9_texture_create(cx, &td, &t, &info, &plan), NUE_ERR_INVALID_ARGUMENT);
    nue_tex_image_destroy(full);
    nue_tex_context_destroy(cx);
}

TEST_MAIN_BEGIN
    RUN(mips_box_and_known_answers);
    RUN(mips_preserved_and_generated);
    RUN(alpha_coverage_preserved);
    RUN(image_convert_between_families_and_metrics);
    RUN(executor_priorities_and_host_mode);
    RUN(context_cache_hit_miss_corrupt_trim);
    RUN(async_requests_and_cancel);
    RUN(import_pipeline_resume_pause_cancel);
    RUN(budget_lru_eviction_and_policies);
    RUN(d3d9_seam);
TEST_MAIN_END
