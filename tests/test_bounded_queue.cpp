#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "edgeflow/bounded_queue.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::BoundedQueue;
using edgeflow::PushResult;

TEST(BoundedQueue, PushAcceptsUntilCapacity) {
    BoundedQueue<int> queue(2, BackpressurePolicy::DropNewest);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.size(), 2u);
}

TEST(BoundedQueue, DropNewestRejectsWhenFull) {
    BoundedQueue<int> queue(1, BackpressurePolicy::DropNewest);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::RejectedBackpressure);
    EXPECT_EQ(queue.size(), 1u);

    auto popped = queue.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, 1);
}

TEST(BoundedQueue, DropOldestEvictsOldest) {
    BoundedQueue<int> queue(2, BackpressurePolicy::DropOldest);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.push(3), PushResult::DroppedOldest);

    EXPECT_EQ(*queue.pop(), 2);
    EXPECT_EQ(*queue.pop(), 3);
}

TEST(BoundedQueue, BlockPolicyRejectsWithoutEvicting) {
    BoundedQueue<int> queue(1, BackpressurePolicy::Block);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::RejectedBackpressure);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(*queue.pop(), 1);
}

TEST(BoundedQueue, ConcurrentPushPopPreservesAllAcceptedItems) {
    BoundedQueue<int> queue(16, BackpressurePolicy::Block);
    constexpr int kProducers = 4;
    constexpr int kItemsPerProducer = 1000;
    std::atomic<int> accepted{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                while (queue.push(p * kItemsPerProducer + i) != PushResult::Accepted) {
                    std::this_thread::yield();
                }
                accepted.fetch_add(1);
            }
        });
    }

    std::atomic<int> consumed{0};
    std::thread consumer([&] {
        while (consumed.load() < kProducers * kItemsPerProducer) {
            if (auto item = queue.pop()) {
                consumed.fetch_add(1);
            }
        }
    });

    for (auto& t : producers) t.join();
    consumer.join();

    EXPECT_EQ(accepted.load(), kProducers * kItemsPerProducer);
    EXPECT_EQ(consumed.load(), kProducers * kItemsPerProducer);
}

TEST(BoundedQueue, RejectsZeroCapacity) {
    EXPECT_THROW((BoundedQueue<int>(0, BackpressurePolicy::Block)), std::invalid_argument);
    EXPECT_THROW((BoundedQueue<int>(0, BackpressurePolicy::DropOldest)), std::invalid_argument);
    EXPECT_THROW((BoundedQueue<int>(0, BackpressurePolicy::DropNewest)), std::invalid_argument);
}

TEST(BoundedQueue, ShutdownUnblocksWaitingPop) {
    BoundedQueue<int> queue(4, BackpressurePolicy::Block);
    std::optional<int> result = 42; // sentinel to prove it got overwritten
    std::thread popper([&] { result = queue.pop(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.shutdown();
    popper.join();

    EXPECT_FALSE(result.has_value());
}
