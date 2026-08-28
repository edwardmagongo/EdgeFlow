#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace edgeflow {

struct Event {
    std::int64_t device_id = 0;
    std::string timestamp;
    double temperature = 0.0;
    int battery = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    std::string event_type;

    bool operator==(const Event&) const = default;
};

// Parses one line of newline-delimited JSON into an Event.
// Returns std::nullopt if the line is not valid JSON or is missing required fields.
std::optional<Event> parse_event(const std::string& line);

// Serializes an Event back to a single JSON line (no trailing newline).
std::string serialize_event(const Event& event);

} // namespace edgeflow
