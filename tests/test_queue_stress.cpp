#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/lock_free_bounded_queue.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::PushResult;

using StressQueueTypes = ::testing::Types<edgeflow::BoundedQueue<int>,
                                          edgeflow::LockFreeBoundedQueue<int>>;

template <typename Q>
class QueueStressTest : public ::testing::Test {};

TYPED_TEST_SUITE(QueueStressTest, StressQueueTypes);

namespace {

// Runs `producers` threads, each pushing a disjoint block of integers, and
// `consumers` threads draining, then asserts the drained multiset is EXACTLY
// the pushed multiset -- nothing lost, nothing duplicated, nothing invented.
//
// Block policy never evicts, so a producer that retries until Accepted pushes
// every one of its values exactly once. Producer p owns the block
// [p * items_per_producer, (p + 1) * items_per_producer), so the union across
// all producers is precisely 0 .. producers * items_per_producer - 1.
template <typename Queue>
void run_exactness_stress(int producers, int consumers, int items_per_producer) {
    Queue queue(64, BackpressurePolicy::Block);

    std::vector<std::thread> producer_threads;
    for (int p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&queue, p, items_per_producer] {
            for (int i = 0; i < items_per_producer; ++i) {
                while (queue.push(p * items_per_producer + i) != PushResult::Accepted) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Each consumer owns its own vector, so collection itself adds no
    // synchronization that could mask a queue bug.
    std::vector<std::vector<int>> drained(static_cast<std::size_t>(consumers));
    std::vector<std::thread> consumer_threads;
    for (int c = 0; c < consumers; ++c) {
        consumer_threads.emplace_back([&queue, &drained, c] {
            // pop() returns nullopt only once the queue is empty AND shutdown()
            // has been called, so this drains everything before exiting.
            while (auto item = queue.pop()) {
                drained[static_cast<std::size_t>(c)].push_back(*item);
            }
        });
    }

    for (auto& thread : producer_threads) {
        thread.join();
    }
    // Only safe once every producer is done: shutdown() before that could let a
    // consumer exit while items were still being pushed.
    queue.shutdown();
    for (auto& thread : consumer_threads) {
        thread.join();
    }

    std::vector<int> collected;
    for (const auto& per_consumer : drained) {
        collected.insert(collected.end(), per_consumer.begin(), per_consumer.end());
    }
    std::sort(collected.begin(), collected.end());

    std::vector<int> expected(static_cast<std::size_t>(producers) * items_per_producer);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<int>(i);
    }

    ASSERT_EQ(collected.size(), expected.size())
        << "pushed " << expected.size() << " items but drained " << collected.size()
        << " -- items were lost or duplicated";
    EXPECT_EQ(collected, expected);
}

} // namespace

// The configuration production actually has: the gateway's io_context is
// single-threaded, so exactly one thread pushes while N workers contend on pop.
// This is the case the phase is really about.
TYPED_TEST(QueueStressTest, SingleProducerManyConsumersLosesNothing) {
    run_exactness_stress<TypeParam>(1, 4, 10000);
}

TYPED_TEST(QueueStressTest, ManyProducersManyConsumersLosesNothing) {
    run_exactness_stress<TypeParam>(4, 4, 2000);
}

TYPED_TEST(QueueStressTest, ManyProducersSingleConsumerLosesNothing) {
    run_exactness_stress<TypeParam>(4, 1, 2000);
}
