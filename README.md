# EdgeFlow

A concurrent C++20 telemetry pipeline: simulated devices → TCP gateway →
bounded queue → worker pool → batcher → sink.

## Build

    brew install boost cmake   # macOS prerequisites
    cmake -S . -B build
    cmake --build build

## Test

    ctest --test-dir build --output-on-failure
