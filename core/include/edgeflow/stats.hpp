#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace edgeflow {

// Plain snapshot of Stats' counters at a point in time (not synchronized as a
// unit — individual counters may be from slightly different instants).
struct StatsSnapshot {
    std::uint64_t events_accepted = 0;
    std::uint64_t events_dropped_oldest = 0;
    std::uint64_t events_dropped_newest = 0;
    std::uint64_t events_malformed = 0;

    // Queue-wait latency: time between an event being pushed onto the
    // gateway's BoundedQueue and a worker popping it. p50/p99 are histogram
    // bucket upper bounds (approximate, not exact percentiles), computed
    // from a small fixed set of buckets rather than storing every sample.
    std::uint64_t queue_wait_count = 0;
    std::uint64_t queue_wait_min_us = 0;
    std::uint64_t queue_wait_max_us = 0;
    double queue_wait_mean_us = 0.0;
    std::uint64_t queue_wait_p50_us = 0;
    std::uint64_t queue_wait_p99_us = 0;

    // Batch-level counters for the HTTP sink. Named batches_* so they cannot be
    // confused with the per-event events_* counters above: one batch carries
    // many events.
    std::uint64_t batches_sent = 0;
    std::uint64_t batches_retried = 0;           // retry ATTEMPTS, not batches
    std::uint64_t batches_dropped_outbound = 0;  // outbound queue was full
    std::uint64_t batches_dropped_exhausted = 0; // retries ran out, or shutdown deadline
};

// Lock-free counters giving observability into gateway ingestion,
// backpressure behavior, and queue-wait latency. Safe to update concurrently
// from multiple connections/worker threads.
class Stats {
public:
    void record_accepted() { events_accepted_.fetch_add(1, std::memory_order_relaxed); }
    void record_dropped_oldest() { events_dropped_oldest_.fetch_add(1, std::memory_order_relaxed); }
    void record_dropped_newest() { events_dropped_newest_.fetch_add(1, std::memory_order_relaxed); }
    void record_batch_sent() { batches_sent_.fetch_add(1, std::memory_order_relaxed); }
    void record_batch_retried() { batches_retried_.fetch_add(1, std::memory_order_relaxed); }
    void record_batch_dropped_outbound() {
        batches_dropped_outbound_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_batch_dropped_exhausted() {
        batches_dropped_exhausted_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_malformed() { events_malformed_.fetch_add(1, std::memory_order_relaxed); }

    // Records one queue-wait latency sample.
    void record_queue_wait(std::chrono::steady_clock::duration wait) {
        auto raw_us = std::chrono::duration_cast<std::chrono::microseconds>(wait).count();
        auto us = static_cast<std::uint64_t>(raw_us < 0 ? 0 : raw_us);

        queue_wait_count_.fetch_add(1, std::memory_order_relaxed);
        queue_wait_sum_us_.fetch_add(us, std::memory_order_relaxed);

        auto current_min = queue_wait_min_us_.load(std::memory_order_relaxed);
        while (us < current_min &&
               !queue_wait_min_us_.compare_exchange_weak(current_min, us, std::memory_order_relaxed)) {
        }
        auto current_max = queue_wait_max_us_.load(std::memory_order_relaxed);
        while (us > current_max &&
               !queue_wait_max_us_.compare_exchange_weak(current_max, us, std::memory_order_relaxed)) {
        }

        queue_wait_buckets_[bucket_for(us)].fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t events_accepted() const { return events_accepted_.load(std::memory_order_relaxed); }
    std::uint64_t events_dropped_oldest() const { return events_dropped_oldest_.load(std::memory_order_relaxed); }
    std::uint64_t events_dropped_newest() const { return events_dropped_newest_.load(std::memory_order_relaxed); }
    std::uint64_t events_malformed() const { return events_malformed_.load(std::memory_order_relaxed); }

    StatsSnapshot snapshot() const {
        StatsSnapshot snap;
        snap.events_accepted = events_accepted();
        snap.events_dropped_oldest = events_dropped_oldest();
        snap.events_dropped_newest = events_dropped_newest();
        snap.events_malformed = events_malformed();

        snap.batches_sent = batches_sent_.load(std::memory_order_relaxed);
        snap.batches_retried = batches_retried_.load(std::memory_order_relaxed);
        snap.batches_dropped_outbound = batches_dropped_outbound_.load(std::memory_order_relaxed);
        snap.batches_dropped_exhausted = batches_dropped_exhausted_.load(std::memory_order_relaxed);
        snap.queue_wait_count = queue_wait_count_.load(std::memory_order_relaxed);
        if (snap.queue_wait_count > 0) {
            snap.queue_wait_min_us = queue_wait_min_us_.load(std::memory_order_relaxed);
            snap.queue_wait_max_us = queue_wait_max_us_.load(std::memory_order_relaxed);
            snap.queue_wait_mean_us =
                static_cast<double>(queue_wait_sum_us_.load(std::memory_order_relaxed)) /
                static_cast<double>(snap.queue_wait_count);
            snap.queue_wait_p50_us = percentile(0.50);
            snap.queue_wait_p99_us = percentile(0.99);
        }
        return snap;
    }

private:
    // Exponential-ish bucket upper bounds, in microseconds, spanning
    // sub-millisecond through multi-second queue waits. Anything above the
    // last boundary falls into one final overflow bucket.
    static constexpr std::array<std::uint64_t, 16> kBucketBoundariesUs = {
        100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000,
        100000, 250000, 500000, 1000000, 2500000, 5000000, 10000000,
    };
    static constexpr std::size_t kBucketCount = kBucketBoundariesUs.size() + 1;

    static std::size_t bucket_for(std::uint64_t us) {
        for (std::size_t i = 0; i < kBucketBoundariesUs.size(); ++i) {
            if (us <= kBucketBoundariesUs[i]) {
                return i;
            }
        }
        return kBucketBoundariesUs.size();
    }

    std::uint64_t percentile(double fraction) const {
        auto total = queue_wait_count_.load(std::memory_order_relaxed);
        if (total == 0) {
            return 0;
        }
        auto target = static_cast<std::uint64_t>(fraction * static_cast<double>(total));
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            cumulative += queue_wait_buckets_[i].load(std::memory_order_relaxed);
            if (cumulative > target) {
                return i < kBucketBoundariesUs.size() ? kBucketBoundariesUs[i] : kBucketBoundariesUs.back();
            }
        }
        return kBucketBoundariesUs.back();
    }

    std::atomic<std::uint64_t> events_accepted_{0};
    std::atomic<std::uint64_t> events_dropped_oldest_{0};
    std::atomic<std::uint64_t> events_dropped_newest_{0};
    std::atomic<std::uint64_t> events_malformed_{0};

    std::atomic<std::uint64_t> queue_wait_count_{0};
    std::atomic<std::uint64_t> queue_wait_sum_us_{0};
    std::atomic<std::uint64_t> queue_wait_min_us_{UINT64_MAX};
    std::atomic<std::uint64_t> queue_wait_max_us_{0};
    std::array<std::atomic<std::uint64_t>, kBucketCount> queue_wait_buckets_{};
    std::atomic<std::uint64_t> batches_sent_{0};
    std::atomic<std::uint64_t> batches_retried_{0};
    std::atomic<std::uint64_t> batches_dropped_outbound_{0};
    std::atomic<std::uint64_t> batches_dropped_exhausted_{0};
};

} // namespace edgeflow
