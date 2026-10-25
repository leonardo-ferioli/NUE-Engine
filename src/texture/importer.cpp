// First-run import pipeline: convert every texture once into the cache, in the background, with progress, pause, cancel and resume.
#include "context.hpp"
#include <condition_variable>
#include <deque>
#include <thread>

namespace nuetex {
namespace {

struct Item {
    std::string name, path;
    nue_result (NUE_CALL* load)(void*, nue_tex_image*);
    void* user;
    nue_tex_usage usage;
    uint32_t flags;
};

struct Importer {
    Context* c;
    nue_tex_import_cb cb; void* cb_user;
    std::mutex m;
    std::condition_variable cv;
    std::deque<Item> queue;
    std::thread feeder;
    bool started = false, cancelled = false, paused = false, stop = false;
    uint32_t classes = 0;               // device classes to cook for (0 = the context's own device)
    uint32_t in_flight = 0;
    uint32_t total = 0, done = 0, skipped = 0, failed = 0;
    uint64_t source_bytes = 0, produced_bytes = 0;
    std::string current;
    std::chrono::steady_clock::time_point t0, last_cb;

    void fill(nue_tex_import_progress* p) {
        std::memset(p, 0, sizeof *p);
        p->struct_size = sizeof *p;
        p->total = total; p->done = done; p->skipped_cached = skipped; p->failed = failed;
        p->source_bytes = source_bytes; p->produced_bytes = produced_bytes;
        p->elapsed_seconds = started ? std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() : 0.0;
        const uint32_t finished_items = done + failed;
        p->eta_seconds = finished_items ? p->elapsed_seconds / finished_items * (total - finished_items) : 0.0;
        p->running = started && !cancelled && (finished_items < total || in_flight);
        p->cancelled = cancelled;
        p->finished = started && finished_items >= total && !in_flight;
        std::snprintf(p->current, sizeof p->current, "%s", current.c_str());
    }
    void notify(bool force) {
        if (!cb) return;
        nue_tex_import_progress p;
        {
            std::lock_guard<std::mutex> l(m);
            const auto now = std::chrono::steady_clock::now();
            const bool fin = started && done + failed >= total && !in_flight;
            if (!force && !fin && now - last_cb < std::chrono::milliseconds(100)) return;
            last_cb = now;
            fill(&p);
        }
        cb(cb_user, &p);
    }

    void run_item(Item it) {
        Image* src = nullptr;
        bool ok = true;
        {
            std::lock_guard<std::mutex> l(m);
            current = it.name;
        }
        nue_tex_image h = nullptr;
        nue_result r;
        { ProfScope pf(P_LOAD); r = it.load ? it.load(it.user, &h) : nue_tex_image_load_file(it.path.c_str(), NUE_TEX_FILE_AUTO, 0, &h); }
        if (r < 0 || !h) { ok = false; log(4, "import %s: cannot load (%s)", it.name.c_str(), "load error"); }
        else src = img(h);
        bool hit = false;
        uint64_t sbytes = 0, pbytes = 0;
        if (ok) {
            sbytes = src->data.size();
            ConvertOptions o;
            o.source = src; o.usage = it.usage; o.flags = it.flags; o.alpha_info = NUE_TEX_ALPHA_INFO_UNKNOWN; o.policy = c->policy;
            o.allow_encode = true; o.cooking = true;
            const uint32_t cls = [&] { std::lock_guard<std::mutex> l(m); return classes; }();
            if (cls) {                              // all formats at once: one variant per distinct target of the chosen device classes
                nue_tex_cook_result cr; std::memset(&cr, 0, sizeof cr); cr.struct_size = sizeof cr;
                r = cook_image(*c, o, cls, &cr);
                if (r < 0) { ok = false; log(4, "import %s: %u variant(s) failed", it.name.c_str(), cr.failed); }
                hit = cr.stored == 0 && cr.failed == 0;
                pbytes = cr.bytes_written;
            } else {
                Image* res = nullptr;
                r = context_convert(*c, o, &res, nullptr, &hit);
                if (r < 0) { ok = false; log(4, "import %s: conversion failed (%d)", it.name.c_str(), (int)r); }
                else { pbytes = res->data.size(); delete res; }
            }
            delete src;
        }
        {
            std::lock_guard<std::mutex> l(m);
            if (ok) { ++done; if (hit) ++skipped; } else ++failed;
            source_bytes += sbytes; produced_bytes += pbytes;
            --in_flight;
        }
        cv.notify_all();
        notify(false);
    }

    void feeder_main() {
        for (;;) {
            Item it;
            {
                std::unique_lock<std::mutex> l(m);
                const uint32_t limit = std::max(1u, c->executor->nthreads);
                cv.wait(l, [&] { return stop || cancelled || (!paused && !queue.empty() && in_flight < limit); });
                if (stop || cancelled) {
                    queue.clear();
                    cv.wait(l, [&] { return in_flight == 0; });
                    return;
                }
                it = std::move(queue.front());
                queue.pop_front();
                ++in_flight;
            }
            struct Ctx { Importer* imp; Item item; };
            auto* j = new Ctx{this, std::move(it)};
            c->executor->submit([](void* u) { std::unique_ptr<Ctx> j(static_cast<Ctx*>(u)); j->imp->run_item(std::move(j->item)); }, j, NUE_TEX_JOB_LOW);
        }
    }

    bool finished_locked() const { return started && (cancelled ? in_flight == 0 : (done + failed >= total && in_flight == 0)); }
};

}  // namespace
}  // namespace nuetex

using namespace nuetex;

extern "C" {

nue_result NUE_CALL nue_tex_importer_create(nue_tex_context cx, nue_tex_import_cb cb, void* userdata, nue_tex_importer* out) {
    if (!cx || !out) return NUE_ERR_INVALID_ARGUMENT;
    auto* imp = new Importer();
    imp->c = ctx(cx); imp->cb = cb; imp->cb_user = userdata;
    imp->last_cb = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    *out = reinterpret_cast<nue_tex_importer>(imp);
    return NUE_OK;
}

void NUE_CALL nue_tex_importer_destroy(nue_tex_importer h) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp) return;
    {
        std::lock_guard<std::mutex> l(imp->m);
        imp->stop = true;
    }
    imp->cv.notify_all();
    if (imp->feeder.joinable()) imp->feeder.join();
    else {                                      // never started: nothing runs
    }
    delete imp;
}

nue_result NUE_CALL nue_tex_importer_add(nue_tex_importer h, const nue_tex_import_item* item) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp || !item || (!item->path && !item->load)) return NUE_ERR_INVALID_ARGUMENT;
    Item it;
    it.name = item->name ? item->name : (item->path ? item->path : "item");
    it.path = item->path ? item->path : "";
    it.load = item->load; it.user = item->userdata; it.usage = item->usage; it.flags = item->flags;
    {
        std::lock_guard<std::mutex> l(imp->m);
        if (imp->cancelled) return NUE_ERR_CANCELLED;
        imp->queue.push_back(std::move(it));
        ++imp->total;
    }
    imp->cv.notify_all();
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_importer_set_classes(nue_tex_importer h, uint32_t class_mask) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp) return NUE_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> l(imp->m);
    imp->classes = class_mask & NUE_TEX_CLASS_MASK_ALL;
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_importer_start(nue_tex_importer h) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp) return NUE_ERR_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> l(imp->m);
        if (imp->started) return NUE_OK_NOOP;
        imp->started = true;
        imp->t0 = std::chrono::steady_clock::now();
    }
    imp->feeder = std::thread([imp] { imp->feeder_main(); });
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_importer_cancel(nue_tex_importer h) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp) return NUE_ERR_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> l(imp->m);
        imp->cancelled = true;
    }
    imp->cv.notify_all();
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_importer_pause(nue_tex_importer h, nue_bool paused) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp) return NUE_ERR_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> l(imp->m);
        imp->paused = paused != 0;
    }
    imp->cv.notify_all();
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_importer_get_progress(nue_tex_importer h, nue_tex_import_progress* out) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp || !out) return NUE_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> l(imp->m);
    imp->fill(out);
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_importer_wait(nue_tex_importer h, int64_t timeout_ms) {
    auto* imp = reinterpret_cast<Importer*>(h);
    if (!imp) return NUE_ERR_INVALID_HANDLE;
    std::unique_lock<std::mutex> l(imp->m);
    auto pred = [&] { return imp->finished_locked(); };
    if (timeout_ms < 0) { imp->cv.wait(l, pred); return NUE_OK; }
    return imp->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), pred) ? NUE_OK : NUE_ERR_TIMEOUT;
}

}  // extern "C"
