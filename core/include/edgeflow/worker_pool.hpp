#pragma once
#include <thread>
#include <vector>
#include "edgeflow/batcher.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/event.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"

namespace edgeflow {

// Runs `num_workers` threads, each popping TimedEvents from `queue`,
// recording queue-wait latency into `stats`, and handing the underlying
// Event to `batcher`. Call stop() before destruction to join all threads.
class WorkerPool {
public:
    WorkerPool(BoundedQueue<TimedEvent>& queue, Batcher& batcher, Stats& stats, std::size_t num_workers);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void start();
    void stop();

private:
    void worker_loop();

    BoundedQueue<TimedEvent>& queue_;
    Batcher& batcher_;
    Stats& stats_;
    std::size_t num_workers_;
    std::vector<std::thread> threads_;
    bool stopped_ = false;
};

} // namespace edgeflow
