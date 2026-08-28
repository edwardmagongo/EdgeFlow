#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/lock_free_bounded_queue.hpp"
#include "edgeflow/queue_concept.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::PushResult;

// Every implementation listed here must satisfy the whole contract below.
//
// Capacities used here are powers of two and at least 2, so that both
// implementations agree exactly. LockFreeBoundedQueue rounds capacity up to a
// power of two with a floor of two slots, so a capacity of 1 or of 50 would
// make the two legitimately disagree about size() and about when
// RejectedBackpressure first appears. Rounding is covered separately, in
// test_lock_free_queue.cpp.
using QueueTypes = ::testing::Types<edgeflow::BoundedQueue<int>,
                                    edgeflow::LockFreeBoundedQueue<int>>;

template <typename Q>
class QueueContractTest : public ::testing::Test {};

TYPED_TEST_SUITE(QueueContractTest, QueueTypes);

TYPED_TEST(QueueContractTest, SatisfiesEventQueueConcept) {
    static_assert(edgeflow::EventQueue<TypeParam>,
                  "queue implementation does not satisfy the EventQueue concept");
    SUCCEED();
}

TYPED_TEST(QueueContractTest, PushAcceptsUntilCapacity) {
    TypeParam queue(2, BackpressurePolicy::DropNewest);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.size(), 2u);
}

TYPED_TEST(QueueContractTest, DropNewestRejectsWhenFull) {
    TypeParam queue(2, BackpressurePolicy::DropNewest);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.push(3), PushResult::RejectedBackpressure);
    EXPECT_EQ(queue.size(), 2u);

    auto popped = queue.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(*popped, 1);
}

TYPED_TEST(QueueContractTest, DropOldestEvictsOldest) {
    TypeParam queue(2, BackpressurePolicy::DropOldest);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.push(3), PushResult::DroppedOldest);

    EXPECT_EQ(*queue.pop(), 2);
    EXPECT_EQ(*queue.pop(), 3);
}

TYPED_TEST(QueueContractTest, BlockPolicyRejectsWithoutEvicting) {
    TypeParam queue(2, BackpressurePolicy::Block);
    EXPECT_EQ(queue.push(1), PushResult::Accepted);
    EXPECT_EQ(queue.push(2), PushResult::Accepted);
    EXPECT_EQ(queue.push(3), PushResult::RejectedBackpressure);
    EXPECT_EQ(queue.size(), 2u);
    // Block rejects rather than evicting: the oldest item is still at the head.
    EXPECT_EQ(*queue.pop(), 1);
}

TYPED_TEST(QueueContractTest, ConcurrentPushPopPreservesAllAcceptedItems) {
    TypeParam queue(16, BackpressurePolicy::Block);
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

TYPED_TEST(QueueContractTest, RejectsZeroCapacity) {
    EXPECT_THROW((TypeParam(0, BackpressurePolicy::Block)), std::invalid_argument);
    EXPECT_THROW((TypeParam(0, BackpressurePolicy::DropOldest)), std::invalid_argument);
    EXPECT_THROW((TypeParam(0, BackpressurePolicy::DropNewest)), std::invalid_argument);
}

TYPED_TEST(QueueContractTest, ShutdownUnblocksWaitingPop) {
    TypeParam queue(4, BackpressurePolicy::Block);
    std::optional<int> result = 42; // sentinel to prove it got overwritten
    std::thread popper([&] { result = queue.pop(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.shutdown();
    popper.join();

    EXPECT_FALSE(result.has_value());
}

TYPED_TEST(QueueContractTest, ShutdownDrainsRemainingItemsBeforeReturningNullopt) {
    TypeParam queue(4, BackpressurePolicy::Block);
    ASSERT_EQ(queue.push(1), PushResult::Accepted);
    ASSERT_EQ(queue.push(2), PushResult::Accepted);

    queue.shutdown();

    // Contract: items already queued when shutdown() arrives are still
    // delivered. WorkerPool's drain loop relies on this, so it is tested
    // explicitly rather than assumed.
    EXPECT_EQ(*queue.pop(), 1);
    EXPECT_EQ(*queue.pop(), 2);
    EXPECT_FALSE(queue.pop().has_value());
}
