add_executable(edgeflow-tests
    test_smoke.cpp
)

target_link_libraries(edgeflow-tests PRIVATE GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(edgeflow-tests)
