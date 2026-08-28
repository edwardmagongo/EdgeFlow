#pragma once
#include <atomic>
#include <cstdint>

namespace edgeflow {

// Plain snapshot of Stats' counters at a point in time (not synchronized as a
// unit — individual counters may be from slightly different instants).
struct StatsSnapshot {
    std::uint64_t events_accepted = 0;
    std::uint64_t events_dropped_oldest = 0;
    std::uint64_t events_dropped_newest = 0;
    std::uint64_t events_malformed = 0;
};

// Lock-free counters giving observability into gateway ingestion and
// backpressure behavior: how many events were accepted, dropped under each
// backpressure policy, or rejected for being malformed NDJSON. Safe to
// increment concurrently from multiple connections/threads.
class Stats {
public:
    void record_accepted() { events_accepted_.fetch_add(1, std::memory_order_relaxed); }
    void record_dropped_oldest() { events_dropped_oldest_.fetch_add(1, std::memory_order_relaxed); }
    void record_dropped_newest() { events_dropped_newest_.fetch_add(1, std::memory_order_relaxed); }
    void record_malformed() { events_malformed_.fetch_add(1, std::memory_order_relaxed); }

    std::uint64_t events_accepted() const { return events_accepted_.load(std::memory_order_relaxed); }
    std::uint64_t events_dropped_oldest() const { return events_dropped_oldest_.load(std::memory_order_relaxed); }
    std::uint64_t events_dropped_newest() const { return events_dropped_newest_.load(std::memory_order_relaxed); }
    std::uint64_t events_malformed() const { return events_malformed_.load(std::memory_order_relaxed); }

    StatsSnapshot snapshot() const {
        return StatsSnapshot{
            events_accepted(),
            events_dropped_oldest(),
            events_dropped_newest(),
            events_malformed(),
        };
    }

private:
    std::atomic<std::uint64_t> events_accepted_{0};
    std::atomic<std::uint64_t> events_dropped_oldest_{0};
    std::atomic<std::uint64_t> events_dropped_newest_{0};
    std::atomic<std::uint64_t> events_malformed_{0};
};

} // namespace edgeflow
