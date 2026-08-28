add_executable(edgeflow-benchmarks
    bench_bounded_queue.cpp
)
target_link_libraries(edgeflow-benchmarks PRIVATE
    edgeflow-core
    benchmark::benchmark
    Threads::Threads
)
