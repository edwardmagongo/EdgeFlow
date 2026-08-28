#include "edgeflow/event.hpp"
#include <cmath>
#include <nlohmann/json.hpp>

namespace edgeflow {

std::optional<Event> parse_event(const std::string& line) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        // Covers parse_error (malformed JSON) as well as out_of_range, which
        // nlohmann throws *during parsing* (not just on later .get<>() calls)
        // for a numeric literal like 1e400 that overflows double's range --
        // catching only parse_error let that exception escape parse_event()
        // entirely and propagate up to the connection's read handler.
        return std::nullopt;
    }

    if (!j.contains("device_id") || !j.contains("timestamp") ||
        !j.contains("temperature") || !j.contains("battery") ||
        !j.contains("latitude") || !j.contains("longitude") ||
        !j.contains("event_type")) {
        return std::nullopt;
    }

    try {
        Event event;
        event.device_id = j.at("device_id").get<std::int64_t>();
        event.timestamp = j.at("timestamp").get<std::string>();
        event.temperature = j.at("temperature").get<double>();
        event.battery = j.at("battery").get<int>();
        event.latitude = j.at("latitude").get<double>();
        event.longitude = j.at("longitude").get<double>();
        event.event_type = j.at("event_type").get<std::string>();

        // Defense in depth: an inf/nan double field would round-trip through
        // serialize_event() as JSON null (nlohmann::json::dump()'s behavior
        // for non-finite doubles), producing sink output that parse_event()
        // itself can no longer parse back. In this nlohmann version an
        // out-of-range numeric literal (e.g. 1e400) is already rejected
        // above during parse(), but this check guards any other path that
        // could otherwise land a non-finite value in an Event.
        if (!std::isfinite(event.temperature) || !std::isfinite(event.latitude) ||
            !std::isfinite(event.longitude)) {
            return std::nullopt;
        }

        return event;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::string serialize_event(const Event& event) {
    nlohmann::json j;
    j["device_id"] = event.device_id;
    j["timestamp"] = event.timestamp;
    j["temperature"] = event.temperature;
    j["battery"] = event.battery;
    j["latitude"] = event.latitude;
    j["longitude"] = event.longitude;
    j["event_type"] = event.event_type;
    return j.dump();
}

} // namespace edgeflow
