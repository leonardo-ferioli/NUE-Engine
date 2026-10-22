// Small fixed-size thread pool (portable, header only). Used for asynchronous file reads when
// io_uring is not available or not wanted, and for any background work of the Linux backend.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nue::lx {

class TaskQueue {
public:
    // `on_thread_start(index)` runs first on every worker (name the thread, lower its priority...).
    explicit TaskQueue(size_t threads, std::function<void(size_t)> on_thread_start = nullptr) {
        if (threads == 0) threads = 1;
        for (size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this, i, on_thread_start] {
                if (on_thread_start) on_thread_start(i);
                run();
            });
    }
    ~TaskQueue() { shutdown(); }
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // Returns false after shutdown().
    bool post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (stop_) return false;
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
        return true;
    }

    // Runs everything already queued, then joins the workers. Idempotent.
    void shutdown() {
        {
            std::lock_guard<std::mutex> g(m_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    size_t pending() const {
        std::lock_guard<std::mutex> g(m_);
        return q_.size();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;  // stop_ and drained
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

}  // namespace nue::lx
