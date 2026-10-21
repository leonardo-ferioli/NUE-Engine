// Frame-level hitch log: every frame Present hands its numbers to note_frame(); a frame slower than max(2.5 x the median of the last 64 frames, 40 ms)
// gets one "spike:" line in nue_d3d9.log with where its time went, and every 10 s with spikes there is a "spikes:" summary. Always on and cheap
// (a copy of a few counters per frame); at most 30 spike lines per 10 s.
#include "facade.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nue::facade {

std::atomic<unsigned long long> g_counter_destroy{0};

namespace {

enum Cause { cause_game, cause_dll, cause_waits, cause_present, cause_programs, cause_count };
const char* const kCauseNames[cause_count] = {"game code outside the dll", "calls into the dll", "waits for the worker", "Present (worker or render thread behind)", "new shader programs"};

struct Snapshot {
    unsigned long long qpc = 0, tsc = 0, waits = 0, wait_us = 0, copies = 0, texture_reads = 0, worker_us = 0, destroys = 0;
    unsigned draws = 0;
    d3d9::LayerStats layer{};
    uint64_t shader_builds = 0;
    double compile_ms = 0, cross_ms = 0;
    unsigned long long ticks[kMaxMethods] = {};
    unsigned calls[kMaxMethods] = {};
};

Snapshot g_previous, g_now;
bool g_have_previous = false;
float g_history[64];
unsigned g_history_count = 0, g_history_at = 0, g_frame_number = 0;
unsigned long long g_summary_qpc = 0;
unsigned g_spikes = 0, g_spikes_logged = 0, g_cause_spikes[cause_count] = {};
double g_worst_ms = 0;
char g_worst_call[64] = "";

void take(Snapshot& s, const d3d9::LayerStats& layer, const gpu::BgfxStats& bgfx) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    s.qpc = static_cast<unsigned long long>(q.QuadPart); s.tsc = __rdtsc();
    s.waits = g_worker_waits.load(std::memory_order_relaxed); s.wait_us = g_worker_wait_us.load(std::memory_order_relaxed);
    s.copies = g_worker_buffer_copies.load(std::memory_order_relaxed); s.texture_reads = g_worker_texture_reads.load(std::memory_order_relaxed);
    s.worker_us = g_worker_busy_total_us.load(std::memory_order_relaxed); s.destroys = g_counter_destroy.load(std::memory_order_relaxed);
    s.draws = g_counter_draw.load(std::memory_order_relaxed);
    s.layer = layer;
    s.shader_builds = bgfx.shader_builds; s.compile_ms = bgfx.compile_ms; s.cross_ms = bgfx.cross_ms;
    for (unsigned i = 0; i < kMaxMethods; ++i) { s.ticks[i] = g_methods[i].ticks; s.calls[i] = g_methods[i].calls; }
}

double median_of_history() {
    float sorted[64];
    std::copy(g_history, g_history + g_history_count, sorted);
    std::nth_element(sorted, sorted + g_history_count / 2, sorted + g_history_count);
    return sorted[g_history_count / 2];
}

}  // namespace

void note_frame(double frame_ms, double present_ms, const d3d9::LayerStats& layer, const gpu::BgfxStats& bgfx) {
    take(g_now, layer, bgfx);
    ++g_frame_number;
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    if (!g_summary_qpc) g_summary_qpc = g_now.qpc;
    const bool judged = g_have_previous && g_history_count >= 16 && frame_ms > 0;
    const double median = judged ? median_of_history() : 0;
    if (judged && frame_ms > std::max(2.5 * median, 40.0)) {
        const Snapshot& a = g_previous;
        const Snapshot& b = g_now;
        const double ms_per_tick = b.tsc > a.tsc ? double(b.qpc - a.qpc) * 1000.0 / double(frequency.QuadPart) / double(b.tsc - a.tsc) : 0;
        // the dll's own calls this frame, the slowest three, and what was created
        unsigned long long dll_ticks = 0;
        unsigned top[3] = {0, 0, 0};
        unsigned long long top_ticks[3] = {0, 0, 0};
        unsigned created_textures = 0, created_buffers = 0, created_declarations = 0, created_shaders = 0, created_other = 0;
        for (unsigned i = 0; i < kMaxMethods; ++i) {
            const unsigned long long t = b.ticks[i] - a.ticks[i];
            const unsigned calls = b.calls[i] - a.calls[i];
            dll_ticks += t;
            for (int k = 0; k < 3; ++k) if (t > top_ticks[k]) { for (int m = 2; m > k; --m) { top_ticks[m] = top_ticks[m - 1]; top[m] = top[m - 1]; } top_ticks[k] = t; top[k] = i; break; }
            const char* name = g_methods[i].name;
            if (!calls || !name || std::strncmp(name, "Create", 6) != 0) continue;
            if (!std::strcmp(name, "CreateTexture") || !std::strcmp(name, "CreateCubeTexture")) created_textures += calls;
            else if (!std::strcmp(name, "CreateVertexBuffer") || !std::strcmp(name, "CreateIndexBuffer")) created_buffers += calls;
            else if (!std::strcmp(name, "CreateVertexDeclaration")) created_declarations += calls;
            else if (!std::strcmp(name, "CreateVertexShader") || !std::strcmp(name, "CreatePixelShader")) created_shaders += calls;
            else created_other += calls;
        }
        const double dll_ms = double(dll_ticks) * ms_per_tick;
        char slowest[200] = "";
        size_t used = 0;
        for (int k = 0; k < 3 && top_ticks[k]; ++k)
            used += size_t(std::snprintf(slowest + used, sizeof slowest - used, "%s%s x%u %.2f ms", k ? ", " : "", g_methods[top[k]].name, b.calls[top[k]] - a.calls[top[k]], double(top_ticks[k]) * ms_per_tick));
        const double wait_ms = double(b.wait_us - a.wait_us) / 1000.0;
        const unsigned long long waits = b.waits - a.waits, reads = b.texture_reads - a.texture_reads, copies = b.copies - a.copies;
        const double program_ms = b.layer.program_ms - a.layer.program_ms;
        const double game_ms = std::max(0.0, frame_ms - dll_ms);
        // the biggest part names the spike
        const double parts[cause_count] = {game_ms, dll_ms - present_ms - wait_ms, wait_ms, present_ms, program_ms};
        int cause = 0;
        for (int k = 1; k < cause_count; ++k) if (parts[k] > parts[cause]) cause = k;
        ++g_spikes; ++g_cause_spikes[cause];
        if (frame_ms > g_worst_ms) { g_worst_ms = frame_ms; std::snprintf(g_worst_call, sizeof g_worst_call, "%s", top_ticks[0] ? g_methods[top[0]].name : ""); }
        if (g_spikes_logged < 30) {
            ++g_spikes_logged;
            const d3d9::LayerStats& la = a.layer;
            const d3d9::LayerStats& lb = b.layer;
            log("spike: frame %u took %.1f ms (median %.1f): game code outside the dll %.1f ms, inside the dll %.1f ms (Present %.2f; slowest calls: %s); "
                "waits for the worker outside Present %llu = %.2f ms (%llu texture levels read, %llu buffer copies, %llu other); worker %.2f ms busy; "
                "bgfx::frame() %.2f ms, render thread %.2f ms, GPU %.2f ms; %u draws; created %u textures, %u buffers, %u declarations, %u shaders, %u other; destroyed %llu; "
                "translator (the frame the worker last finished, in worker mode): %llu draws, %llu UP draws, dynamic vertices %.0f KB, buffer uploads %.0f KB, texture uploads %llu = %.0f KB, "
                "textures created %llu, new programs %llu = %.1f ms (shader builds %llu, compiler %.1f ms, cross-compiler %.1f ms) -> mostly %s",
                g_frame_number, frame_ms, median, game_ms, dll_ms, present_ms, slowest[0] ? slowest : "none",
                waits, wait_ms, reads, copies, waits >= reads + copies ? waits - reads - copies : 0ull, double(b.worker_us - a.worker_us) / 1000.0,
                bgfx.frame_call_ms, bgfx.render_cpu_ms, bgfx.gpu_ms, b.draws - a.draws,
                created_textures, created_buffers, created_declarations, created_shaders, created_other, b.destroys - a.destroys,
                static_cast<unsigned long long>(lb.draws - la.draws), static_cast<unsigned long long>(lb.draw_up - la.draw_up), double(lb.transient_bytes - la.transient_bytes) / 1024.0,
                double(lb.static_buffer_bytes - la.static_buffer_bytes) / 1024.0, static_cast<unsigned long long>(lb.texture_uploads - la.texture_uploads), double(lb.texture_upload_bytes - la.texture_upload_bytes) / 1024.0,
                static_cast<unsigned long long>(lb.textures_created - la.textures_created), static_cast<unsigned long long>(lb.programs - la.programs), program_ms,
                static_cast<unsigned long long>(b.shader_builds - a.shader_builds), b.compile_ms - a.compile_ms, b.cross_ms - a.cross_ms, kCauseNames[cause]);
        }
    }
    if (frame_ms > 0) { g_history[g_history_at] = float(frame_ms); g_history_at = (g_history_at + 1) % 64; g_history_count = std::min(g_history_count + 1, 64u); }
    // every 10 s: the spikes of that stretch, if any
    if (double(g_now.qpc - g_summary_qpc) / double(frequency.QuadPart) >= 10.0) {
        if (g_spikes) {
            char causes[400] = "";
            size_t used = 0;
            for (int k = 0; k < cause_count; ++k)
                if (g_cause_spikes[k]) used += size_t(std::snprintf(causes + used, sizeof causes - used, "%s%s %u", used ? ", " : "", kCauseNames[k], g_cause_spikes[k]));
            log("spikes: %u in the last %.0f s (%u logged), worst %.1f ms (slowest call in it: %s); mostly: %s", g_spikes, double(g_now.qpc - g_summary_qpc) / double(frequency.QuadPart),
                g_spikes_logged, g_worst_ms, g_worst_call[0] ? g_worst_call : "none", causes);
        }
        g_summary_qpc = g_now.qpc; g_spikes = 0; g_spikes_logged = 0; g_worst_ms = 0; g_worst_call[0] = 0;
        std::fill(g_cause_spikes, g_cause_spikes + cause_count, 0u);
    }
    std::swap(g_previous, g_now);
    g_have_previous = true;
}

}  // namespace nue::facade
