#pragma once
#include <chrono>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "edgeflow/batcher.hpp"
#include "edgeflow/event.hpp"
#include "edgeflow/queue_concept.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"

namespace edgeflow {

// Runs `num_workers` threads, each popping TimedEvents from `queue`,
// recording queue-wait latency into `stats`, and handing the underlying
// Event to `batcher`. Call stop() before destruction to join all threads.
//
// Templated on the queue so the implementation is selected at compile time.
// The alternative -- a virtual queue interface chosen at runtime -- would put a
// ~2-5ns dispatch on top of a ~19-30ns push, a 10-25% distortion of the exact
// quantity Phase 3 exists to measure. It would compare "mutex + vtable" against
// "lock-free + vtable" rather than the two queues.
//
// Class template argument deduction means existing construction sites
// (`WorkerPool pool(queue, batcher, stats, n);`) continue to compile unchanged.
template <EventQueue Q>
class WorkerPool {
    static_assert(std::is_same_v<typename Q::value_type, TimedEvent>,
                  "WorkerPool consumes TimedEvent: it reads enqueued_at to record "
                  "queue-wait latency and forwards .event to the Batcher");

public:
    WorkerPool(Q& queue, Batcher& batcher, Stats& stats, std::size_t num_workers)
        : queue_(queue), batcher_(batcher), stats_(stats), num_workers_(num_workers) {}

    ~WorkerPool() { stop(); }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void start() {
        for (std::size_t i = 0; i < num_workers_; ++i) {
            threads_.emplace_back([this] { worker_loop(); });
        }
    }

    void stop() {
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

private:
    // Relies on the queue contract's drain guarantee: pop() keeps returning
    // items already queued when shutdown() arrived, and yields nullopt only
    // once the queue is genuinely empty. Both implementations are held to that
    // by QueueContractTest.ShutdownDrainsRemainingItemsBeforeReturningNullopt.
    void worker_loop() {
        while (auto timed = queue_.pop()) {
            stats_.record_queue_wait(std::chrono::steady_clock::now() - timed->enqueued_at);
            batcher_.add_event(std::move(timed->event));
        }
    }

    Q& queue_;
    Batcher& batcher_;
    Stats& stats_;
    std::size_t num_workers_;
    std::vector<std::thread> threads_;
    bool stopped_ = false;
};

} // namespace edgeflow
