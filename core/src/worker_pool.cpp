#include "edgeflow/worker_pool.hpp"

namespace edgeflow {

WorkerPool::WorkerPool(BoundedQueue<TimedEvent>& queue, Batcher& batcher, Stats& stats,
                        std::size_t num_workers)
    : queue_(queue), batcher_(batcher), stats_(stats), num_workers_(num_workers) {}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::start() {
    for (std::size_t i = 0; i < num_workers_; ++i) {
        threads_.emplace_back([this] { worker_loop(); });
    }
}

void WorkerPool::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    queue_.shutdown();
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

void WorkerPool::worker_loop() {
    while (auto timed = queue_.pop()) {
        stats_.record_queue_wait(std::chrono::steady_clock::now() - timed->enqueued_at);
        batcher_.add_event(std::move(timed->event));
    }
}

} // namespace edgeflow
