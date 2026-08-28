add_executable(edgeflow-tests
    test_smoke.cpp
    test_event_parsing.cpp
    test_bounded_queue.cpp
    test_batcher.cpp
    test_file_sink.cpp
    test_worker_pool.cpp
    test_args.cpp
    test_gateway_config.cpp
)

target_link_libraries(edgeflow-tests PRIVATE
    edgeflow-core
    edgeflow-gateway-config
    GTest::gtest_main
    Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(edgeflow-tests)
