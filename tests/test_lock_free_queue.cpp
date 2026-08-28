#include <gtest/gtest.h>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <vector>
#include "edgeflow/lock_free_bounded_queue.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::LockFreeBoundedQueue;
using edgeflow::PushResult;

TEST(LockFreeQueue, RoundsCapacityUpToPowerOfTwo) {
    LockFreeBoundedQueue<int> queue(50, BackpressurePolicy::Block);
    EXPECT_EQ(queue.capacity(), 64u);

    // The rounded capacity is real, not cosmetic: all 64 slots accept.
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(queue.push(i), PushResult::Accepted) << "failed at i=" << i;
    }
    EXPECT_EQ(queue.push(999), PushResult::RejectedBackpressure);
}

TEST(LockFreeQueue, LeavesExactPowerOfTwoCapacityUnchanged) {
    LockFreeBoundedQueue<int> queue(64, BackpressurePolicy::Block);
    EXPECT_EQ(queue.capacity(), 64u);
}

TEST(LockFreeQueue, RoundsCapacityOneUpToTwoSlots) {
    // The ring cannot run with a single slot: a written-but-unconsumed slot and
    // a slot freed one lap ahead would share a sequence value, so a producer
    // would overwrite live data and pop() would spin forever. A capacity of 1 is
    // therefore rounded up to 2.
    LockFreeBoundedQueue<int> queue(1, BackpressurePolicy::DropNewest);
    EXPECT_EQ(queue.capacity(), 2u);

    // Both slots are real, and the queue still reports backpressure once full.
    ASSERT_EQ(queue.push(1), PushResult::Accepted);
    ASSERT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.push(3), PushResult::RejectedBackpressure);

    // The regression this guards is a livelock, so prove pop() still returns.
    EXPECT_EQ(*queue.pop(), 1);
    EXPECT_EQ(*queue.pop(), 2);
}

TEST(LockFreeQueue, RejectsCapacityAboveMaximum) {
    EXPECT_THROW((LockFreeBoundedQueue<int>(2000000000, BackpressurePolicy::Block)),
                 std::invalid_argument);
}

TEST(LockFreeQueue, DropOldestSucceedsWithASingleProducer) {
    // The DropOldest guarantee is exact with one producer -- which is what
    // production has, since the gateway's io_context is single-threaded. Under
    // multiple concurrent producers it degrades to approximate; that is
    // documented on the class and asserted loosely in the stress tests.
    LockFreeBoundedQueue<int> queue(2, BackpressurePolicy::DropOldest);
    ASSERT_EQ(queue.push(1), PushResult::Accepted);
    ASSERT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.push(3), PushResult::DroppedOldest);
    EXPECT_EQ(queue.size(), 2u);

    EXPECT_EQ(*queue.pop(), 2);
    EXPECT_EQ(*queue.pop(), 3);
}

TEST(LockFreeQueue, SizeIsExactWhenQuiescent) {
    LockFreeBoundedQueue<int> queue(8, BackpressurePolicy::Block);
    EXPECT_EQ(queue.size(), 0u);
    ASSERT_EQ(queue.push(1), PushResult::Accepted);
    ASSERT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.size(), 2u);
    ASSERT_TRUE(queue.pop().has_value());
    EXPECT_EQ(queue.size(), 1u);
}

// The deliverable of this task is "an idle consumer sleeps rather than spins",
// and CPU time is the only thing that actually distinguishes those. A parked
// thread accrues almost no processor time while it waits; a spinning-and-
// yielding one accrues roughly wall-clock time.
//
// std::clock() measures processor time for the whole process, and
// gtest_discover_tests runs each test in its own process, so nothing else is
// contributing here. The margin is deliberately loose (a third of the wait):
// the difference being detected is ~0ms versus ~400ms, so a generous threshold
// still separates them cleanly while leaving room for a loaded CI machine.
TEST(LockFreeQueue, ParksRatherThanSpinningWhileEmpty) {
    LockFreeBoundedQueue<int> queue(8, BackpressurePolicy::Block);
    constexpr auto kWait = std::chrono::milliseconds(400);

    std::clock_t cpu_before = std::clock();
    std::thread consumer([&] { queue.pop(); });

    std::this_thread::sleep_for(kWait);
    ASSERT_EQ(queue.push(1), PushResult::Accepted);
    consumer.join();

    double cpu_ms = 1000.0 * static_cast<double>(std::clock() - cpu_before) / CLOCKS_PER_SEC;
    EXPECT_LT(cpu_ms, 130.0)
        << "pop() burned " << cpu_ms << "ms of CPU while waiting " << kWait.count()
        << "ms for an item; it is spinning rather than parking";
}

TEST(LockFreeQueue, WakesAParkedConsumerOnPush) {
    LockFreeBoundedQueue<int> queue(8, BackpressurePolicy::Block);
    std::atomic<bool> got_item{false};

    std::thread consumer([&] {
        auto item = queue.pop();
        if (item && *item == 7) {
            got_item.store(true);
        }
    });

    // Well past the bounded spin phase, so the consumer is genuinely parked and
    // the push below must go through the notify path to wake it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(queue.push(7), PushResult::Accepted);

    consumer.join();
    EXPECT_TRUE(got_item.load());
}

TEST(LockFreeQueue, ShutdownWakesEveryParkedConsumer) {
    LockFreeBoundedQueue<int> queue(8, BackpressurePolicy::Block);
    constexpr int kConsumers = 4;
    std::atomic<int> returned{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            EXPECT_FALSE(queue.pop().has_value());
            returned.fetch_add(1);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    queue.shutdown();

    // If shutdown() notified only one waiter, or raced a consumer that was
    // between its last check and its wait, this join hangs.
    for (auto& consumer : consumers) {
        consumer.join();
    }
    EXPECT_EQ(returned.load(), kConsumers);
}
