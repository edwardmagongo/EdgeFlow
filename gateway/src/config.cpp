#include "edgeflow/gateway/config.hpp"
#include <stdexcept>
#include <string_view>
#include "edgeflow/args.hpp"

namespace edgeflow::gateway {

namespace {
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
            config.port = static_cast<std::uint16_t>(std::stoi(std::string(edgeflow::arg_value(arg))));
        } else if (arg.starts_with("--workers=")) {
            config.workers = static_cast<std::size_t>(std::stoul(std::string(edgeflow::arg_value(arg))));
        } else if (arg.starts_with("--queue-capacity=")) {
            config.queue_capacity = static_cast<std::size_t>(std::stoul(std::string(edgeflow::arg_value(arg))));
        } else if (arg.starts_with("--backpressure=")) {
            config.backpressure = parse_policy(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--batch-size=")) {
            config.batch_size = static_cast<std::size_t>(std::stoul(std::string(edgeflow::arg_value(arg))));
        } else if (arg.starts_with("--batch-age-ms=")) {
            config.batch_age = std::chrono::milliseconds(std::stoul(std::string(edgeflow::arg_value(arg))));
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
