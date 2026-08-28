#pragma once
#include <chrono>
#include <cstdint>
#include <string>

namespace edgeflow::simulator {

struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::size_t device_count = 1000;
    double events_per_second_per_device = 1.0;
    std::size_t duration_seconds = 30;

    // Number of io_context threads the fleet is sharded across. 1 keeps the
    // original single-threaded behaviour exactly.
    std::size_t thread_count = 1;

    // Chaos scenarios, all off (zero/0.0) by default.
    std::chrono::milliseconds chaos_latency{0};
    double chaos_packet_loss_percent = 0.0;
    std::size_t chaos_device_spike_count = 0;
    std::size_t chaos_device_spike_at_sec = 0;
};

// Parses argv into a Config. Throws std::invalid_argument on bad input.
Config parse_args(int argc, char** argv);

} // namespace edgeflow::simulator
