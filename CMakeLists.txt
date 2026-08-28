cmake_minimum_required(VERSION 3.20)
project(edgeflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(EDGEFLOW_SANITIZE "off" CACHE STRING "Sanitizer to build with: off, address, thread")
set_property(CACHE EDGEFLOW_SANITIZE PROPERTY STRINGS off address thread)

if(EDGEFLOW_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(EDGEFLOW_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

include(cmake/Dependencies.cmake)

enable_testing()

add_subdirectory(core)
add_subdirectory(tests)
