#include "edgeflow/gateway/config.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
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

SinkKind parse_sink_kind(std::string_view value) {
    if (value == "file") return SinkKind::File;
    if (value == "http") return SinkKind::Http;
    throw std::invalid_argument("--sink must be file or http, got: " + std::string(value));
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg.starts_with("--port=")) {
            config.port = edgeflow::parse_port(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--workers=")) {
            config.workers = edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--workers");
        } else if (arg.starts_with("--queue-capacity=")) {
            config.queue_capacity = edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--queue-capacity");
        } else if (arg.starts_with("--backpressure=")) {
            config.backpressure = parse_policy(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--batch-size=")) {
            config.batch_size = edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--batch-size");
        } else if (arg.starts_with("--batch-age-ms=")) {
            config.batch_age = std::chrono::milliseconds(
                edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--batch-age-ms"));
        } else if (arg.starts_with("--sink-file=")) {
            config.sink_file = std::string(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--sink=")) {
            config.sink_kind = parse_sink_kind(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--sink-url=")) {
            config.sink_url = std::string(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--sink-outbound-capacity=")) {
            config.sink_outbound_capacity = edgeflow::parse_positive_size(
                edgeflow::arg_value(arg), "--sink-outbound-capacity");
        } else if (arg.starts_with("--sink-concurrency=")) {
            config.sink_concurrency =
                edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--sink-concurrency");
        } else if (arg.starts_with("--sink-backpressure=")) {
            config.sink_backpressure = parse_policy(edgeflow::arg_value(arg));
        } else if (arg.starts_with("--sink-max-retries=")) {
            config.sink_max_retries =
                edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--sink-max-retries");
        } else if (arg.starts_with("--sink-backoff-ms=")) {
            config.sink_backoff_ms = std::chrono::milliseconds(
                edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--sink-backoff-ms"));
        } else if (arg.starts_with("--sink-timeout-ms=")) {
            config.sink_timeout_ms = std::chrono::milliseconds(
                edgeflow::parse_positive_size(edgeflow::arg_value(arg), "--sink-timeout-ms"));
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
    if (config.sink_kind == SinkKind::Http && config.sink_url.empty()) {
        throw std::invalid_argument("--sink=http requires --sink-url");
    }
    if (config.sink_outbound_capacity == 0) {
        throw std::invalid_argument("--sink-outbound-capacity must be at least 1");
    }
    if (config.sink_concurrency == 0) {
        throw std::invalid_argument("--sink-concurrency must be at least 1");
    }
    // Upper bound, not just a lower one. The sink is I/O-bound (Phase 10's
    // attribution put `wait` at 73.9-88.3% of the round trip), so a thread
    // count far above the core count buys nothing -- while an absurd value
    // makes std::thread's constructor throw partway through HttpSink's spawn
    // loop, which is a startup abort rather than a legible error. Reject it
    // here, where the flag can still be reported by name.
    if (config.sink_concurrency > kMaxSinkConcurrency) {
        throw std::invalid_argument("--sink-concurrency must be at most " +
                                    std::to_string(kMaxSinkConcurrency));
    }
    return config;
}

} // namespace edgeflow::gateway
