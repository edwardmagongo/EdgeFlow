#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include "edgeflow/queue_types.hpp"

namespace edgeflow::gateway {

// Which Sink implementation the gateway constructs at startup.
enum class SinkKind { File, Http };

// Ceiling on --sink-concurrency. Generous relative to any plausible core count
// (the sink is I/O-bound, so oversubscribing a little is reasonable and
// oversubscribing a lot is pointless), but low enough that the value can never
// reach std::thread's own resource limit inside HttpSink's spawn loop.
inline constexpr std::size_t kMaxSinkConcurrency = 1024;

struct Config {
    std::uint16_t port = 9000;
    std::size_t workers = 4;
    std::size_t queue_capacity = 1024;
    edgeflow::BackpressurePolicy backpressure = edgeflow::BackpressurePolicy::Block;
    std::size_t batch_size = 100;
    std::chrono::milliseconds batch_age{200};
    std::string sink_file = "edgeflow_events.ndjson";

    // HTTP sink. sink_backpressure governs the OUTBOUND queue and is separate
    // from `backpressure` above, which governs the event queue.
    SinkKind sink_kind = SinkKind::File;
    std::string sink_url;
    std::size_t sink_outbound_capacity = 256;
    std::size_t sink_concurrency = 4;
    edgeflow::BackpressurePolicy sink_backpressure = edgeflow::BackpressurePolicy::DropOldest;
    std::size_t sink_max_retries = 3;
    std::chrono::milliseconds sink_backoff_ms{100};
    std::chrono::milliseconds sink_timeout_ms{5000};
};

// Parses argv into a Config. Throws std::invalid_argument on bad input.
Config parse_args(int argc, char** argv);

} // namespace edgeflow::gateway
