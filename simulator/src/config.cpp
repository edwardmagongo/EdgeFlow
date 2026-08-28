#include "edgeflow/simulator/config.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include "edgeflow/args.hpp"

namespace edgeflow::simulator {

namespace {

std::size_t parse_positive_size(std::string_view value, std::string_view flag_name) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    std::size_t pos = 0;
    unsigned long parsed;
    try {
        parsed = std::stoul(std::string(value), &pos);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    if (pos != value.size()) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    return static_cast<std::size_t>(parsed);
}

std::uint16_t parse_port(std::string_view value) {
    std::size_t raw = parse_positive_size(value, "--port");
    if (raw == 0 || raw > 65535) {
        throw std::invalid_argument("--port must be between 1 and 65535, got: " + std::string(value));
    }
    return static_cast<std::uint16_t>(raw);
}

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
            config.port = parse_port(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--devices=")) {
            config.device_count = parse_positive_size(edgeflow::arg_value(arg), "--devices");
        } else if (arg.starts_with("--rate=")) {
            config.events_per_second_per_device = parse_positive_double(edgeflow::arg_value(arg), "--rate");
        } else if (arg.starts_with("--duration=")) {
            config.duration_seconds = parse_positive_size(edgeflow::arg_value(arg), "--duration");
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
    }

    if (config.device_count == 0) {
        throw std::invalid_argument("--devices must be at least 1");
    }
    return config;
}

} // namespace edgeflow::simulator
