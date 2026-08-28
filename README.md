# EdgeFlow

A concurrent C++20 telemetry pipeline: simulated devices → TCP gateway →
bounded queue → worker pool → batcher → sink.

Phases 1-3 have landed: the C++ core pipeline with a file-backed sink
(Phase 1), a benchmark suite and simulator chaos scenarios (Phase 2), and a
lock-free queue variant benchmarked against the mutex one (Phase 3). No cloud
backend, database, or dashboard yet. Each phase has its own spec under
`docs/superpowers/specs/`.

## Build

    brew install boost cmake   # macOS prerequisites
    cmake -S . -B build
    cmake --build build

## Test

    ctest --test-dir build --output-on-failure

Sanitizer builds:

    cmake -S . -B build-asan -DEDGEFLOW_SANITIZE=address
    cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

    cmake -S . -B build-tsan -DEDGEFLOW_SANITIZE=thread
    cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure

## Run

Start the gateway:

    ./build/gateway/edgeflow-gateway --port=9000 --workers=4 \
        --queue-capacity=2000 --backpressure=block \
        --batch-size=100 --batch-age-ms=200 \
        --sink-file=/tmp/edgeflow_events.ndjson

Gateway flags: `--port`, `--workers`, `--queue-capacity`, `--backpressure`
(`block`|`drop-oldest`|`drop-newest`), `--batch-size`, `--batch-age-ms`,
`--sink-file`.

In another terminal, run ~1,000 simulated devices for 15 seconds:

    ./build/simulator/edgeflow-simulator --devices=1000 --rate=1 --duration=15

Simulator flags: `--host`, `--port`, `--devices`, `--rate` (events/sec per
device), `--duration` (seconds), `--chaos-latency-ms` (extra per-event send
delay), `--chaos-packet-loss-percent` (0-100, chance a device skips a send --
models a device failing to produce telemetry, not true network packet loss),
`--chaos-device-spike` + `--chaos-device-spike-at-sec` (add N devices mid-run
at the given second).

Inspect the output:

    wc -l /tmp/edgeflow_events.ndjson
    head -n 3 /tmp/edgeflow_events.ndjson

On shutdown (SIGINT/SIGTERM), the gateway prints its observability counters:
accepted, dropped_oldest, dropped_newest, and malformed event counts, plus
queue-wait latency (time from a connection enqueueing an event to a worker
popping it), e.g.

    edgeflow-gateway shut down (accepted=14800, dropped_oldest=0, dropped_newest=0, malformed=0, queue_wait_count=14800, queue_wait_mean_us=17.4695, queue_wait_p50_us=100, queue_wait_p99_us=100)

`queue_wait_mean_us` is exact; the p50/p99 figures come from a fixed
histogram whose lowest bucket boundary is 100us, so a reported `p50=100`
means "at or below 100us", not "exactly 100us".

## Benchmarks

Real, measured numbers from `scripts/run_benchmarks.py` and
`./build-release/benchmarks/edgeflow-benchmarks` -- see `docs/benchmarks.md`
for the full results table. Apple M-series, 10 cores,
`-DCMAKE_BUILD_TYPE=Release`, gateway and simulator sharing one machine.
Micro-benchmarks are the median of 9 repetitions; macro rows are the median of
3 full matrix runs.

- Peak observed throughput: 23,800 events/sec sustained over 10s (500 devices
  x 50 events/sec, 4 workers, queue capacity 64, `--backpressure=block`), with
  zero dropped and zero malformed events.
- SPMC micro-benchmark (one producer, N consumers -- the shape the gateway
  actually has): mutex 82.2 / 158 / 289 / 1308 ns per push at 1/2/4/8
  consumers, lock-free 53.2 / 53.1 / 138 / 205 ns. The lock-free queue is
  1.6x faster at one consumer and 6.4x faster at eight; the mutex queue
  degrades sharply past four consumers while the lock-free one stays flat.
- End-to-end across the full macro matrix, the two queues differed by less
  than 2.5% on throughput in every row -- a null result, and the expected one.
  The pipeline is offered-load-limited, not queue-limited: 200 devices x 10
  events/sec offers 2,000 events/sec and both queues accept ~1,960 of them
  with zero drops. No queue implementation can raise a ceiling the simulator
  sets.
- The lock-free queue showed *higher* mean queue-wait latency in most macro
  rows. The direction was consistent, but run-to-run magnitudes varied by up
  to 17x, so it is reported as a direction and not quantified. The mechanism
  is that at these loads the queue is empty almost always, so nearly every
  event pays the park/wake path rather than the lock-free fast path: the mutex
  queue notifies under the very mutex its consumer waits on, while the
  lock-free queue must fence, read its waiter count, and then take that mutex
  anyway -- strictly more work when there is never a backlog to absorb.
- Worker count still makes no difference to throughput, for the same
  offered-load reason: 1, 2, 4 and 8 workers all land at ~1,960 events/sec.
- Chaos scenarios behave identically on both queues: 20% packet loss and the
  +100-device spike move throughput by the same amount either way.

**Conclusion: `BoundedQueue` (mutex) stays the production default.** The
lock-free queue is decisively faster under saturation, and the gateway never
reaches saturation. `LockFreeBoundedQueue` is kept, tested and benchmarked so
the choice can be revisited if a future phase raises the offered load past the
point where the queue becomes the bottleneck.

Reproduce: build with `-DCMAKE_BUILD_TYPE=Release`, then run
`python3 scripts/run_benchmarks.py --build-dir build-release` (several minutes)
and `./build-release/benchmarks/edgeflow-benchmarks --benchmark_repetitions=9
--benchmark_report_aggregates_only=true`.
