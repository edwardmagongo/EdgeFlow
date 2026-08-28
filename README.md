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
device), `--duration` (seconds).

Inspect the output:

    wc -l /tmp/edgeflow_events.ndjson
    head -n 3 /tmp/edgeflow_events.ndjson

On shutdown (SIGINT/SIGTERM), the gateway prints its observability counters:
accepted, dropped_oldest, dropped_newest, and malformed event counts, e.g.

    edgeflow-gateway shut down (accepted=15000, dropped_oldest=0, dropped_newest=0, malformed=0)
