#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include "edgeflow/bounded_queue.hpp"

namespace edgeflow::gateway {

struct Config {
    std::uint16_t port = 9000;
    std::size_t workers = 4;
    std::size_t queue_capacity = 1024;
    edgeflow::BackpressurePolicy backpressure = edgeflow::BackpressurePolicy::Block;
    std::size_t batch_size = 100;
    std::chrono::milliseconds batch_age{200};
    std::string sink_file = "edgeflow_events.ndjson";
};

// Parses argv into a Config. Throws std::invalid_argument on bad input.
Config parse_args(int argc, char** argv);

} // namespace edgeflow::gateway
