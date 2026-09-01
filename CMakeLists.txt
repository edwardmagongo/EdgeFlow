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
find_package(Boost 1.75 CONFIG REQUIRED COMPONENTS system)

# TLS for the HTTP sink. macOS ships LibreSSL as its system "openssl", which
# Boost.Asio's SSL layer does not fully track, so prefer Homebrew's OpenSSL 3
# when the caller has not already pinned one.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(candidate /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS ${candidate})
            set(OPENSSL_ROOT_DIR ${candidate})
            break()
        endif()
    endforeach()
endif()
find_package(OpenSSL REQUIRED)

enable_testing()

add_subdirectory(core)
add_subdirectory(gateway)
add_subdirectory(simulator)
add_subdirectory(benchmarks)
add_subdirectory(tests)
