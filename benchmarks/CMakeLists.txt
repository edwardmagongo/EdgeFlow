add_executable(edgeflow-benchmarks
    bench_bounded_queue.cpp
    bench_queue_spmc.cpp
)
# So both translation units can include "bench_support.hpp".
target_include_directories(edgeflow-benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edgeflow-benchmarks PRIVATE
    edgeflow-core
    benchmark::benchmark
    Threads::Threads
)
