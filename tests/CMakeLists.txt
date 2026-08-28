add_executable(edgeflow-tests
    test_smoke.cpp
    test_event_parsing.cpp
)

target_link_libraries(edgeflow-tests PRIVATE edgeflow-core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(edgeflow-tests)
