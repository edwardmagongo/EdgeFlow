#pragma once
#include <cstdint>
#include <string>

namespace edgeflow::simulator {

struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::size_t device_count = 1000;
    double events_per_second_per_device = 1.0;
    std::size_t duration_seconds = 30;
};

// Parses argv into a Config. Throws std::invalid_argument on bad input.
Config parse_args(int argc, char** argv);

} // namespace edgeflow::simulator
