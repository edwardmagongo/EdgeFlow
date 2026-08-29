#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>
#include "edgeflow/stats.hpp"

using edgeflow::Stats;

TEST(Stats, StartsAtZero) {
    Stats stats;
    EXPECT_EQ(stats.events_accepted(), 0u);
    EXPECT_EQ(stats.events_dropped_oldest(), 0u);
    EXPECT_EQ(stats.events_dropped_newest(), 0u);
    EXPECT_EQ(stats.events_malformed(), 0u);
}

TEST(Stats, RecordAcceptedIncrementsOnlyThatCounter) {
    Stats stats;
    stats.record_accepted();
    stats.record_accepted();
    EXPECT_EQ(stats.events_accepted(), 2u);
    EXPECT_EQ(stats.events_dropped_oldest(), 0u);
    EXPECT_EQ(stats.events_dropped_newest(), 0u);
    EXPECT_EQ(stats.events_malformed(), 0u);
}

TEST(Stats, RecordDroppedOldestIncrementsOnlyThatCounter) {
    Stats stats;
    stats.record_dropped_oldest();
    EXPECT_EQ(stats.events_dropped_oldest(), 1u);
    EXPECT_EQ(stats.events_accepted(), 0u);
    EXPECT_EQ(stats.events_dropped_newest(), 0u);
    EXPECT_EQ(stats.events_malformed(), 0u);
}

TEST(Stats, RecordDroppedNewestIncrementsOnlyThatCounter) {
    Stats stats;
    stats.record_dropped_newest();
    EXPECT_EQ(stats.events_dropped_newest(), 1u);
    EXPECT_EQ(stats.events_accepted(), 0u);
    EXPECT_EQ(stats.events_dropped_oldest(), 0u);
    EXPECT_EQ(stats.events_malformed(), 0u);
}

TEST(Stats, RecordMalformedIncrementsOnlyThatCounter) {
    Stats stats;
    stats.record_malformed();
    EXPECT_EQ(stats.events_malformed(), 1u);
    EXPECT_EQ(stats.events_accepted(), 0u);
    EXPECT_EQ(stats.events_dropped_oldest(), 0u);
    EXPECT_EQ(stats.events_dropped_newest(), 0u);
}

TEST(Stats, SnapshotReflectsCurrentCounts) {
    Stats stats;
    stats.record_accepted();
    stats.record_accepted();
    stats.record_dropped_oldest();
    stats.record_dropped_newest();
    stats.record_dropped_newest();
    stats.record_dropped_newest();
    stats.record_malformed();

    auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.events_accepted, 2u);
    EXPECT_EQ(snapshot.events_dropped_oldest, 1u);
    EXPECT_EQ(snapshot.events_dropped_newest, 3u);
    EXPECT_EQ(snapshot.events_malformed, 1u);
}

TEST(Stats, ConcurrentIncrementsAreAllCounted) {
    Stats stats;
    constexpr int kThreads = 8;
    constexpr int kIncrementsPerThread = 1000;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&stats] {
            for (int j = 0; j < kIncrementsPerThread; ++j) {
                stats.record_accepted();
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(stats.events_accepted(), static_cast<std::uint64_t>(kThreads * kIncrementsPerThread));
}

TEST(Stats, QueueWaitStartsAtZero) {
    Stats stats;
    auto snap = stats.snapshot();
    EXPECT_EQ(snap.queue_wait_count, 0u);
    EXPECT_EQ(snap.queue_wait_min_us, 0u);
    EXPECT_EQ(snap.queue_wait_max_us, 0u);
    EXPECT_DOUBLE_EQ(snap.queue_wait_mean_us, 0.0);
    EXPECT_EQ(snap.queue_wait_p50_us, 0u);
    EXPECT_EQ(snap.queue_wait_p99_us, 0u);
}

TEST(Stats, RecordQueueWaitTracksCountMinMaxMean) {
    Stats stats;
    stats.record_queue_wait(std::chrono::microseconds(100));
    stats.record_queue_wait(std::chrono::microseconds(300));
    stats.record_queue_wait(std::chrono::microseconds(200));

    auto snap = stats.snapshot();
    EXPECT_EQ(snap.queue_wait_count, 3u);
    EXPECT_EQ(snap.queue_wait_min_us, 100u);
    EXPECT_EQ(snap.queue_wait_max_us, 300u);
    EXPECT_NEAR(snap.queue_wait_mean_us, 200.0, 0.001);
}

TEST(Stats, RecordQueueWaitPercentilesReflectDistribution) {
    Stats stats;
    // 99 fast samples (100us) and 1 slow sample (10s): p50 should land in
    // the fast bucket, p99 should land at/above the slow one.
    for (int i = 0; i < 99; ++i) {
        stats.record_queue_wait(std::chrono::microseconds(100));
    }
    stats.record_queue_wait(std::chrono::seconds(10));

    auto snap = stats.snapshot();
    EXPECT_EQ(snap.queue_wait_count, 100u);
    EXPECT_LE(snap.queue_wait_p50_us, 250u);
    EXPECT_GE(snap.queue_wait_p99_us, 5000000u);
}

TEST(Stats, RecordQueueWaitConcurrentSamplesAllCounted) {
    Stats stats;
    constexpr int kThreads = 8;
    constexpr int kSamplesPerThread = 500;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&stats] {
            for (int j = 0; j < kSamplesPerThread; ++j) {
                stats.record_queue_wait(std::chrono::microseconds(50));
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(stats.snapshot().queue_wait_count,
              static_cast<std::uint64_t>(kThreads * kSamplesPerThread));
}

TEST(Stats, BatchCountersStartAtZero) {
    edgeflow::Stats stats;
    auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.batches_sent, 0u);
    EXPECT_EQ(snapshot.batches_retried, 0u);
    EXPECT_EQ(snapshot.batches_dropped_outbound, 0u);
    EXPECT_EQ(snapshot.batches_dropped_exhausted, 0u);
}

TEST(Stats, BatchCountersAreIndependent) {
    // Each cause has its own counter so an outage (exhausted) can be told apart
    // from a burst (outbound). Incrementing one must not move another.
    edgeflow::Stats stats;
    stats.record_batch_sent();
    stats.record_batch_sent();
    stats.record_batch_retried();
    stats.record_batch_dropped_outbound();
    stats.record_batch_dropped_exhausted();
    stats.record_batch_dropped_exhausted();
    stats.record_batch_dropped_exhausted();

    auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.batches_sent, 2u);
    EXPECT_EQ(snapshot.batches_retried, 1u);
    EXPECT_EQ(snapshot.batches_dropped_outbound, 1u);
    EXPECT_EQ(snapshot.batches_dropped_exhausted, 3u);
}

TEST(Stats, BatchCountersDoNotDisturbEventCounters) {
    edgeflow::Stats stats;
    stats.record_accepted();
    stats.record_batch_sent();
    stats.record_batch_dropped_outbound();

    auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.events_accepted, 1u);
    EXPECT_EQ(snapshot.events_dropped_oldest, 0u);
    EXPECT_EQ(snapshot.events_dropped_newest, 0u);
    EXPECT_EQ(snapshot.batches_sent, 1u);
    EXPECT_EQ(snapshot.batches_dropped_outbound, 1u);
}

TEST(Stats, ConcurrentBatchIncrementsAreAllCounted) {
    // The sink thread and the worker threads both touch these.
    edgeflow::Stats stats;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&stats] {
            for (int j = 0; j < kPerThread; ++j) {
                stats.record_batch_sent();
                stats.record_batch_retried();
            }
        });
    }
    for (auto& thread : threads) thread.join();

    auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.batches_sent, static_cast<std::uint64_t>(kThreads * kPerThread));
    EXPECT_EQ(snapshot.batches_retried, static_cast<std::uint64_t>(kThreads * kPerThread));
}
