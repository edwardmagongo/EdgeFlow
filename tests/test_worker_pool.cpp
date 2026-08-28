#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include "edgeflow/batcher.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"
#include "edgeflow/worker_pool.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::Batcher;
using edgeflow::BoundedQueue;
using edgeflow::Event;
using edgeflow::PushResult;
using edgeflow::Stats;
using edgeflow::TimedEvent;
using edgeflow::WorkerPool;

TEST(WorkerPool, DrainsQueueIntoBatcher) {
    BoundedQueue<TimedEvent> queue(100, BackpressurePolicy::Block);
    Stats stats;
    std::atomic<int> events_seen{0};
    Batcher batcher(1000, std::chrono::milliseconds(60000),
                     [&](std::vector<Event> batch) { events_seen += static_cast<int>(batch.size()); });

    WorkerPool pool(queue, batcher, stats, 2);
    pool.start();

    for (int i = 0; i < 50; ++i) {
        Event event{i, "2026-08-28T00:00:00Z", 20.0, 100, 0.0, 0.0, "telemetry"};
        ASSERT_EQ(queue.push(TimedEvent{event, std::chrono::steady_clock::now()}), PushResult::Accepted);
    }

    pool.stop();
    batcher.flush();

    EXPECT_EQ(events_seen.load(), 50);
}

TEST(WorkerPool, RecordsQueueWaitLatency) {
    BoundedQueue<TimedEvent> queue(100, BackpressurePolicy::Block);
    Stats stats;
    Batcher batcher(1000, std::chrono::milliseconds(60000), [](std::vector<Event>) {});

    WorkerPool pool(queue, batcher, stats, 1);
    pool.start();

    Event event{1, "2026-08-28T00:00:00Z", 20.0, 100, 0.0, 0.0, "telemetry"};
    ASSERT_EQ(queue.push(TimedEvent{event, std::chrono::steady_clock::now()}), PushResult::Accepted);

    pool.stop();

    EXPECT_EQ(stats.snapshot().queue_wait_count, 1u);
}

TEST(WorkerPool, StopIsIdempotent) {
    BoundedQueue<TimedEvent> queue(10, BackpressurePolicy::Block);
    Stats stats;
    Batcher batcher(1000, std::chrono::milliseconds(60000), [](std::vector<Event>) {});
    WorkerPool pool(queue, batcher, stats, 2);
    pool.start();
    pool.stop();
    pool.stop();
    SUCCEED();
}
