// Job executor with priorities (own worker threads, or a host job system through desc.submit) and parallel_for on top of it.
#include "executor.hpp"
#include <thread>

namespace nuetex {

Executor::Executor(const nue_tex_executor_desc& d) : host_submit(d.submit), host_user(d.host_userdata) {
    if (host_submit) { nthreads = std::max(1u, d.threads ? d.threads : std::thread::hardware_concurrency()); return; }
    nthreads = d.threads ? d.threads : std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
    name = d.name ? d.name : "nue-tex";
    for (uint32_t i = 0; i < nthreads; ++i) workers.emplace_back([this] { worker_main(); });
}

Executor::~Executor() {
    wait_idle();
    { std::lock_guard<std::mutex> l(m); stop = true; }
    cv.notify_all();
    for (auto& t : workers) t.join();
}

void Executor::submit(nue_tex_job_fn fn, void* user, nue_tex_job_priority pr) {
    if (host_submit) {
        { std::lock_guard<std::mutex> l(m); ++outstanding; }
        struct Tramp { Executor* ex; nue_tex_job_fn fn; void* user; };
        auto* t = new Tramp{this, fn, user};
        host_submit(host_user, [](void* u) {
            auto* t = static_cast<Tramp*>(u);
            t->fn(t->user);
            Executor* ex = t->ex;
            delete t;
            ex->finished_one();
        }, t, pr);
        return;
    }
    {
        std::lock_guard<std::mutex> l(m);
        queues[std::min<int>(pr, 3)].push_back(Job{fn, user});
        ++outstanding;
    }
    cv.notify_one();
}

void Executor::finished_one() {
    std::lock_guard<std::mutex> l(m);
    if (--outstanding == 0) idle_cv.notify_all();
}

void Executor::worker_main() {
    for (;;) {
        Job job{nullptr, nullptr};
        {
            std::unique_lock<std::mutex> l(m);
            cv.wait(l, [&] { return stop || queues[0].size() + queues[1].size() + queues[2].size() + queues[3].size() > 0; });
            bool got = false;
            for (int p = 3; p >= 0 && !got; --p)
                if (!queues[p].empty()) { job = queues[p].front(); queues[p].pop_front(); got = true; }
            if (!got) { if (stop) return; continue; }
        }
        job.fn(job.user);
        finished_one();
    }
}

void Executor::wait_idle() {
    std::unique_lock<std::mutex> l(m);
    idle_cv.wait(l, [&] { return outstanding == 0; });
}

namespace {
struct PState {
    void (*fn)(void*, uint32_t, uint32_t);
    void* user;
    uint32_t n, chunk, chunks;
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> done{0};
    std::mutex m;
    std::condition_variable cv;
    void run_chunks() {
        for (;;) {
            const uint32_t c = next.fetch_add(1);
            if (c >= chunks) return;
            const uint32_t b = c * chunk, e = std::min(n, b + chunk);
            fn(user, b, e);
            if (done.fetch_add(1) + 1 == chunks) { std::lock_guard<std::mutex> l(m); cv.notify_all(); }
        }
    }
};
}

void parallel_for(nue_tex_executor exh, uint32_t n, uint32_t min_chunk, uint32_t max_threads, void (*fn)(void*, uint32_t, uint32_t), void* user) {
    if (!n) return;
    Executor* ex = reinterpret_cast<Executor*>(exh);
    uint32_t threads = ex ? ex->nthreads + 1 : 1;     // the caller works too
    if (max_threads) threads = std::min(threads, max_threads);
    min_chunk = std::max(1u, min_chunk);
    if (!ex || threads <= 1 || n <= min_chunk) { fn(user, 0, n); return; }
    const uint32_t chunks = std::min(threads * 2, (n + min_chunk - 1) / min_chunk);       // 2 chunks per thread balances uneven blocks
    if (chunks <= 1) { fn(user, 0, n); return; }
    auto st = std::make_shared<PState>();
    st->fn = fn; st->user = user; st->n = n; st->chunks = chunks; st->chunk = (n + chunks - 1) / chunks;
    st->chunks = (n + st->chunk - 1) / st->chunk;
    const uint32_t helpers = std::min(threads - 1, st->chunks - 1);
    for (uint32_t i = 0; i < helpers; ++i) {
        auto* keep = new std::shared_ptr<PState>(st);
        ex->submit([](void* u) { auto* p = static_cast<std::shared_ptr<PState>*>(u); (*p)->run_chunks(); delete p; }, keep, NUE_TEX_JOB_NORMAL);
    }
    st->run_chunks();
    std::unique_lock<std::mutex> l(st->m);
    st->cv.wait(l, [&] { return st->done.load() >= st->chunks; });
}

}  // namespace nuetex

extern "C" {

nue_result NUE_CALL nue_tex_executor_create(const nue_tex_executor_desc* desc, nue_tex_executor* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    nue_tex_executor_desc d{};
    d.struct_size = sizeof d;
    if (desc) std::memcpy(&d, desc, std::min<size_t>(desc->struct_size, sizeof d));
    *out = reinterpret_cast<nue_tex_executor>(new nuetex::Executor(d));
    return NUE_OK;
}
void NUE_CALL nue_tex_executor_destroy(nue_tex_executor e) { delete reinterpret_cast<nuetex::Executor*>(e); }
nue_result NUE_CALL nue_tex_executor_submit(nue_tex_executor e, nue_tex_job_fn fn, void* user, nue_tex_job_priority pr) {
    if (!e || !fn) return NUE_ERR_INVALID_ARGUMENT;
    reinterpret_cast<nuetex::Executor*>(e)->submit(fn, user, pr);
    return NUE_OK;
}
void NUE_CALL nue_tex_executor_wait_idle(nue_tex_executor e) { if (e) reinterpret_cast<nuetex::Executor*>(e)->wait_idle(); }
uint32_t NUE_CALL nue_tex_executor_thread_count(nue_tex_executor e) { return e ? reinterpret_cast<nuetex::Executor*>(e)->nthreads : 0; }

}  // extern "C"
