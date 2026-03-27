cmake_minimum_required(VERSION 3.28)

# Force Clang 18+ because GCC 13 lacks the P1689R5 format required for Module scanning
set(CMAKE_CXX_COMPILER "clang++")

# Export exactly how files are compiled for IntelliSense (`compile_commands.json`)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(layer4_loadbalancer VERSION 1.0.0 LANGUAGES CXX)

# Enforce C++23 strictly
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add our executable
add_executable(loadbalancer src/main.cpp)

# Define our C++23 Modules targeting our executable
target_sources(loadbalancer
    PUBLIC
    FILE_SET CXX_MODULES FILES
    src/FileDescriptor.cppm
    src/Socket.cppm
    src/Epoll.cppm
    src/DnsResolver.cppm
    src/ProxyServer.cppm
)

# Apply our production-ready, no-compromise compiler flags
target_compile_options(loadbalancer PRIVATE
    -Wall -Wextra -Wpedantic -Werror -pthread
    $<$<CONFIG:Release>:-O3 -flto>
    $<$<CONFIG:Debug>:-O0 -g>
)

target_link_options(loadbalancer PRIVATE
    -pthread
)