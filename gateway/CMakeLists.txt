add_library(edgeflow-gateway-config src/config.cpp)
target_include_directories(edgeflow-gateway-config PUBLIC include)
target_link_libraries(edgeflow-gateway-config PUBLIC edgeflow-core)

add_executable(edgeflow-gateway
    src/main.cpp
    src/server.cpp
    src/connection.cpp
)
target_include_directories(edgeflow-gateway PRIVATE src)
target_link_libraries(edgeflow-gateway PRIVATE
    edgeflow-core
    edgeflow-gateway-config
    Boost::system
    Threads::Threads
)
