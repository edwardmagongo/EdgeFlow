#include <benchmark/benchmark.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include "bench_support.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/lock_free_bounded_queue.hpp"
#include "edgeflow/timed_event.hpp"

using edgeflow::BackpressurePolicy;
using edgeflow::TimedEvent;
using edgeflow::bench::make_sample_event;

using MutexQueue = edgeflow::BoundedQueue<TimedEvent>;
using LockFreeQueue = edgeflow::LockFreeBoundedQueue<TimedEvent>;

// Single-threaded push/pop baseline: no contention, establishes a floor.
template <typename Queue>
static void BM_Queue_SingleThreaded(benchmark::State& state) {
    Queue queue(1024, BackpressurePolicy::Block);
    std::int64_t device_id = 0;
    for (auto _ : state) {
        queue.push(make_sample_event(device_id++));
        benchmark::DoNotOptimize(queue.pop());
    }
}
BENCHMARK_TEMPLATE(BM_Queue_SingleThreaded, MutexQueue);
BENCHMARK_TEMPLATE(BM_Queue_SingleThreaded, LockFreeQueue);

// Fixture sharing one queue and one drain thread across all producer threads
// in a single benchmark::Threads(N) run, to measure push() throughput under
// real multi-producer contention against a single consumer. Members are
// static (not instance fields) because Google Benchmark constructs one
// Fixture instance PER THREAD for a threaded benchmark -- only thread 0's
// SetUp actually builds the shared queue and drain thread; Google Benchmark
// guarantees all threads' SetUp calls complete (via an internal barrier)
// before any thread enters its timed loop, so this is safe.
//
// Templated now, so the statics are per-queue-type and the two instantiations
// cannot tread on each other.
//
// NOTE: this is MPMC, which the gateway is not. It is kept as a stress
// measurement and for continuity with Phase 2's numbers; BM_Queue_SPMC_Push in
// bench_queue_spmc.cpp is the fixture that models production.
template <typename Queue>
class ContendedPushFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State& state) override {
        if (state.thread_index() != 0) return;
        queue_ = std::make_unique<Queue>(4096, BackpressurePolicy::Block);
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

    static std::unique_ptr<Queue> queue_;
    static std::unique_ptr<std::thread> drain_thread_;
    static std::atomic<bool> draining_;
};

template <typename Queue>
std::unique_ptr<Queue> ContendedPushFixture<Queue>::queue_;
template <typename Queue>
std::unique_ptr<std::thread> ContendedPushFixture<Queue>::drain_thread_;
template <typename Queue>
std::atomic<bool> ContendedPushFixture<Queue>::draining_{false};

BENCHMARK_TEMPLATE_DEFINE_F(ContendedPushFixture, PushMutex, MutexQueue)
(benchmark::State& state) {
    std::int64_t device_id = static_cast<std::int64_t>(state.thread_index()) * 1'000'000;
    for (auto _ : state) {
        queue_->push(make_sample_event(device_id++));
    }
}
BENCHMARK_REGISTER_F(ContendedPushFixture, PushMutex)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8);

BENCHMARK_TEMPLATE_DEFINE_F(ContendedPushFixture, PushLockFree, LockFreeQueue)
(benchmark::State& state) {
    std::int64_t device_id = static_cast<std::int64_t>(state.thread_index()) * 1'000'000;
    for (auto _ : state) {
        queue_->push(make_sample_event(device_id++));
    }
}
BENCHMARK_REGISTER_F(ContendedPushFixture, PushLockFree)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// Backpressure policy comparison: a small queue guaranteed to fill under a
// steady single-threaded push stream, showing how push() behaves once
// contention forces the issue under each policy.
template <typename Queue>
static void BM_Queue_BackpressurePolicy(benchmark::State& state) {
    auto policy = static_cast<BackpressurePolicy>(state.range(0));
    Queue queue(8, policy);
    std::int64_t device_id = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.push(make_sample_event(device_id++)));
    }
}
BENCHMARK_TEMPLATE(BM_Queue_BackpressurePolicy, MutexQueue)
    ->Arg(static_cast<int>(BackpressurePolicy::Block))
    ->Arg(static_cast<int>(BackpressurePolicy::DropOldest))
    ->Arg(static_cast<int>(BackpressurePolicy::DropNewest));
BENCHMARK_TEMPLATE(BM_Queue_BackpressurePolicy, LockFreeQueue)
    ->Arg(static_cast<int>(BackpressurePolicy::Block))
    ->Arg(static_cast<int>(BackpressurePolicy::DropOldest))
    ->Arg(static_cast<int>(BackpressurePolicy::DropNewest));

BENCHMARK_MAIN();
