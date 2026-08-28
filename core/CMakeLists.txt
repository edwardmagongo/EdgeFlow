add_library(edgeflow-core
    src/event.cpp
)

target_include_directories(edgeflow-core PUBLIC include)
target_link_libraries(edgeflow-core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
