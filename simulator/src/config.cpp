#include "edgeflow/simulator/config.hpp"
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include "edgeflow/args.hpp"

namespace edgeflow::simulator {

namespace {

double parse_positive_double(std::string_view value, std::string_view flag_name) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    std::size_t pos = 0;
    double parsed;
    try {
        parsed = std::stod(std::string(value), &pos);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    if (pos != value.size()) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    if (!std::isfinite(parsed)) {
        throw std::invalid_argument(std::string(flag_name) + " must be finite, got: " + std::string(value));
    }
    if (parsed <= 0.0) {
        throw std::invalid_argument(std::string(flag_name) + " must be greater than 0, got: " + std::string(value));
    }
    return parsed;
}

} // namespace

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.starts_with("--host=")) {
            config.host = std::string(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--port=")) {
            config.port = edgeflow::parse_port(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--devices=")) {
            config.device_count = edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--devices");
        } else if (arg.starts_with("--rate=")) {
            config.events_per_second_per_device = parse_positive_double(edgeflow::arg_value(arg), "--rate");
        } else if (arg.starts_with("--duration=")) {
            config.duration_seconds = edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--duration");
        } else if (arg.starts_with("--chaos-latency-ms=")) {
            config.chaos_latency = std::chrono::milliseconds(
                edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--chaos-latency-ms"));
        } else if (arg.starts_with("--chaos-packet-loss-percent=")) {
            config.chaos_packet_loss_percent =
                edgeflow::parse_percentage(edgeflow::arg_value(arg), "--chaos-packet-loss-percent");
        } else if (arg.starts_with("--chaos-device-spike-at-sec=")) {
            config.chaos_device_spike_at_sec = edgeflow::parse_positive_size(
                edgeflow::arg_value(arg), "--chaos-device-spike-at-sec");
        } else if (arg.starts_with("--chaos-device-spike=")) {
            config.chaos_device_spike_count =
                edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--chaos-device-spike");
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
    }

    if (config.device_count == 0) {
        throw std::invalid_argument("--devices must be at least 1");
    }
    if (config.device_count > 100000) {
        throw std::invalid_argument("--devices exceeds maximum of 100000");
    }
    if (config.chaos_device_spike_count > 0 &&
        config.chaos_device_spike_at_sec >= config.duration_seconds) {
        throw std::invalid_argument(
            "--chaos-device-spike-at-sec must be less than --duration for the spike to occur");
    }
    return config;
}

} // namespace edgeflow::simulator
