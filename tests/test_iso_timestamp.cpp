#include <gtest/gtest.h>
#include <ctime>
#include <string>
#include "edgeflow/iso_timestamp.hpp"

using edgeflow::CachedIsoTimestamp;

// 2026-08-28T12:34:56Z as seconds since the Unix epoch.
constexpr std::time_t kFixedSecond = 1787920496;

TEST(CachedIsoTimestamp, FormatsIso8601Utc) {
    CachedIsoTimestamp clock;
    EXPECT_EQ(clock.format(kFixedSecond), "2026-08-28T12:34:56Z");
}

TEST(CachedIsoTimestamp, RepeatedCallsInTheSameSecondAreStable) {
    CachedIsoTimestamp clock;
    const std::string first = clock.format(kFixedSecond);
    const std::string second = clock.format(kFixedSecond);
    EXPECT_EQ(first, second);
}

TEST(CachedIsoTimestamp, RegeneratesWhenTheSecondChanges) {
    // The bug this guards against: a cache that never invalidates would return
    // 12:34:56 forever, stamping every event with a wrong time.
    CachedIsoTimestamp clock;
    EXPECT_EQ(clock.format(kFixedSecond), "2026-08-28T12:34:56Z");
    EXPECT_EQ(clock.format(kFixedSecond + 1), "2026-08-28T12:34:57Z");
    EXPECT_EQ(clock.format(kFixedSecond + 4), "2026-08-28T12:35:00Z");
}

TEST(CachedIsoTimestamp, HandlesGoingBackwards) {
    // system_clock is not monotonic; NTP can step it backwards. The cache must
    // notice any change of second, not just an increase.
    CachedIsoTimestamp clock;
    EXPECT_EQ(clock.format(kFixedSecond), "2026-08-28T12:34:56Z");
    EXPECT_EQ(clock.format(kFixedSecond - 1), "2026-08-28T12:34:55Z");
}

TEST(CachedIsoTimestamp, CrossesADayBoundary) {
    // 2026-08-28T23:59:59Z -> 2026-08-29T00:00:00Z
    constexpr std::time_t kBeforeMidnight = 1787961599;
    CachedIsoTimestamp clock;
    EXPECT_EQ(clock.format(kBeforeMidnight), "2026-08-28T23:59:59Z");
    EXPECT_EQ(clock.format(kBeforeMidnight + 1), "2026-08-29T00:00:00Z");
}

TEST(CachedIsoTimestamp, NowMatchesFormatOfTheCurrentSecond) {
    CachedIsoTimestamp a;
    CachedIsoTimestamp b;
    const std::time_t second = std::time(nullptr);
    // Compare against format() of the same second rather than a literal, so this
    // does not depend on when the suite runs. Re-read the clock if the second
    // rolled over between the two calls.
    std::string via_now = a.now();
    if (std::time(nullptr) != second) {
        via_now = a.now();
    }
    EXPECT_EQ(via_now, b.format(std::time(nullptr)));
}
