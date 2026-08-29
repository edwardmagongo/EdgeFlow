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

HTTP sink flags: `--sink` (`file`|`http`), `--sink-url`,
`--sink-outbound-capacity` (batches held for the backend, default 256),
`--sink-backpressure` (governs the *outbound* queue, separately from
`--backpressure` which governs the event queue), `--sink-max-retries`,
`--sink-backoff-ms`, `--sink-timeout-ms`. Plain HTTP only -- no TLS, no auth.

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

**The gateway saturates at roughly 200,000 events/sec offered** on this
machine. Below that both queues accept everything: 196,596 events/sec
accepted at the 200,000/sec rung with zero drops and ~40us mean queue wait.
Above it the pipeline stops keeping up -- accepted falls below sent and mean
queue wait jumps by an order of magnitude (360us for the mutex queue, 4,241us
for the lock-free one at the 400,000/sec rung).

Saturation shows up as **queue-wait blowing out, not as drops**. Under the
default `--backpressure=block` the connection stops reading and retries rather
than discarding, so `dropped_oldest`/`dropped_newest` stay at zero even when
the gateway is well past its limit. A sweep that only watched drop counters
would report "never saturated" forever; `scripts/run_saturation_sweep.py`
classifies a rung on queue-wait and on `accepted < sent` as well.

Two load-generator findings, both measured:

- **More generator threads make things worse here.** At 1000 devices x 400/sec:
  1 thread sends 371,506/sec, 2 threads 338,451/sec, 3 threads 71,662/sec.
  Generator and gateway share ten cores, so extra generator threads take cores
  the gateway needs. `--threads` exists and works; on one machine, leave it at 1.
- **The send interval is computed in microseconds.** It used to be whole
  milliseconds, which silently rounded any rate that did not divide 1000 evenly
  *upwards* -- 400/sec became 500/sec, 800/sec became 1000/sec -- capping the
  fleet near 500,000 events/sec and making the ladder's top rungs measure
  demand nobody asked for.

The disk is not the constraint: re-running the whole ladder with
`--sink-file=/dev/null` changed accepted throughput by under 5% on nine of
twelve rungs, with zero drops either way.

Because generator and gateway share one machine, the knee is a **lower bound**.
A dedicated load box would very likely push it higher.

### HTTP sink (Phase 5)

The gateway can POST batches to an HTTP endpoint instead of writing them to a
file: `--sink=http --sink-url=http://host:port/path`. The body is NDJSON,
**byte-identical to what `FileSink` writes**, so a file can be replayed to the
backend with plain `curl`.

- **End to end, nothing is lost while the backend is healthy.** 1000 devices at
  50 events/sec for 20s: 938,191 events accepted, 9,430 batches sent, and the
  backend received exactly 938,191 lines. Zero retried, zero dropped.
- **The network sink does not move the saturation knee.** Re-running Phase 4's
  full ladder against a local HTTP backend and against the file sink,
  back to back at load average ~2.3, three repetitions each: accepted
  throughput differed by under 1% on eight of twelve rungs, with zero drops
  under either sink. The two larger gaps (-9.9% and -30.5%) both fall on
  rungs already classified generator-limited, where the load generator rather
  than the sink is the variable.
- Mean queue wait stayed at ~5.6us with the HTTP sink under a 20-device load,
  confirming worker threads are not blocking on the network -- `consume()`
  queues the batch and returns, and a dedicated thread does the round trips.

Four counters on the shutdown line account for every batch: `batches_sent`,
`batches_retried` (attempts, not batches), `batches_dropped_outbound` (the
outbound queue was full) and `batches_dropped_exhausted` (retries ran out, or
the 5s shutdown drain deadline passed). The two drop counters are separate so a
backend outage reads differently from a traffic burst.

**The sink speaks plain HTTP with no TLS and no authentication.** It is for a
trusted network or a local backend only; both belong with the service itself,
which is a later phase.

### Queue comparison (Phase 3, unchanged by Phase 4)

- SPMC micro-benchmark (one producer, N consumers -- the shape the gateway
  actually has): mutex 82.2 / 158 / 289 / 1308 ns per push at 1/2/4/8
  consumers, lock-free 53.2 / 53.1 / 138 / 205 ns. The lock-free queue is
  1.6x faster at one consumer and 6.4x at eight.
- Below the knee the two are indistinguishable end to end. **Above it the
  mutex queue wins**, which is the opposite of what the micro-benchmark
  predicts: at the 400,000/sec rung the mutex gateway sustained 177,737
  events/sec at 361us mean queue wait, the lock-free one 114,794 at 4,241us.
  Worth treating as provisional -- it is three repetitions on a shared machine,
  and the two differ by more past the knee than anything measured below it.
- `BoundedQueue` (mutex) stays the production default. Three phases have now
  failed to find a load at which it is the bottleneck, and the one regime where
  the two measurably diverge favours it.

Reproduce: build with `-DCMAKE_BUILD_TYPE=Release`, then
`python3 scripts/run_saturation_sweep.py --build-dir build-release`
(around ten minutes) and `python3 scripts/run_benchmarks.py --build-dir build-release`.
Run them on an otherwise idle machine -- throughput here swings several-fold
with background load.
