#include <gtest/gtest.h>
#include <thread>
#include "edgeflow/batcher.hpp"

using edgeflow::Batcher;
using edgeflow::Event;

namespace {
Event make_event(int device_id) {
    return Event{device_id, "2026-08-28T00:00:00Z", 20.0, 100, 0.0, 0.0, "telemetry"};
}
} // namespace

TEST(Batcher, FlushesWhenSizeThresholdReached) {
    std::vector<std::vector<Event>> flushed;
    Batcher batcher(3, std::chrono::milliseconds(60000),
                     [&](std::vector<Event> batch) { flushed.push_back(std::move(batch)); });

    batcher.add_event(make_event(1));
    batcher.add_event(make_event(2));
    EXPECT_TRUE(flushed.empty());

    batcher.add_event(make_event(3));
    ASSERT_EQ(flushed.size(), 1u);
    EXPECT_EQ(flushed[0].size(), 3u);
}

TEST(Batcher, FlushesWhenAgeThresholdReached) {
    std::vector<std::vector<Event>> flushed;
    Batcher batcher(1000, std::chrono::milliseconds(10),
                     [&](std::vector<Event> batch) { flushed.push_back(std::move(batch)); });

    batcher.add_event(make_event(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    batcher.add_event(make_event(2));

    ASSERT_EQ(flushed.size(), 1u);
    EXPECT_EQ(flushed[0].size(), 2u);
}

TEST(Batcher, FlushSendsPartialBatch) {
    std::vector<std::vector<Event>> flushed;
    Batcher batcher(100, std::chrono::milliseconds(60000),
                     [&](std::vector<Event> batch) { flushed.push_back(std::move(batch)); });

    batcher.add_event(make_event(1));
    batcher.flush();

    ASSERT_EQ(flushed.size(), 1u);
    EXPECT_EQ(flushed[0].size(), 1u);
}

TEST(Batcher, FlushDoesNothingWhenEmpty) {
    int flush_count = 0;
    Batcher batcher(100, std::chrono::milliseconds(60000),
                     [&](std::vector<Event>) { ++flush_count; });
    batcher.flush();
    EXPECT_EQ(flush_count, 0);
}
