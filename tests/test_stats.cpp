#include <gtest/gtest.h>
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
