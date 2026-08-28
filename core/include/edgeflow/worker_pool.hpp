#pragma once
#include <thread>
#include <vector>
#include "edgeflow/batcher.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/event.hpp"

namespace edgeflow {

// Runs `num_workers` threads, each popping Events from `queue` and handing
// them to `batcher`. Call stop() before destruction to join all threads.
class WorkerPool {
public:
    WorkerPool(BoundedQueue<Event>& queue, Batcher& batcher, std::size_t num_workers);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void start();
    void stop();

private:
    void worker_loop();

    BoundedQueue<Event>& queue_;
    Batcher& batcher_;
    std::size_t num_workers_;
    std::vector<std::thread> threads_;
    bool stopped_ = false;
};

} // namespace edgeflow
