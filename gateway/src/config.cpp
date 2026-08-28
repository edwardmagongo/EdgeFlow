#include "edgeflow/gateway/config.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include "edgeflow/args.hpp"

namespace edgeflow::gateway {

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

edgeflow::BackpressurePolicy parse_policy(std::string_view value) {
    if (value == "block") return edgeflow::BackpressurePolicy::Block;
    if (value == "drop-oldest") return edgeflow::BackpressurePolicy::DropOldest;
    if (value == "drop-newest") return edgeflow::BackpressurePolicy::DropNewest;
    throw std::invalid_argument("unknown --backpressure value: " + std::string(value));
}

} // namespace

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.starts_with("--port=")) {
            config.port = parse_port(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--workers=")) {
            config.workers = parse_positive_size(edgeflow::arg_value(arg), "--workers");
        } else if (arg.starts_with("--queue-capacity=")) {
            config.queue_capacity = parse_positive_size(edgeflow::arg_value(arg), "--queue-capacity");
        } else if (arg.starts_with("--backpressure=")) {
            config.backpressure = parse_policy(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--batch-size=")) {
            config.batch_size = parse_positive_size(edgeflow::arg_value(arg), "--batch-size");
        } else if (arg.starts_with("--batch-age-ms=")) {
            config.batch_age = std::chrono::milliseconds(
                parse_positive_size(edgeflow::arg_value(arg), "--batch-age-ms"));
        } else if (arg.starts_with("--sink-file=")) {
            config.sink_file = std::string(edgeflow::arg_value(arg));
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
    }

    if (config.workers == 0) {
        throw std::invalid_argument("--workers must be at least 1");
    }
    if (config.queue_capacity == 0) {
        throw std::invalid_argument("--queue-capacity must be at least 1");
    }
    return config;
}

} // namespace edgeflow::gateway
