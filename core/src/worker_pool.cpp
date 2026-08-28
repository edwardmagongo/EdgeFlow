#include "edgeflow/worker_pool.hpp"

namespace edgeflow {

WorkerPool::WorkerPool(BoundedQueue<Event>& queue, Batcher& batcher, std::size_t num_workers)
    : queue_(queue), batcher_(batcher), num_workers_(num_workers) {}

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
    while (auto event = queue_.pop()) {
        batcher_.add_event(std::move(*event));
    }
}

} // namespace edgeflow
