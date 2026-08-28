add_library(edgeflow-simulator-lib
    src/config.cpp
    src/device_client.cpp
)
target_include_directories(edgeflow-simulator-lib PUBLIC include)
target_link_libraries(edgeflow-simulator-lib PUBLIC
    edgeflow-core
    Boost::system
    Threads::Threads
)

add_executable(edgeflow-simulator src/main.cpp)
target_link_libraries(edgeflow-simulator PRIVATE
    edgeflow-core
    edgeflow-simulator-lib
    Boost::system
    Threads::Threads
)
