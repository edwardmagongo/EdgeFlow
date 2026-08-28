add_executable(edgeflow-tests
    test_smoke.cpp
    test_event_parsing.cpp
    test_bounded_queue.cpp
    test_batcher.cpp
    test_file_sink.cpp
    test_worker_pool.cpp
    test_args.cpp
    test_gateway_config.cpp
    test_simulator_config.cpp
    test_stats.cpp
    test_device_client_integration.cpp
)

target_link_libraries(edgeflow-tests PRIVATE
    edgeflow-core
    edgeflow-gateway-lib
    edgeflow-simulator-lib
    GTest::gtest_main
    Boost::system
    Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(edgeflow-tests)
