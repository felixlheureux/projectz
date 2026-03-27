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

# Mutually Exclusive Sanitizer Options
option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan are mutually exclusive. Enable only one.")
endif()

# Global sanitizers applied at the link and compilation boundary
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fsanitize=undefined)
    add_link_options(-fsanitize=address -fsanitize=undefined)
elseif(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# 1. Create the Core Library containing all C++23 Modules
add_library(loadbalancer_core STATIC)
target_sources(loadbalancer_core
    PUBLIC
    FILE_SET CXX_MODULES FILES
    src/FileDescriptor.cppm
    src/Socket.cppm
    src/Epoll.cppm
    src/DnsResolver.cppm
    src/ProxyServer.cppm
)

# Apply our production-ready compiler flags strictly to the CORE logic
target_compile_options(loadbalancer_core PRIVATE
    -Wall -Wextra -Wpedantic -Werror -pthread
    $<$<CONFIG:Release>:-O3 -flto>
    $<$<CONFIG:Debug>:-O0 -g>
)

# 2. Main Executable (Links to Core)
add_executable(loadbalancer src/main.cpp)
target_link_libraries(loadbalancer PRIVATE loadbalancer_core)
target_compile_options(loadbalancer PRIVATE -pthread)
target_link_options(loadbalancer PRIVATE -pthread)

# 3. Test Executable (Links to Core + GTest)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows/MSVC mapping; no-op on native Ubuntu but ensures cross-compatibility standards
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(proxy_tests 
    tests/FileDescriptorTest.cpp
    tests/DnsResolverTest.cpp
    tests/ProxyServerTest.cpp
)

target_link_libraries(proxy_tests 
    PRIVATE 
    loadbalancer_core 
    gtest_main
)

target_compile_options(proxy_tests PRIVATE -Wall -Wextra -Wpedantic -pthread)
target_link_options(proxy_tests PRIVATE -pthread)

include(GoogleTest)
gtest_discover_tests(proxy_tests)