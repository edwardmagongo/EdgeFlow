#include <gtest/gtest.h>
#include <chrono>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/lock_free_bounded_queue.hpp"
#include "edgeflow/timed_event.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::Event;
using edgeflow::PushResult;
using edgeflow::TimedEvent;

using TimedEventQueueTypes =
    ::testing::Types<edgeflow::BoundedQueue<TimedEvent>,
                     edgeflow::LockFreeBoundedQueue<TimedEvent>>;

template <typename Q>
class TimedEventQueueTest : public ::testing::Test {};

TYPED_TEST_SUITE(TimedEventQueueTest, TimedEventQueueTypes);

TYPED_TEST(TimedEventQueueTest, RoundTripsEventAndTimestamp) {
    TypeParam queue(4, BackpressurePolicy::Block);

    Event event{42, "2026-08-28T00:00:00Z", 21.5, 90, 0.0, 0.0, "telemetry"};
    auto enqueued_at = std::chrono::steady_clock::now();
    TimedEvent timed{event, enqueued_at};

    ASSERT_EQ(queue.push(timed), PushResult::Accepted);

    auto popped = queue.pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->event, event);
    EXPECT_EQ(popped->enqueued_at, enqueued_at);
}

TYPED_TEST(TimedEventQueueTest, PreservesOrderAndBackpressure) {
    TypeParam queue(2, BackpressurePolicy::DropNewest);

    Event event_a{1, "t", 1.0, 1, 0.0, 0.0, "telemetry"};
    Event event_b{2, "t", 2.0, 2, 0.0, 0.0, "telemetry"};
    Event event_c{3, "t", 3.0, 3, 0.0, 0.0, "telemetry"};

    auto now = std::chrono::steady_clock::now();
    ASSERT_EQ(queue.push(TimedEvent{event_a, now}), PushResult::Accepted);
    ASSERT_EQ(queue.push(TimedEvent{event_b, now}), PushResult::Accepted);
    EXPECT_EQ(queue.push(TimedEvent{event_c, now}), PushResult::RejectedBackpressure);

    EXPECT_EQ(queue.pop()->event, event_a);
    EXPECT_EQ(queue.pop()->event, event_b);
}
