#include <benchmark/benchmark.h>
#include <cstdint>
#include <thread>
#include <vector>
#include "bench_support.hpp"
#include "edgeflow/bounded_queue.hpp"
#include "edgeflow/lock_free_bounded_queue.hpp"
#include "edgeflow/timed_event.hpp"

// NO BENCHMARK_MAIN() here -- bench_bounded_queue.cpp owns it. Adding a second
// one makes the link fail with a duplicate main.

using edgeflow::BackpressurePolicy;
using edgeflow::PushResult;
using edgeflow::TimedEvent;
using edgeflow::bench::make_sample_event;

using MutexQueue = edgeflow::BoundedQueue<TimedEvent>;
using LockFreeQueue = edgeflow::LockFreeBoundedQueue<TimedEvent>;

// THE fixture that models the real gateway: one producer, N consumers.
//
// The gateway's io_context is single-threaded, so exactly one thread ever calls
// push(); the contention that matters is N worker threads on pop(). This is a
// plain (non-threaded) benchmark rather than a Threads(N) one because Google
// Benchmark's threaded model runs the SAME loop on every thread, which cannot
// express "one producer, N consumers". Instead the consumers are spawned in
// untimed setup and the timed loop is the single producer -- exactly the
// gateway's shape.
//
// state.range(0) is the consumer count. Everything before the `for (auto _ :
// state)` loop and everything after it is untimed: Google Benchmark starts the
// clock when the loop is entered and stops it when the loop exits.
template <typename Queue>
static void BM_Queue_SPMC_Push(benchmark::State& state) {
    const int consumers = static_cast<int>(state.range(0));
    Queue queue(4096, BackpressurePolicy::Block);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(consumers));
    for (int i = 0; i < consumers; ++i) {
        // Mirrors WorkerPool::worker_loop(): pop until shutdown drains the queue.
        workers.emplace_back([&queue] {
            while (auto item = queue.pop()) {
                benchmark::DoNotOptimize(item);
            }
        });
    }

    std::int64_t device_id = 0;
    for (auto _ : state) {
        // Retry rather than drop, so every iteration corresponds to exactly one
        // real enqueue and the reported per-iteration cost includes the time
        // spent waiting on consumers that have fallen behind. Under Block the
        // queue never evicts, so nothing is silently lost.
        while (queue.push(make_sample_event(device_id++)) != PushResult::Accepted) {
            std::this_thread::yield();
        }
    }

    queue.shutdown();
    for (auto& worker : workers) {
        worker.join();
    }

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_Queue_SPMC_Push, MutexQueue)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8);
BENCHMARK_TEMPLATE(BM_Queue_SPMC_Push, LockFreeQueue)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8);
