#include <gtest/gtest.h>
#include <stdexcept>
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
