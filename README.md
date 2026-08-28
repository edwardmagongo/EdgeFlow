# EdgeFlow

A concurrent C++20 telemetry pipeline: simulated devices → TCP gateway →
bounded queue → worker pool → batcher → sink.

Phase 1 (this codebase): the C++ core pipeline only, with a file-backed
sink. No cloud backend, database, or dashboard yet — see
`docs/superpowers/specs/2026-08-28-phase1-cpp-core-design.md` for scope.

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
`./build/benchmarks/edgeflow-benchmarks` -- see `docs/benchmarks.md` for the
full results table. Highlights from the most recent run (Apple M-series,
10 cores, `-DCMAKE_BUILD_TYPE=Release`, gateway and simulator sharing the
one machine):

- Peak observed throughput: 23,742 events/sec sustained over 10s
  (500 devices x 50 events/sec, 4 workers, queue capacity 50,
  `--backpressure=block`), with zero dropped and zero malformed events.
- Queue-wait latency at that peak: mean 36.2us, p50 <=100us, p99 <=500us.
- Worker count made no difference to throughput in this matrix: 1, 2, 4 and
  8 workers all accepted exactly 19,600 events at 200 devices x 10/sec. The
  run is offered-load-limited, not gateway-limited (zero drops in every row),
  so it measures what the simulator sent, not what the gateway could take.
  Mean queue wait did move -- 45.8us / 46.9us / 38.0us / 57.8us for 1/2/4/8
  workers -- i.e. 8 workers contend on the queue's single mutex for no
  throughput gain.
- Chaos vs baseline (100 devices x 10/sec for 15s, 14,800 events accepted
  clean): 20% packet loss accepted 11,891 (-19.7%, matching the configured
  rate); a +100-device spike at t=5s accepted 24,600 (+66%, matching the
  166.7 average device-seconds); 200ms injected send latency accepted 6,600
  (-55%) and *lowered* mean queue wait to 11.1us, because the delay gates
  each device's next send rather than bursting -- it reduces offered load.

Reproduce: build the project, then run `python3 scripts/run_benchmarks.py`
(takes a few minutes) and `./build/benchmarks/edgeflow-benchmarks`.
