#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>
#include "edgeflow/event.hpp"

namespace edgeflow {

// Accumulates events and flushes a batch when either the size or time
// threshold is reached. add_event() must be called for the time threshold
// to be checked, since Batcher does not run its own timer thread.
class Batcher {
public:
    using FlushCallback = std::function<void(std::vector<Event>)>;

    Batcher(std::size_t max_batch_size,
            std::chrono::milliseconds max_batch_age,
            FlushCallback on_flush);

    // Adds an event to the current batch. Flushes synchronously (calling
    // on_flush on the calling thread) if a threshold is crossed.
    void add_event(Event event);

    // Flushes whatever is currently buffered, even if under threshold.
    // Used for graceful shutdown.
    void flush();

private:
    bool should_flush_locked() const;
    void flush_locked();

    std::size_t max_batch_size_;
    std::chrono::milliseconds max_batch_age_;
    FlushCallback on_flush_;

    std::mutex mutex_;
    std::vector<Event> buffer_;
    std::chrono::steady_clock::time_point batch_started_at_;
};

} // namespace edgeflow
