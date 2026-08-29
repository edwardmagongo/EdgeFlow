add_library(edgeflow-gateway-lib
    src/config.cpp
    src/http_sink.cpp
)
target_include_directories(edgeflow-gateway-lib PUBLIC include)
target_link_libraries(edgeflow-gateway-lib PUBLIC
    edgeflow-core
    Boost::system
    Threads::Threads
)

# Two binaries, one main.cpp. The only difference is the compile definition that
# selects the queue type; keeping a single source guarantees the two gateways
# cannot drift apart in anything except the queue under test.
foreach(variant IN ITEMS "mutex" "lockfree")
    if(variant STREQUAL "mutex")
        set(target edgeflow-gateway)
    else()
        set(target edgeflow-gateway-lockfree)
    endif()

    add_executable(${target} src/main.cpp)
    target_link_libraries(${target} PRIVATE
        edgeflow-core
        edgeflow-gateway-lib
        Boost::system
        Threads::Threads
    )
    if(variant STREQUAL "lockfree")
        target_compile_definitions(${target} PRIVATE EDGEFLOW_USE_LOCK_FREE_QUEUE)
    endif()
endforeach()
