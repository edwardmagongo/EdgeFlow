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

Real, measured numbers from `scripts/run_benchmarks.py`,
`scripts/run_saturation_sweep.py`, and
`./build-release/benchmarks/edgeflow-benchmarks` -- see `docs/benchmarks.md`
and `docs/saturation.md` for the full tables. Apple M-series, 10 cores,
`-DCMAKE_BUILD_TYPE=Release`, gateway and simulator sharing one machine.
Micro-benchmarks are medians of 9 repetitions; macro rows medians of 3.

### Gateway saturation (Phase 4)

**The gateway has never been made to drop an event.** Across a ladder of
offered rates from 25,000 to 800,000 events/sec, against both queue
implementations, every rung reported zero drops and `accepted == sent`
exactly. Peak observed: **462,163 events/sec accepted with nothing dropped**.

The load generator is the bottleneck at every rung above 50,000 events/sec,
not the gateway. Phase 4 cut the simulator's per-event cost (caching the ISO
timestamp: 4.65 -> 4.00 us/event) and added `--threads=N` to shard the fleet
across io_context threads, and it is still the limiting factor. The gateway's
ceiling remains **unknown and above 462,000 events/sec** -- it is a lower
bound, not a measurement of the gateway.

The disk is ruled out as the constraint: re-running the whole ladder with
`--sink-file=/dev/null` changed accepted throughput by under 5% on nine of
twelve rungs and produced zero drops either way.

One artefact worth knowing: the 800,000/sec rungs achieve *less* than the
400,000/sec rungs (~60,000 vs ~460,000). `DeviceClient` computes its send
interval as `1000 / events_per_second` truncated to whole milliseconds, so
per-device rates above 500/sec collapse onto a 1 ms interval and the fleet
stops scaling. That is a load-generator limitation, not a gateway one.

### Queue comparison (Phase 3, unchanged by Phase 4)

- SPMC micro-benchmark (one producer, N consumers -- the shape the gateway
  actually has): mutex 82.2 / 158 / 289 / 1308 ns per push at 1/2/4/8
  consumers, lock-free 53.2 / 53.1 / 138 / 205 ns. The lock-free queue is
  1.6x faster at one consumer and 6.4x at eight.
- End-to-end the two remain indistinguishable, and Phase 4 did **not** settle
  the question it set out to settle: since the gateway never saturates, the
  regime where the queue could matter is still out of reach. `BoundedQueue`
  (mutex) stays the production default, now for a measured reason rather than
  an assumed one.

Reproduce: build with `-DCMAKE_BUILD_TYPE=Release`, then
`python3 scripts/run_saturation_sweep.py --build-dir build-release`
(around ten minutes) and `python3 scripts/run_benchmarks.py --build-dir build-release`.
Run them on an otherwise idle machine -- throughput here swings several-fold
with background load.
