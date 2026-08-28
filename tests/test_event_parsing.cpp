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

TEST(EventParsing, RejectsNonFiniteTemperature) {
    // 1e400 is rejected during JSON parsing itself: nlohmann::json (v3.11.3)
    // throws out_of_range while parsing a numeric literal that overflows
    // double range, rather than silently producing +inf. This is why
    // parse_event's catch clause has to catch more than just parse_error --
    // this is the exact network-reachable path that used to slip an inf
    // through to the sink before that catch clause was broadened.
    const std::string line =
        R"({"device_id":1,"timestamp":"2026-08-28T00:00:00Z","temperature":1e400,)"
        R"("battery":100,"latitude":0.0,"longitude":0.0,"event_type":"telemetry"})";

    auto result = parse_event(line);
    EXPECT_FALSE(result.has_value());
}

TEST(EventParsing, RejectsNonFiniteLatitude) {
    const std::string line =
        R"({"device_id":1,"timestamp":"2026-08-28T00:00:00Z","temperature":20.0,)"
        R"("battery":100,"latitude":-1e400,"longitude":0.0,"event_type":"telemetry"})";

    auto result = parse_event(line);
    EXPECT_FALSE(result.has_value());
}

TEST(EventParsing, RejectsNonFiniteLongitude) {
    const std::string line =
        R"({"device_id":1,"timestamp":"2026-08-28T00:00:00Z","temperature":20.0,)"
        R"("battery":100,"latitude":0.0,"longitude":1e400,"event_type":"telemetry"})";

    auto result = parse_event(line);
    EXPECT_FALSE(result.has_value());
}

TEST(EventParsing, RoundTripsThroughSerialize) {
    Event event{18291, "2026-08-28T09:41:22Z", 72.4, 81, 37.7749, -122.4194, "telemetry"};
    auto line = serialize_event(event);
    auto parsed = parse_event(line);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, event);
}
