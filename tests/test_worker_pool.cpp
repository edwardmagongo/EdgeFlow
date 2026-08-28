#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <vector>
#include "edgeflow/batcher.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/lock_free_bounded_queue.hpp"
#include "edgeflow/stats.hpp"
#include "edgeflow/timed_event.hpp"
#include "edgeflow/worker_pool.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::Batcher;
using edgeflow::Event;
using edgeflow::PushResult;
using edgeflow::Stats;
using edgeflow::TimedEvent;
using edgeflow::WorkerPool;

// WorkerPool must behave identically whichever queue it is instantiated on.
using WorkerPoolQueueTypes =
    ::testing::Types<edgeflow::BoundedQueue<TimedEvent>,
                     edgeflow::LockFreeBoundedQueue<TimedEvent>>;

template <typename Q>
class WorkerPoolTest : public ::testing::Test {};

TYPED_TEST_SUITE(WorkerPoolTest, WorkerPoolQueueTypes);

TYPED_TEST(WorkerPoolTest, DrainsQueueIntoBatcher) {
    TypeParam queue(128, BackpressurePolicy::Block);
    Stats stats;
    std::atomic<int> events_seen{0};
    Batcher batcher(1000, std::chrono::milliseconds(60000),
                     [&](std::vector<Event> batch) { events_seen += static_cast<int>(batch.size()); });

    WorkerPool<TypeParam> pool(queue, batcher, stats, 2);
    pool.start();

    for (int i = 0; i < 50; ++i) {
        Event event{i, "2026-08-28T00:00:00Z", 20.0, 100, 0.0, 0.0, "telemetry"};
        ASSERT_EQ(queue.push(TimedEvent{event, std::chrono::steady_clock::now()}), PushResult::Accepted);
    }

    pool.stop();
    batcher.flush();

    EXPECT_EQ(events_seen.load(), 50);
}

TYPED_TEST(WorkerPoolTest, RecordsQueueWaitLatency) {
    TypeParam queue(128, BackpressurePolicy::Block);
    Stats stats;
    Batcher batcher(1000, std::chrono::milliseconds(60000), [](std::vector<Event>) {});

    WorkerPool<TypeParam> pool(queue, batcher, stats, 1);
    pool.start();

    Event event{1, "2026-08-28T00:00:00Z", 20.0, 100, 0.0, 0.0, "telemetry"};
    ASSERT_EQ(queue.push(TimedEvent{event, std::chrono::steady_clock::now()}), PushResult::Accepted);

    pool.stop();

    EXPECT_EQ(stats.snapshot().queue_wait_count, 1u);
}

TYPED_TEST(WorkerPoolTest, StopIsIdempotent) {
    TypeParam queue(16, BackpressurePolicy::Block);
    Stats stats;
    Batcher batcher(1000, std::chrono::milliseconds(60000), [](std::vector<Event>) {});
    WorkerPool<TypeParam> pool(queue, batcher, stats, 2);
    pool.start();
    pool.stop();
    pool.stop();
    SUCCEED();
}

// Deduction check: the gateway constructs WorkerPool without naming the queue
// type, and Task 5 depends on that continuing to work. A compile failure here
// means main.cpp would break too.
TYPED_TEST(WorkerPoolTest, ClassTemplateArgumentDeductionWorks) {
    TypeParam queue(16, BackpressurePolicy::Block);
    Stats stats;
    Batcher batcher(1000, std::chrono::milliseconds(60000), [](std::vector<Event>) {});
    WorkerPool pool(queue, batcher, stats, 1); // no explicit <TypeParam>
    static_assert(std::is_same_v<decltype(pool), WorkerPool<TypeParam>>);
    pool.start();
    pool.stop();
    SUCCEED();
}
