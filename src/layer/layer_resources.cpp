// Layer: device lifecycle, resource creation and destruction, locks and unlocks.
#include "layer/layer.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nue::d3d9 {
using namespace k;

std::unique_ptr<Layer> create_layer(gpu::Gpu& gpu, const LayerOptions& options) { return std::make_unique<LayerImpl>(gpu, options); }

LayerImpl::LayerImpl(gpu::Gpu& gpu, const LayerOptions& options) : gpu_(gpu), options_(options) { init_state(state_); mark_missing_ = options.mark_missing_textures; }

LayerImpl::~LayerImpl() {
    end_pass();
    for (auto& t : textures_.items) if (t.alive && t.gpu) gpu_.destroy(t.gpu);
    for (auto& b : buffers_.items) if (b.alive && b.gpu) gpu_.destroy(b.gpu);
    for (auto& s : surfaces_.items) if (s.alive && s.gpu) gpu_.destroy(s.gpu);
    if (dummy_2d_) gpu_.destroy(dummy_2d_);
    if (dummy_cube_) gpu_.destroy(dummy_cube_);
}

void LayerImpl::log(const std::string& message) { if (options_.log) options_.log(message.c_str()); }

void LayerImpl::note_unsupported(const char* what) {
    ++stats_.unsupported_calls;
    if (unsupported_[what]++ == 0 && options_.log_unsupported) log(std::string("unsupported: ") + what);
}

void LayerImpl::note_unsupported_literal(const char* what) {
    ++stats_.unsupported_calls;
    for (auto& e : unsupported_literals_) if (e.first == what) { ++e.second; return; }
    unsupported_literals_.emplace_back(what, 1);
    if (options_.log_unsupported) log(std::string("unsupported: ") + what);
}

gpu::Texture LayerImpl::checker_texture(bool cube) {
    gpu::Texture& slot = cube ? checker_cube_ : checker_2d_;
    if (slot) return slot;
    auto r = gpu_.create_texture(8, 8, 1, gpu::Format::rgba8, false, cube);
    if (!r) return dummy_texture(cube);
    slot = r.value;
    uint8_t texels[8 * 8 * 4];
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
        const bool on = ((x >> 2) ^ (y >> 2)) & 1;
        uint8_t* p = texels + (y * 8 + x) * 4;
        p[0] = on ? 255 : 0; p[1] = 0; p[2] = on ? 255 : 0; p[3] = 255;   // magenta and black
    }
    for (uint32_t face = 0; face < (cube ? 6u : 1u); ++face) gpu_.update_texture(slot, 0, face, nullptr, Bytes(reinterpret_cast<const std::byte*>(texels), sizeof texels), 32);
    return slot;
}

void LayerImpl::note_texture_use(uint32_t id, uint32_t stage, uint32_t pixel_shader, bool stale) {
    for (TextureUse& u : uses_) if (u.texture == id) { ++u.draws; u.stage_mask |= 1u << stage; u.stale |= stale; return; }
    uses_.push_back(TextureUse{id, 1u << stage, 1, uint32_t(stats_.draws - capture_first_draw_), stage, pixel_shader, stale});
}

// One line per texture the draws of one frame sampled: what it is, whether it has data, a hash of level 0 to tell textures (and a texture that changed) apart, and the first draw that used it.
std::string LayerImpl::build_texture_report() {
    std::sort(uses_.begin(), uses_.end(), [](const TextureUse& a, const TextureUse& b) { return a.first_draw < b.first_draw; });
    uint32_t no_data = 0, zero = 0, stale = 0, sysmem = 0, partial = 0;
    std::string lines;
    for (const TextureUse& u : uses_) {
        const TextureObject* t = textures_.get(u.texture);
        if (!t) continue;
        const uint32_t all = t->levels >= 32 ? ~0u : ((1u << t->levels) - 1u);
        const uint32_t up = t->uploaded_levels & all;
        const bool empty = !t->render_target && !(up & 1u);
        if (!t->gpu) ++sysmem; else if (empty) ++no_data;
        if (!t->render_target && t->zero0 && (up & 1u)) ++zero;
        if (u.stale) ++stale;
        if (!t->render_target && up && up != all) ++partial;
        const FormatInfo& info = *t->info;
        char fourcc[16];
        if (info.d3d > 0xFFFF) std::snprintf(fourcc, sizeof fourcc, "%c%c%c%c", char(info.d3d & 0xFF), char((info.d3d >> 8) & 0xFF), char((info.d3d >> 16) & 0xFF), char((info.d3d >> 24) & 0xFF));
        else std::snprintf(fourcc, sizeof fourcc, "%s", info.name ? info.name : "?");
        char line[600];
        std::snprintf(line, sizeof line,
                      "tex#%u %ux%u%s %s levels %u (data in mask %x of %x) pool %u usage %x gpu fmt %d%s%s%s | uploads %u (last frame %u, after first use %u, relocks %u) hash0 %016llx%s partial writes %u%s%s | first draw %u stage %u pixel shader #%u, %u draws, stages %x\n",
                      u.texture, t->width, t->height, t->cube ? " cube" : "", fourcc, t->levels, up, all, t->pool, t->usage, int(t->gpu_format), t->render_target ? " RENDER-TARGET" : "", t->gpu ? "" : " SYSTEM-MEMORY-ONLY",
                      empty ? " NO-DATA" : "", t->uploads, t->last_upload_frame, t->updates_after_use, t->relocks, (unsigned long long)t->hash0, t->zero0 && (up & 1u) ? " ALL-ZERO" : "", t->partial_writes0,
                      u.stale ? (t->written_frame == 0xFFFFFFFFu ? " SAMPLED-BUT-NEVER-RENDERED" : " SAMPLED-BEFORE-RENDERED-THIS-FRAME") : "", up && up != all && !t->render_target ? " MIPS-MISSING" : "", u.first_draw, u.first_stage, u.first_ps, u.draws, u.stage_mask);
        lines += line;
    }
    char head[400];
    std::snprintf(head, sizeof head, "texture report of frame %u: %zu textures sampled by %llu draws; without data %u, all-zero level 0 %u, system-memory only %u, mip levels missing %u, render targets sampled before rendered %u\n",
                  frame_, uses_.size(), (unsigned long long)(stats_.draws - capture_first_draw_), no_data, zero, sysmem, partial, stale);
    return std::string(head) + lines;
}

void LayerImpl::note_create_failure(const char* what, uint32_t width, uint32_t height, uint32_t levels, uint32_t format) {
    const uint64_t n = stats_.texture_create_failures + stats_.buffer_create_failures;
    if (n <= 20 || n % 200 == 0) log(std::string("FAILED to create a ") + what + " " + std::to_string(width) + "x" + std::to_string(height) + " levels " + std::to_string(levels) + " d3d format " + std::to_string(format) +
                                     " (failure " + std::to_string(n) + "; the object will be missing in the picture)");
}

// Textures alive by format and size, how many have received data, and the ones sampled before any data arrived. Called from the game thread.
std::string LayerImpl::audit() const {
    struct Group { uint32_t format, width, height, levels; uint32_t count = 0, uploaded = 0, partial = 0, empty = 0; };
    std::vector<Group> groups;
    uint32_t alive = 0, in_gpu = 0, sysmem = 0, rts = 0, cubes = 0, empty = 0, partial = 0, locked = 0;
    for (const TextureObject& t : textures_.items) {
        if (!t.alive) continue;
        ++alive;
        if (!t.gpu) { ++sysmem; continue; }
        ++in_gpu;
        if (t.cube) ++cubes;
        if (t.render_target) { ++rts; continue; }
        for (const auto& l : t.locks) if (l.active) { ++locked; break; }
        const uint32_t all = t.levels >= 32 ? ~0u : ((1u << t.levels) - 1u);
        const bool none = (t.uploaded_levels & all) == 0, full = (t.uploaded_levels & all) == all;
        if (none) ++empty; else if (!full) ++partial;
        Group* g = nullptr;
        for (Group& x : groups) if (x.format == t.format && x.width == t.width && x.height == t.height && x.levels == t.levels) { g = &x; break; }
        if (!g) { groups.push_back(Group{t.format, t.width, t.height, t.levels}); g = &groups.back(); }
        ++g->count; if (none) ++g->empty; else if (full) ++g->uploaded; else ++g->partial;
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.count > b.count; });
    char head[900];
    std::snprintf(head, sizeof head,
                  "textures: %u alive (%u on the GPU: %u render targets, %u cube; %u system-memory only), created %llu, failed %llu; sampled-before-upload %llu textures; never uploaded %u, partly uploaded (some mip levels missing) %u, locked now %u; "
                  "buffer failures %llu; uploads %llu (%.1f MB), draws skipped %llu; levels locked again with data %llu (%llu without DISCARD), uploaded after first use %llu, render targets sampled before rendered %llu, "
                  "UpdateTexture calls %llu, kept copies %.1f MB, draws with a missing texture shown as checker %llu",
                  alive, in_gpu, rts, cubes, sysmem, (unsigned long long)stats_.textures_created, (unsigned long long)stats_.texture_create_failures, (unsigned long long)stats_.sampled_before_upload, empty, partial, locked,
                  (unsigned long long)stats_.buffer_create_failures, (unsigned long long)stats_.texture_uploads, double(stats_.texture_upload_bytes) / 1048576.0, (unsigned long long)stats_.draws_skipped,
                  (unsigned long long)stats_.relocks_with_data, (unsigned long long)stats_.relocks_without_discard, (unsigned long long)stats_.texture_updates_after_use, (unsigned long long)stats_.rt_sampled_stale,
                  (unsigned long long)stats_.texture_update_calls, double(copy_bytes_) / 1048576.0, (unsigned long long)stats_.missing_texture_draws);
    std::string text = head;
    for (size_t i = 0; i < groups.size() && i < 8; ++i) {
        const Group& g = groups[i];
        char line[200];
        char name[16];
        if (g.format > 0xFFFF) std::snprintf(name, sizeof name, "%c%c%c%c", char(g.format & 0xFF), char((g.format >> 8) & 0xFF), char((g.format >> 16) & 0xFF), char((g.format >> 24) & 0xFF));   // FourCC (DXT1...)
        else std::snprintf(name, sizeof name, "%u", g.format);
        std::snprintf(line, sizeof line, "\n    format %s %ux%u levels %u: %u alive (%u complete, %u partial, %u empty)", name, g.width, g.height, g.levels, g.count, g.uploaded, g.partial, g.empty);
        text += line;
    }
    return text;
}

std::string LayerImpl::report() const {
    std::string text;
    for (const auto& e : unsupported_literals_) text += "  " + std::string(e.first) + ": " + std::to_string(e.second) + "\n";
    for (const auto& [what, count] : unsupported_) text += "  " + what + ": " + std::to_string(count) + "\n";
    return text.empty() ? "  nothing unsupported was met\n" : text;
}

void LayerImpl::init_state(DeviceState& st) {
    st = DeviceState{};
    auto& rs = st.rs;
    rs[rs_zenable] = 1; rs[rs_fillmode] = fill_solid; rs[rs_shademode] = 2; rs[rs_zwriteenable] = 1; rs[rs_alphatestenable] = 0; rs[rs_lastpixel] = 1;
    rs[rs_srcblend] = blend_one; rs[rs_destblend] = blend_zero; rs[rs_cullmode] = cull_ccw; rs[rs_zfunc] = cmp_lessequal; rs[rs_alpharef] = 0; rs[rs_alphafunc] = cmp_always;
    rs[rs_ditherenable] = 0; rs[rs_alphablendenable] = 0; rs[rs_fogenable] = 0; rs[rs_specularenable] = 0; rs[rs_fogcolor] = 0; rs[rs_fogtablemode] = 0;
    rs[rs_fogstart] = 0; rs[rs_fogend] = 0x3F800000; rs[rs_fogdensity] = 0x3F800000; rs[rs_rangefogenable] = 0;
    rs[rs_stencilenable] = 0; rs[rs_stencilfail] = stencil_keep; rs[rs_stencilzfail] = stencil_keep; rs[rs_stencilpass] = stencil_keep; rs[rs_stencilfunc] = cmp_always;
    rs[rs_stencilref] = 0; rs[rs_stencilmask] = 0xFFFFFFFF; rs[rs_stencilwritemask] = 0xFFFFFFFF; rs[rs_texturefactor] = 0xFFFFFFFF;
    rs[rs_clipping] = 1; rs[rs_lighting] = 1; rs[rs_ambient] = 0; rs[rs_colorvertex] = 1; rs[rs_localviewer] = 1; rs[rs_normalizenormals] = 0;
    rs[rs_diffusematerialsource] = 1; rs[rs_specularmaterialsource] = 2; rs[rs_ambientmaterialsource] = 0; rs[rs_emissivematerialsource] = 0;
    rs[rs_colorwriteenable] = 0xF; rs[rs_colorwriteenable1] = rs[rs_colorwriteenable2] = rs[rs_colorwriteenable3] = 0xF;
    rs[rs_blendop] = blendop_add; rs[rs_blendfactor] = 0xFFFFFFFF; rs[rs_srgbwriteenable] = 0; rs[rs_depthbias] = 0; rs[rs_slopescaledepthbias] = 0;
    rs[rs_separatealphablendenable] = 0; rs[rs_srcblendalpha] = blend_one; rs[rs_destblendalpha] = blend_zero; rs[rs_blendopalpha] = blendop_add;
    rs[rs_scissortestenable] = 0; rs[rs_multisamplemask] = 0xFFFFFFFF; rs[rs_pointsize] = 0x3F800000; rs[rs_pointsize_min] = 0x3F800000; rs[rs_pointsize_max] = 0x42800000;
    rs[rs_pointscale_a] = 0x3F800000;
    for (auto& s : st.ss) { s[ss_addressu] = s[ss_addressv] = s[ss_addressw] = taddress_wrap; s[ss_magfilter] = s[ss_minfilter] = texf_point; s[ss_mipfilter] = texf_none; s[ss_maxanisotropy] = 1; }
    for (uint32_t stage = 0; stage < 8; ++stage) {
        auto& t = st.tss[stage];
        t[tss_colorop] = stage == 0 ? 4 : 1; t[tss_colorarg1] = 2; t[tss_colorarg2] = 1; t[tss_alphaop] = stage == 0 ? 2 : 1; t[tss_alphaarg1] = 2; t[tss_alphaarg2] = 1;
        t[tss_texcoordindex] = stage; t[tss_resultarg] = 1; t[tss_colorarg0] = t[tss_alphaarg0] = 1;
    }
    auto identity = [](float* m) { std::memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; };
    identity(st.view); identity(st.projection); identity(st.world);
    for (auto& m : st.texture) identity(m);
}

// ---- lifecycle ---------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kUnknownQuality = 0xFFFFFFFFu;

// Direct3D 9 multisample type -> sample count. NONMASKABLE (1) means "the driver's quality level `quality`": the levels of the hardware the game was
// written against are 2x, 4x, 8x in that order. NUE_MSAA=N forces a count (tests, and a way to turn it off with 1).
uint32_t LayerImpl::samples_for(uint32_t type, uint32_t quality) const {
    if (type == 0) return 1;   // D3DMULTISAMPLE_NONE stays single-sampled whatever is forced
    uint32_t forced = options_.msaa_override;
    if (const char* e = std::getenv("NUE_MSAA")) forced = uint32_t(std::atoi(e));
    if (forced) return std::min(std::clamp<uint32_t>(forced, 1, 16), std::max(gpu_.caps().max_samples, 1u));
    uint32_t n = 1;
    if (type == 1) n = quality >= 4 ? 8 : quality >= 2 ? 4 : 2;   // levels 0-1 -> 2 samples, 2-3 -> 4, 4 and above -> 8 (an unknown quality, as in the recorded traces, means the highest)
    if (type == 1 && quality == kUnknownQuality) n = 8;
    else if (type >= 2) n = type;
    n = n >= 16 ? 16 : n >= 8 ? 8 : n >= 3 ? 4 : n;
    return std::min(n, std::max(gpu_.caps().max_samples, 1u));
}

// Render targets and depth surfaces the game creates with a multisample type: the same quality as the back buffer when the type is the same (Direct3D 9 only
// lets a multisampled target be used with a depth surface of the same type, and the creation calls carry no quality here).
uint32_t LayerImpl::surface_samples(uint32_t multisample_type) const {
    if (multisample_type == 0) return 1;
    if (multisample_type == params_.multisample) return backbuffer_samples_;
    return samples_for(multisample_type, kUnknownQuality);
}

void LayerImpl::recreate_auto_depth(uint32_t width, uint32_t height) {
    if (auto_depth_surface_) {
        if (auto* s = surfaces_.get(auto_depth_surface_)) { if (s->gpu) gpu_.destroy(s->gpu); }
        surfaces_.release(auto_depth_surface_);
        auto_depth_surface_ = 0;
    }
    if (!params_.depth_format) return;
    const FormatInfo& info = format_info(params_.depth_format);
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::depth; s.width = width; s.height = height; s.format = params_.depth_format;
    auto r = gpu_.create_texture(width, height, 1, info.valid ? info.native : gpu::Format::d24s8, true, false, backbuffer_samples_);
    s.gpu = r.value;
    auto_depth_surface_ = surfaces_.add(std::move(s));
}

Status LayerImpl::reset(const DeviceParams& p) {
    end_pass();
    const bool resized = p.back_width != params_.back_width || p.back_height != params_.back_height;
    params_ = p;
    { char text[200]; std::snprintf(text, sizeof text, "device parameters: back buffer %ux%u format %u, depth format %u, multisample type %u, %u buffers, %s, present interval %x", p.back_width, p.back_height, p.back_format, p.depth_format, p.multisample, p.buffers, p.windowed ? "windowed" : "fullscreen", p.present_interval); log(text); }
    if (p.back_width && p.back_height) {
        const uint32_t samples = samples_for(p.multisample, p.multisample_quality);
        { char text[200]; std::snprintf(text, sizeof text, "multisample: requested type %u quality %u -> %u samples (the renderer allows up to %u)", p.multisample, p.multisample_quality, samples, gpu_.caps().max_samples); log(text); }
        if (samples != backbuffer_samples_) { backbuffer_samples_ = samples; if (auto_depth_surface_) { resized_for_samples_ = true; } }
        gpu_.set_backbuffer_samples(backbuffer_samples_);
        gpu_.resize(p.back_width, p.back_height);
        if (!format_has_alpha(p.back_format)) init_opaque_target(gpu::backbuffer);
        if (!backbuffer_surface_) {
            SurfaceObject s; s.alive = true; s.kind = SurfaceObject::Kind::backbuffer; s.width = p.back_width; s.height = p.back_height; s.format = p.back_format;
            backbuffer_surface_ = surfaces_.add(std::move(s));
        } else if (auto* s = surfaces_.get(backbuffer_surface_)) { s->width = p.back_width; s->height = p.back_height; s->format = p.back_format; }
        if (resized || resized_for_samples_ || !auto_depth_surface_) { recreate_auto_depth(p.back_width, p.back_height); resized_for_samples_ = false; }
        state_.render_targets[0] = backbuffer_surface_;
        state_.depth_stencil = auto_depth_surface_;
        state_.viewport = Viewport{0, 0, p.back_width, p.back_height, 0.0f, 1.0f};
    }
    return Status::ok;
}

Status LayerImpl::present() {
    end_pass();
    if (capturing_) { texture_report_ = build_texture_report(); report_ready_ = true; capturing_ = false; }   // the frame that just ended is the one that was asked for
    if (capture_request_) { capture_request_ = false; capturing_ = true; uses_.clear(); capture_first_draw_ = stats_.draws; }
    gpu_.present(params_.present_interval);
    ++stats_.frames;
    ++frame_;
    for (auto& b : retired_buffers_) gpu_.destroy(b);
    retired_buffers_.clear();
    for (auto& t : pending_destroy_textures_) gpu_.destroy(t);
    pending_destroy_textures_.clear();
    return Status::ok;
}

// ---- textures and surfaces -----------------------------------------------------------------------------------------------------------------

static gpu::Format render_target_format(const FormatInfo& info) {
    if (!info.valid) return gpu::Format::bgra8;
    if (info.depth) return info.native;
    switch (info.d3d) {
    case fmt_a8r8g8b8: case fmt_x8r8g8b8: return gpu::Format::bgra8;
    case fmt_r5g6b5: return gpu::Format::r5g6b5;
    case fmt_a16b16g16r16f: return gpu::Format::rgba16f;
    case fmt_a32b32g32r32f: return gpu::Format::rgba32f;
    case fmt_r16f: return gpu::Format::r16f;
    case fmt_r32f: return gpu::Format::r32f;
    case fmt_g16r16f: return gpu::Format::rg16f;
    case fmt_g32r32f: return gpu::Format::rg32f;
    case fmt_a8: return gpu::Format::a8;
    default: return gpu::Format::bgra8;
    }
}

Result<TextureId> LayerImpl::create_texture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) {
    const FormatInfo& info = format_info(format);
    if (!info.valid || !width || !height) { note_unsupported(("create_texture format " + std::to_string(format)).c_str()); return {Status::unsupported, {}}; }
    TextureObject t;
    t.alive = true; t.width = width; t.height = height; t.usage = usage; t.format = format; t.pool = pool; t.info = &info;
    t.render_target = (usage & (usage_rendertarget | usage_depthstencil)) != 0;
    t.levels = levels ? levels : mip_count(width, height);
    if (t.render_target && std::getenv("NUE_LOG_RT")) std::fprintf(stderr, "render target texture %ux%u levels %u format %u usage %x%c", width, height, t.levels, format, usage, 10);
    if (usage & usage_autogenmipmap) t.levels = std::max(t.levels, 1u);
    if (pool == pool_systemmem || pool == pool_scratch) {
        t.locks.resize(t.levels);
        t.contents.resize(t.locks.size());
        return {Status::ok, TextureId{textures_.add(std::move(t))}};
    }
    t.gpu_format = t.render_target ? render_target_format(info) : gpu_format_for(info, gpu_.caps());
    auto r = gpu_.create_texture(width, height, t.render_target ? 1 : t.levels, t.gpu_format, t.render_target, false);
    if (!r) { ++stats_.texture_create_failures; note_create_failure("texture", width, height, t.levels, format); return {r.status, {}}; }
    ++stats_.textures_created;
    if (t.render_target) t.uploaded_levels = ~0u;
    t.gpu = r.value;
    t.locks.resize(t.levels);
    t.contents.resize(t.locks.size());
    return {Status::ok, TextureId{textures_.add(std::move(t))}};
}

Result<TextureId> LayerImpl::create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) {
    const FormatInfo& info = format_info(format);
    if (!info.valid || !edge) { note_unsupported(("create_cube_texture format " + std::to_string(format)).c_str()); return {Status::unsupported, {}}; }
    TextureObject t;
    t.alive = true; t.cube = true; t.width = t.height = edge; t.usage = usage; t.format = format; t.pool = pool; t.info = &info;
    t.render_target = (usage & usage_rendertarget) != 0;
    t.levels = levels ? levels : mip_count(edge, edge);
    t.gpu_format = t.render_target ? render_target_format(info) : gpu_format_for(info, gpu_.caps());
    auto r = gpu_.create_texture(edge, edge, t.levels, t.gpu_format, t.render_target, true);
    if (!r) { ++stats_.texture_create_failures; note_create_failure("cube texture", edge, edge, t.levels, format); return {r.status, {}}; }
    ++stats_.textures_created;
    if (t.render_target) t.uploaded_levels = ~0u;
    t.gpu = r.value;
    t.locks.resize(size_t(t.levels) * 6);
    t.contents.resize(t.locks.size());
    return {Status::ok, TextureId{textures_.add(std::move(t))}};
}

Result<SurfaceId> LayerImpl::create_render_target(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, bool) {
    const FormatInfo& info = format_info(format);
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::render_target; s.width = width; s.height = height; s.format = format;
    auto r = gpu_.create_texture(width, height, 1, render_target_format(info), true, false, surface_samples(multisample));
    if (!r) return {r.status, {}};
    s.gpu = r.value;
    if (!format_has_alpha(format)) init_opaque_target(s.gpu);
    return {Status::ok, SurfaceId{surfaces_.add(std::move(s))}};
}

Result<SurfaceId> LayerImpl::create_depth_stencil(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample) {
    const FormatInfo& info = format_info(format);
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::depth; s.width = width; s.height = height; s.format = format;
    auto r = gpu_.create_texture(width, height, 1, info.valid ? info.native : gpu::Format::d24s8, true, false, surface_samples(multisample));
    if (!r) return {r.status, {}};
    s.gpu = r.value;
    return {Status::ok, SurfaceId{surfaces_.add(std::move(s))}};
}

Result<SurfaceId> LayerImpl::texture_surface(TextureId id, uint32_t level, uint32_t face) {
    TextureObject* t = textures_.get(id.value);
    if (!t) { bad_handle("texture_surface"); return {Status::invalid_argument, {}}; }
    SurfaceObject s;
    s.alive = true; s.kind = SurfaceObject::Kind::texture_level; s.texture = id.value; s.level = level; s.face = face;
    s.width = mip_dimension(t->width, level); s.height = mip_dimension(t->height, level); s.format = t->format;
    return {Status::ok, SurfaceId{surfaces_.add(std::move(s))}};
}

Result<SurfaceId> LayerImpl::back_buffer(uint32_t, uint32_t) {
    if (!backbuffer_surface_) {
        SurfaceObject s; s.alive = true; s.kind = SurfaceObject::Kind::backbuffer; s.width = params_.back_width; s.height = params_.back_height; s.format = params_.back_format;
        backbuffer_surface_ = surfaces_.add(std::move(s));
    }
    return {Status::ok, SurfaceId{backbuffer_surface_}};
}

void LayerImpl::destroy(TextureId id) {
    TextureObject* t = textures_.get(id.value);
    if (!t) return;
    for (uint32_t s = 0; s < state_.textures.size(); ++s) if (state_.textures[s] == id.value) { state_.textures[s] = 0; if (s < 16) cube_mask_ &= ~(1u << s); }
    ++program_epoch_;
    if (t->gpu) pending_destroy_textures_.push_back(t->gpu);
    for (const auto& c : t->contents) if (t->gpu) copy_bytes_ -= std::min<uint64_t>(copy_bytes_, c.size());
    stats_.texture_copy_bytes = copy_bytes_;
    textures_.release(id.value);
}

void LayerImpl::destroy(BufferId id) {
    BufferObject* b = buffers_.get(id.value);
    if (!b) return;
    for (auto& st : state_.streams) if (st.buffer == id.value) st = {};
    if (state_.index_buffer == id.value) state_.index_buffer = 0;
    if (b->gpu) retired_buffers_.push_back(b->gpu);
    buffers_.release(id.value);
}

void LayerImpl::destroy(SurfaceId id) {
    SurfaceObject* s = surfaces_.get(id.value);
    if (!s || id.value == backbuffer_surface_ || id.value == auto_depth_surface_) return;
    for (auto& r : state_.render_targets) if (r == id.value) r = 0;
    if (state_.depth_stencil == id.value) state_.depth_stencil = 0;
    if (s->gpu) pending_destroy_textures_.push_back(s->gpu);
    surfaces_.release(id.value);
}

void LayerImpl::destroy(DeclarationId id) {
    ++program_epoch_;
    if (state_.declaration == id.value) state_.declaration = 0;
    declarations_.release(id.value);
}
void LayerImpl::destroy(ShaderId id) {
    ++program_epoch_;
    if (state_.vertex_shader == id.value) state_.vertex_shader = 0;
    if (state_.pixel_shader == id.value) state_.pixel_shader = 0;
    shaders_.release(id.value);
}
void LayerImpl::destroy(StateBlockId id) { blocks_.release(id.value); }

// ---- buffers --------------------------------------------------------------------------------------------------------------------------------

Result<BufferId> LayerImpl::create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) {
    BufferObject b;
    b.alive = true; b.bytes = bytes; b.usage = usage; b.format_or_fvf = fvf; b.pool = pool;
    b.shadow.assign(bytes, std::byte{0});
    return {Status::ok, BufferId{buffers_.add(std::move(b))}};
}

Result<BufferId> LayerImpl::create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) {
    BufferObject b;
    b.alive = true; b.index = true; b.bytes = bytes; b.usage = usage; b.format_or_fvf = format; b.pool = pool;
    b.shadow.assign(bytes, std::byte{0});
    return {Status::ok, BufferId{buffers_.add(std::move(b))}};
}

Result<LockedRegion> LayerImpl::lock_buffer(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags) {
    BufferObject* b = buffers_.get(id.value);
    if (!b) { bad_handle("lock_buffer"); return {Status::invalid_argument, {}}; }
    if (offset > b->bytes) return {Status::invalid_argument, {}};
    if (!bytes) bytes = b->bytes - offset;
    if (uint64_t(offset) + bytes > b->bytes) return {Status::invalid_argument, {}};
    b->locked = true; b->lock_offset = offset; b->lock_bytes = bytes; b->lock_flags = flags;
    return {Status::ok, LockedRegion{b->shadow.data() + offset, bytes, 0}};
}

Status LayerImpl::unlock_buffer(BufferId id) {
    BufferObject* b = buffers_.get(id.value);
    if (!b) { bad_handle("unlock_buffer"); return Status::invalid_argument; }
    if (!b->locked) return Status::invalid_argument;
    b->locked = false;
    if (b->lock_flags & lock_readonly) return Status::ok;
    ++b->version;
    if (b->score_frame != frame_) { const uint32_t decay = (frame_ - b->score_frame) / 4; b->score = b->score > decay ? b->score - decay : 0; b->score_frame = frame_; }
    if (b->used_since_update) b->score = std::min(b->score + 2, 8u);   // rewritten after having been drawn from: it behaves as a dynamic buffer
    b->used_since_update = false;
    return Status::ok;
}

// ---- texture locks --------------------------------------------------------------------------------------------------------------------------

Result<LockedRegion> LayerImpl::lock_texture(TextureId id, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) {
    TextureObject* t = textures_.get(id.value);
    if (!t || level >= t->levels || face >= (t->cube ? 6u : 1u)) { bad_handle("lock_texture"); return {Status::invalid_argument, {}}; }
    if (t->render_target) { note_unsupported("lock of a render target texture"); return {Status::unsupported, {}}; }
    auto& lock = t->locks[size_t(face) * t->levels + level];
    if (lock.active) return {Status::invalid_argument, {}};
    const FormatInfo& info = *t->info;
    const uint32_t lw = mip_dimension(t->width, level), lh = mip_dimension(t->height, level);
    uint32_t x = 0, y = 0, w = lw, h = lh;
    if (rect4) {
        x = uint32_t(std::max(0, rect4[0])); y = uint32_t(std::max(0, rect4[1]));
        w = uint32_t(std::max(0, rect4[2] - rect4[0])); h = uint32_t(std::max(0, rect4[3] - rect4[1]));
        if (x + w > lw) w = lw - std::min(x, lw);
        if (y + h > lh) h = lh - std::min(y, lh);
    }
    lock.active = true; lock.flags = flags;
    lock.rect[0] = x; lock.rect[1] = y; lock.rect[2] = w; lock.rect[3] = h;
    lock.pitch = level_pitch(info, w);
    const size_t size = level_bytes(info, w, h);
    lock.staging.assign(size, std::byte{0});
    const size_t index = size_t(face) * t->levels + level;
    const std::vector<std::byte>& copy = t->contents[index];
    const bool has_data = !copy.empty() || (t->uploaded_levels & (1u << std::min(level, 31u)));
    if (has_data) {
        ++stats_.relocks_with_data; ++t->relocks;
        if (!(flags & (k::lock_discard | lock_readonly))) ++stats_.relocks_without_discard;
    }
    if (!copy.empty() && !(flags & k::lock_discard)) {   // the lock shows what the texture holds: a game that changes a part of a level gets the rest as it was
        const uint32_t full_pitch = level_pitch(info, lw), rows = (h + info.block_h - 1) / info.block_h, row_bytes = ((w + info.block_w - 1) / info.block_w) * info.block_bytes;
        for (uint32_t r = 0; r < rows; ++r) {
            const size_t from = size_t(y / info.block_h + r) * full_pitch + size_t(x / info.block_w) * info.block_bytes;
            if (from + row_bytes <= copy.size()) std::memcpy(lock.staging.data() + size_t(r) * lock.pitch, copy.data() + from, row_bytes);
        }
    }
    return {Status::ok, LockedRegion{lock.staging.data(), lock.pitch, 0}};
}

static uint64_t hash_bytes(const std::byte* p, size_t n, bool& all_zero) {
    uint64_t h = 1469598103934665603ull, acc = 0;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); acc |= v; h = (h ^ v) * 1099511628211ull; h ^= h >> 29; }
    for (; i < n; ++i) { acc |= uint8_t(p[i]); h = (h ^ uint8_t(p[i])) * 1099511628211ull; }
    all_zero = acc == 0;
    return h;
}

void LayerImpl::upload_level_region(TextureObject& t, uint32_t level, uint32_t face, const uint32_t rect[4], bool whole, const std::byte* data, size_t bytes, uint32_t pitch, uint32_t w, uint32_t h) {
    const FormatInfo& info = *t.info;
    const bool same_layout = info.native == t.gpu_format && info.convert != Convert::x8_opaque;
    if (same_layout) {
        gpu_.update_texture(t.gpu, level, face, whole ? nullptr : rect, Bytes(data, bytes), pitch);
        stats_.texture_upload_bytes += bytes;
    } else {
        uint32_t out_pitch = 0;
        convert_pixels(info, t.gpu_format, reinterpret_cast<const uint8_t*>(data), pitch, w, h, scratch_, out_pitch);
        gpu_.update_texture(t.gpu, level, face, whole ? nullptr : rect, Bytes(scratch_.data(), scratch_.size()), out_pitch);
        stats_.texture_upload_bytes += scratch_.size();
        stats_.texture_converted_bytes += scratch_.size();
    }
    ++stats_.texture_uploads;
    t.uploaded_levels |= level < 32 ? (1u << level) : 0u;
    ++t.uploads;
    t.last_upload_frame = frame_;
    if (t.used) { ++t.updates_after_use; ++stats_.texture_updates_after_use; }
    if (level == 0 && face == 0) {
        if (whole) t.hash0 = hash_bytes(data, bytes, t.zero0); else ++t.partial_writes0;
    }
}

// Keeps what a level holds, in the game's own format, so that a later Lock can show it (the managed pool of the real runtime keeps a system copy of every texture).
void LayerImpl::retain_copy(TextureObject& t, uint32_t index, uint32_t level, const uint32_t rect[4], bool whole, std::vector<std::byte>& staging, uint32_t pitch) {
    const FormatInfo& info = *t.info;
    auto& copy = t.contents[index];
    const uint32_t lw = mip_dimension(t.width, level), lh = mip_dimension(t.height, level);
    const uint32_t full_pitch = level_pitch(info, lw);
    const size_t full = level_bytes(info, lw, lh);
    if (copy.empty()) {
        const uint64_t budget = uint64_t(options_.texture_copy_mb) << 20;
        if (t.gpu && copy_bytes_ + full > budget) return;   // over the budget: this level cannot be read back
        if (t.gpu) copy_bytes_ += full;
        if (whole) { copy.swap(staging); stats_.texture_copy_bytes = copy_bytes_; return; }
        copy.assign(full, std::byte{0});
    }
    if (whole && staging.size() == copy.size()) { copy.swap(staging); return; }
    const uint32_t rows = (rect[3] + info.block_h - 1) / info.block_h, row_bytes = ((rect[2] + info.block_w - 1) / info.block_w) * info.block_bytes;
    for (uint32_t r = 0; r < rows; ++r) {
        const size_t to = size_t(rect[1] / info.block_h + r) * full_pitch + size_t(rect[0] / info.block_w) * info.block_bytes;
        if (to + row_bytes <= copy.size() && size_t(r) * pitch + row_bytes <= staging.size()) std::memcpy(copy.data() + to, staging.data() + size_t(r) * pitch, row_bytes);
    }
    stats_.texture_copy_bytes = copy_bytes_;
}

Status LayerImpl::unlock_texture(TextureId id, uint32_t level, uint32_t face) {
    TextureObject* t = textures_.get(id.value);
    if (!t || level >= t->levels || face >= (t->cube ? 6u : 1u)) { bad_handle("unlock_texture"); return Status::invalid_argument; }
    const size_t index = size_t(face) * t->levels + level;
    auto& lock = t->locks[index];
    if (!lock.active) return Status::invalid_argument;
    lock.active = false;
    const uint32_t w = lock.rect[2], h = lock.rect[3];
    if ((lock.flags & lock_readonly) || !w || !h) { lock.staging.clear(); lock.staging.shrink_to_fit(); return Status::ok; }
    const uint32_t lw = mip_dimension(t->width, level), lh = mip_dimension(t->height, level);
    const bool whole = lock.rect[0] == 0 && lock.rect[1] == 0 && w == lw && h == lh;
    const uint32_t rect[4] = {lock.rect[0], lock.rect[1], w, h};
    if (t->gpu) upload_level_region(*t, level, face, rect, whole, lock.staging.data(), lock.staging.size(), lock.pitch, w, h);
    else { t->uploaded_levels |= level < 32 ? (1u << level) : 0u; ++t->uploads; t->last_upload_frame = frame_; }
    retain_copy(*t, uint32_t(index), level, rect, whole, lock.staging, lock.pitch);
    lock.staging.clear(); lock.staging.shrink_to_fit();
    return Status::ok;
}

// UpdateTexture: the levels of a system-memory (or managed) texture go to the default-pool texture that has the same top size.
Status LayerImpl::update_texture(TextureId source, TextureId destination) {
    ++stats_.texture_update_calls;
    TextureObject* s = textures_.get(source.value);
    TextureObject* d = textures_.get(destination.value);
    if (!s || !d || !d->gpu || d->render_target || s->cube != d->cube || s->format != d->format) { bad_handle("update_texture"); return Status::invalid_argument; }
    uint32_t skip = 0;   // levels of the source above the destination's top level
    while (skip < s->levels && (mip_dimension(s->width, skip) != d->width || mip_dimension(s->height, skip) != d->height)) ++skip;
    if (skip >= s->levels) skip = 0;
    for (uint32_t face = 0; face < (d->cube ? 6u : 1u); ++face) {
        for (uint32_t level = 0; level < d->levels && level + skip < s->levels; ++level) {
            const auto& copy = s->contents[size_t(face) * s->levels + level + skip];
            if (copy.empty()) continue;
            const uint32_t lw = mip_dimension(d->width, level), lh = mip_dimension(d->height, level);
            const uint32_t rect[4] = {0, 0, lw, lh};
            upload_level_region(*d, level, face, rect, true, copy.data(), copy.size(), level_pitch(*d->info, lw), lw, lh);
            std::vector<std::byte> again;
            if (d->contents[size_t(face) * d->levels + level].empty()) { again = copy; retain_copy(*d, uint32_t(size_t(face) * d->levels + level), level, rect, true, again, level_pitch(*d->info, lw)); }
        }
    }
    return Status::ok;
}

// ---- declarations and shaders ---------------------------------------------------------------------------------------------------------------

void finalize_declaration(Declaration& d) {
    d.stream_count = 0;
    for (uint32_t s = 0; s < 16; ++s) if ((d.stream_mask & (1u << s)) && d.stream_count < 4) d.stream_order[d.stream_count++] = uint8_t(s);
    d.remapped.clear(); d.flat0.clear();
    for (const gpu::VertexAttribute& a : d.attributes) {
        gpu::VertexAttribute flat = a;
        flat.stream = 0;
        d.flat0.push_back(flat);
        for (uint32_t i = 0; i < d.stream_count; ++i) if (d.stream_order[i] == a.stream) { gpu::VertexAttribute r = a; r.stream = uint8_t(i); d.remapped.push_back(r); }
    }
}

Result<DeclarationId> LayerImpl::create_vertex_declaration(std::span<const uint8_t> bytes) {
    Declaration d;
    for (size_t pos = 0; pos + 8 <= bytes.size(); pos += 8) {
        const uint8_t* e = bytes.data() + pos;
        uint16_t stream, offset;
        std::memcpy(&stream, e, 2); std::memcpy(&offset, e + 2, 2);
        if (stream == 0xFF) break;   // D3DDECL_END
        DeclElement el{uint8_t(stream), offset, e[4], e[6], e[7]};
        d.elements.push_back(el);
        d.attributes.push_back(gpu::VertexAttribute{el.stream, el.offset, el.type, el.usage, el.usage_index});
        d.stream_mask |= 1u << (el.stream & 15);
        if (el.type == dt_d3dcolor) d.bgra_attributes |= 1u << shader::bgfx_attribute_for(el.usage, el.usage_index);
        if (el.type == dt_ubyte4) d.uint_attributes |= 1u << shader::bgfx_attribute_for(el.usage, el.usage_index);
        if (el.type == dt_short2 || el.type == dt_short4) d.sint_attributes |= 1u << shader::bgfx_attribute_for(el.usage, el.usage_index);
    }
    finalize_declaration(d);
    ++program_epoch_;   // the table may have moved: cached pointers are dropped
    return {Status::ok, DeclarationId{declarations_.add(std::move(d))}};
}

static uint64_t hash_words(std::span<const uint32_t> w) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t v : w) { h ^= v; h *= 0x100000001b3ull; }
    return h;
}

Result<ShaderId> LayerImpl::create_vertex_shader(std::span<const uint32_t> bytecode) {
    ShaderObject s;
    s.vertex = true; s.bytecode.assign(bytecode.begin(), bytecode.end()); s.hash = hash_words(bytecode);
    s.valid = shader::decode(bytecode, s.program, s.error) && s.program.stage == shader::Stage::vertex;
    if (!s.valid) { ++stats_.shader_failures; log("vertex shader rejected: " + s.error); }
    ++program_epoch_;
    return {Status::ok, ShaderId{shaders_.add(std::move(s))}};
}

Result<ShaderId> LayerImpl::create_pixel_shader(std::span<const uint32_t> bytecode) {
    ShaderObject s;
    s.vertex = false; s.bytecode.assign(bytecode.begin(), bytecode.end()); s.hash = hash_words(bytecode);
    s.valid = shader::decode(bytecode, s.program, s.error) && s.program.stage == shader::Stage::pixel;
    if (!s.valid) { ++stats_.shader_failures; log("pixel shader rejected: " + s.error); }
    ++program_epoch_;
    return {Status::ok, ShaderId{shaders_.add(std::move(s))}};
}

Status LayerImpl::stretch_rect(SurfaceId from, const int32_t* fr, SurfaceId to, const int32_t* tr, uint32_t filter) {
    ++stats_.stretch_rects;
    if (stats_.stretch_rects < 12) {
        SurfaceObject* a = surfaces_.get(from.value); SurfaceObject* b = surfaces_.get(to.value);
        char text[256];
        std::snprintf(text, sizeof text, "stretch_rect #%llu: %s %ux%u fmt %u rect %d -> %s %ux%u fmt %u rect %d filter %u", (unsigned long long)stats_.stretch_rects,
                      a ? (a->kind == SurfaceObject::Kind::backbuffer ? "backbuffer" : a->kind == SurfaceObject::Kind::render_target ? "rt" : a->kind == SurfaceObject::Kind::texture_level ? "texture" : "depth") : "?", a ? a->width : 0, a ? a->height : 0, a ? a->format : 0, fr ? 1 : 0,
                      b ? (b->kind == SurfaceObject::Kind::backbuffer ? "backbuffer" : b->kind == SurfaceObject::Kind::render_target ? "rt" : b->kind == SurfaceObject::Kind::texture_level ? "texture" : "depth") : "?", b ? b->width : 0, b ? b->height : 0, b ? b->format : 0, tr ? 1 : 0, filter);
        log(text);
    }
    auto texture_of = [&](SurfaceObject* surface) -> gpu::Texture {
        if (!surface) return {};
        switch (surface->kind) {
        case SurfaceObject::Kind::backbuffer: return gpu::backbuffer;
        case SurfaceObject::Kind::render_target: return surface->gpu;
        case SurfaceObject::Kind::texture_level: { if (surface->level != 0) return {}; auto* t = textures_.get(surface->texture); return t ? t->gpu : gpu::Texture{}; }
        default: return {};
        }
    };
    const gpu::Texture src = texture_of(surfaces_.get(from.value)), dst = texture_of(surfaces_.get(to.value));
    if (!src || !dst) { note_unsupported("stretch_rect between these surface kinds"); return Status::unsupported; }
    end_pass();
    auto rect = [](const int32_t* r, int32_t out[4]) { out[0] = r[0]; out[1] = r[1]; out[2] = r[2] - r[0]; out[3] = r[3] - r[1]; };
    int32_t sr[4], dr[4];
    if (fr) rect(fr, sr);
    if (tr) rect(tr, dr);
    if (options_.debug_log_passes && options_.log) { char t[200]; std::snprintf(t, sizeof t, "copy: surface %u (gpu %u) -> surface %u (gpu %u)", from.value, src.value, to.value, dst.value); log(t); }
    gpu_.copy_texture(dst, tr ? dr : nullptr, src, fr ? sr : nullptr, filter == texf_linear);
    if (SurfaceObject* to_surface = surfaces_.get(to.value); to_surface && to_surface->kind == SurfaceObject::Kind::texture_level) if (auto* t = textures_.get(to_surface->texture)) { t->uploaded_levels |= 1u; t->written_frame = frame_; }
    return Status::ok;
}

}  // namespace nue::d3d9
