add_library(edgeflow-gateway-lib
    src/config.cpp
)
target_include_directories(edgeflow-gateway-lib PUBLIC include)
target_link_libraries(edgeflow-gateway-lib PUBLIC
    edgeflow-core
    Boost::system
    Threads::Threads
)

add_executable(edgeflow-gateway src/main.cpp)
target_link_libraries(edgeflow-gateway PRIVATE
    edgeflow-core
    edgeflow-gateway-lib
    Boost::system
    Threads::Threads
)
