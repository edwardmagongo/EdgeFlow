#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
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
//
// When burst_size > 0, each producer pushes items in bursts of burst_size,
// sleeping inter_burst_sleep between bursts. This is what actually exercises
// LockFreeBoundedQueue's parking path: with producers pushing flat-out (the
// burst_size == 0 default), the ring stays fed enough that consumers rarely
// exhaust the bounded spin in pop() and essentially never park. A sleeping
// producer lets consumers fully drain the queue and go idle, so the next
// pop() call spins out and genuinely registers as a waiter and blocks on the
// condition variable -- the exact code path this phase's fence protocol is
// about. Consumers are unaffected by bursting; they still drain until
// shutdown, so the exactness assertion applies unchanged.
template <typename Queue>
void run_exactness_stress(int producers, int consumers, int items_per_producer,
                          int burst_size = 0,
                          std::chrono::milliseconds inter_burst_sleep = std::chrono::milliseconds(0)) {
    Queue queue(64, BackpressurePolicy::Block);

    std::vector<std::thread> producer_threads;
    for (int p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&queue, p, items_per_producer, burst_size, inter_burst_sleep] {
            if (burst_size <= 0) {
                for (int i = 0; i < items_per_producer; ++i) {
                    while (queue.push(p * items_per_producer + i) != PushResult::Accepted) {
                        std::this_thread::yield();
                    }
                }
                return;
            }
            int i = 0;
            while (i < items_per_producer) {
                int burst_end = std::min(i + burst_size, items_per_producer);
                for (; i < burst_end; ++i) {
                    while (queue.push(p * items_per_producer + i) != PushResult::Accepted) {
                        std::this_thread::yield();
                    }
                }
                if (i < items_per_producer) {
                    std::this_thread::sleep_for(inter_burst_sleep);
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

// The three shapes above push as fast as possible, so the ring stays fed and
// consumers rarely if ever exhaust their bounded spin -- meaning
// LockFreeBoundedQueue's parking path (register as a waiter, block on the
// condition variable, get woken by notify_one_waiter()'s fence-guarded
// notify) goes essentially uncovered despite being the most correctness-
// sensitive code this phase added. This shape closes that gap: producers
// push a small burst, then sleep long enough that consumers fully drain the
// queue and each blocked pop() spins out its budget and genuinely parks,
// before the next burst arrives and wakes them back up. The exactness
// assertion is identical to the other shapes -- bursting production timing
// doesn't change what "correct" means, only how much idle time consumers get
// between items.
TYPED_TEST(QueueStressTest, BurstyProducersDrivesConsumerParkingLosesNothing) {
    run_exactness_stress<TypeParam>(2, 4, 500, /*burst_size=*/24,
                                    /*inter_burst_sleep=*/std::chrono::milliseconds(5));
}
