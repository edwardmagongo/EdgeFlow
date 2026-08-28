#pragma once
#include <chrono>
#include <cstdint>
#include "edgeflow/event.hpp"
#include "edgeflow/timed_event.hpp"

namespace edgeflow::bench {

// One representative telemetry event. Shared by both benchmark translation
// units so the two measure identical payloads -- a differently-sized Event
// would change copy and move costs and make the numbers incomparable.
inline edgeflow::TimedEvent make_sample_event(std::int64_t device_id) {
    edgeflow::Event event{
        device_id,
        "2026-08-28T00:00:00Z",
        20.0 + static_cast<double>(device_id % 15),
        static_cast<int>(100 - (device_id % 100)),
        37.7749,
        -122.4194,
        "telemetry",
    };
    return edgeflow::TimedEvent{event, std::chrono::steady_clock::now()};
}

} // namespace edgeflow::bench
