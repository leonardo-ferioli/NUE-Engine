#pragma once
#include "internal.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace nuetex {

struct Job { nue_tex_job_fn fn; void* user; };

struct Executor {
    explicit Executor(const nue_tex_executor_desc& d);
    ~Executor();
    void submit(nue_tex_job_fn fn, void* user, nue_tex_job_priority pr);
    void wait_idle();
    void finished_one();
    void worker_main();

    void (NUE_CALL *host_submit)(void*, nue_tex_job_fn, void*, nue_tex_job_priority) = nullptr;
    void* host_user = nullptr;
    uint32_t nthreads = 1;
    std::string name;
    std::vector<std::thread> workers;
    std::mutex m;
    std::condition_variable cv, idle_cv;
    std::deque<Job> queues[4];
    uint64_t outstanding = 0;
    bool stop = false;
};

}  // namespace nuetex
