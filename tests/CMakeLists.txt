add_executable(edgeflow-tests
    test_smoke.cpp
    test_event_parsing.cpp
    test_bounded_queue.cpp
    test_batcher.cpp
)

target_link_libraries(edgeflow-tests PRIVATE edgeflow-core GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(edgeflow-tests)
