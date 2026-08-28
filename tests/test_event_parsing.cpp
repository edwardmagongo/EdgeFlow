#include <gtest/gtest.h>
#include "edgeflow/event.hpp"

using edgeflow::Event;
using edgeflow::parse_event;
using edgeflow::serialize_event;

TEST(EventParsing, ParsesWellFormedLine) {
    const std::string line =
        R"({"device_id":18291,"timestamp":"2026-08-28T09:41:22Z","temperature":72.4,)"
        R"("battery":81,"latitude":37.7749,"longitude":-122.4194,"event_type":"telemetry"})";

    auto result = parse_event(line);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->device_id, 18291);
    EXPECT_EQ(result->timestamp, "2026-08-28T09:41:22Z");
    EXPECT_DOUBLE_EQ(result->temperature, 72.4);
    EXPECT_EQ(result->battery, 81);
    EXPECT_EQ(result->event_type, "telemetry");
}

TEST(EventParsing, RejectsInvalidJson) {
    auto result = parse_event("not json");
    EXPECT_FALSE(result.has_value());
}

TEST(EventParsing, RejectsMissingFields) {
    auto result = parse_event(R"({"device_id":1})");
    EXPECT_FALSE(result.has_value());
}

TEST(EventParsing, RoundTripsThroughSerialize) {
    Event event{18291, "2026-08-28T09:41:22Z", 72.4, 81, 37.7749, -122.4194, "telemetry"};
    auto line = serialize_event(event);
    auto parsed = parse_event(line);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, event);
}
