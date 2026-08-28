#include <benchmark/benchmark.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/event.hpp"
#include "edgeflow/timed_event.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::BoundedQueue;
using edgeflow::Event;
using edgeflow::TimedEvent;

namespace {

TimedEvent make_sample_event(std::int64_t device_id) {
    Event event{
        device_id,
        "2026-08-28T00:00:00Z",
        20.0 + static_cast<double>(device_id % 15),
        static_cast<int>(100 - (device_id % 100)),
        37.7749,
        -122.4194,
        "telemetry",
    };
    return TimedEvent{event, std::chrono::steady_clock::now()};
}

} // namespace

// Single-threaded push/pop baseline: no contention, establishes a floor.
static void BM_BoundedQueue_SingleThreaded(benchmark::State& state) {
    BoundedQueue<TimedEvent> queue(1024, BackpressurePolicy::Block);
    std::int64_t device_id = 0;
    for (auto _ : state) {
        queue.push(make_sample_event(device_id++));
        benchmark::DoNotOptimize(queue.pop());
    }
}
BENCHMARK(BM_BoundedQueue_SingleThreaded);

// Fixture sharing one queue and one drain thread across all producer threads
// in a single benchmark::Threads(N) run, to measure push() throughput under
// real multi-producer contention against a single consumer. Members are
// static (not instance fields) because Google Benchmark constructs one
// Fixture instance PER THREAD for a threaded benchmark -- only thread 0's
// SetUp actually builds the shared queue and drain thread; Google Benchmark
// guarantees all threads' SetUp calls complete (via an internal barrier)
// before any thread enters its timed loop, so this is safe.
class ContendedPushFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State& state) override {
        if (state.thread_index() != 0) return;
        queue_ = std::make_unique<BoundedQueue<TimedEvent>>(4096, BackpressurePolicy::Block);
        draining_ = true;
        drain_thread_ = std::make_unique<std::thread>([] {
            while (draining_.load(std::memory_order_relaxed)) {
                queue_->pop();
            }
        });
    }

    void TearDown(const benchmark::State& state) override {
        if (state.thread_index() != 0) return;
        draining_ = false;
        queue_->shutdown();
        drain_thread_->join();
        drain_thread_.reset();
        queue_.reset();
    }

    static std::unique_ptr<BoundedQueue<TimedEvent>> queue_;
    static std::unique_ptr<std::thread> drain_thread_;
    static std::atomic<bool> draining_;
};

std::unique_ptr<BoundedQueue<TimedEvent>> ContendedPushFixture::queue_;
std::unique_ptr<std::thread> ContendedPushFixture::drain_thread_;
std::atomic<bool> ContendedPushFixture::draining_{false};

BENCHMARK_DEFINE_F(ContendedPushFixture, Push)(benchmark::State& state) {
    std::int64_t device_id = static_cast<std::int64_t>(state.thread_index()) * 1'000'000;
    for (auto _ : state) {
        queue_->push(make_sample_event(device_id++));
    }
}
BENCHMARK_REGISTER_F(ContendedPushFixture, Push)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// Backpressure policy comparison: a small queue guaranteed to fill under a
// steady single-threaded push stream, showing how push() behaves once
// contention forces the issue under each policy.
static void BM_BoundedQueue_BackpressurePolicy(benchmark::State& state) {
    auto policy = static_cast<BackpressurePolicy>(state.range(0));
    BoundedQueue<TimedEvent> queue(8, policy);
    std::int64_t device_id = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.push(make_sample_event(device_id++)));
    }
}
BENCHMARK(BM_BoundedQueue_BackpressurePolicy)
    ->Arg(static_cast<int>(BackpressurePolicy::Block))
    ->Arg(static_cast<int>(BackpressurePolicy::DropOldest))
    ->Arg(static_cast<int>(BackpressurePolicy::DropNewest));

BENCHMARK_MAIN();
