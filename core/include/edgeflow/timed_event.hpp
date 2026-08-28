#pragma once
#include <chrono>
#include "edgeflow/event.hpp"

namespace edgeflow {

// Wraps an Event with the steady-clock time it was enqueued onto the
// gateway's BoundedQueue, so a consumer can measure queue-wait latency.
// Internal only -- never serialized to the NDJSON wire format.
struct TimedEvent {
    Event event;
    std::chrono::steady_clock::time_point enqueued_at;
};

} // namespace edgeflow
