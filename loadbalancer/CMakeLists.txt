# 1. Set the minimum version of CMake required
cmake_minimum_required(VERSION 3.18)

# 2. Name your project and specify the language
project(LoadBalancer VERSION 1.0 LANGUAGES CXX)

# 3. Set the C++ standard (e.g., C++17 or C++20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. Tell CMake where your header files are located
include_directories(include)

# 5. List your source files
set(SOURCES 
    src/main.cpp
)

# 6. Create the executable from those source files
add_executable(loadbalancer ${SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(loadbalancer uring Threads::Threads)