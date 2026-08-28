add_library(edgeflow-simulator-config src/config.cpp)
target_include_directories(edgeflow-simulator-config PUBLIC include)
target_link_libraries(edgeflow-simulator-config PUBLIC edgeflow-core)

add_executable(edgeflow-simulator
    src/main.cpp
    src/device_client.cpp
)
target_include_directories(edgeflow-simulator PRIVATE src)
target_link_libraries(edgeflow-simulator PRIVATE
    edgeflow-core
    edgeflow-simulator-config
    Boost::system
    Threads::Threads
)
