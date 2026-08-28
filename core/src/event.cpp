#include "edgeflow/event.hpp"
#include <nlohmann/json.hpp>

namespace edgeflow {

std::optional<Event> parse_event(const std::string& line) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error&) {
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
        return event;
    } catch (const nlohmann::json::type_error&) {
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
