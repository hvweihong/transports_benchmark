cmake_minimum_required(VERSION 3.16)
project(fastdds_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/benchmark_common.cmake)
benchmarks_add_common_library()

set(EPROSIMA_BUILD OFF CACHE BOOL "" FORCE)
set(EPROSIMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(THIRDPARTY FORCE CACHE STRING "" FORCE)
set(THIRDPARTY_fastcdr FORCE CACHE STRING "" FORCE)
set(THIRDPARTY_fastrtps FORCE CACHE STRING "" FORCE)
set(COMPILE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(COMPILE_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(FASTDDS_STATISTICS OFF CACHE BOOL "" FORCE)

# Build foonathan_memory first (Fast-DDS dependency)
set(FOONATHAN_MEMORY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FOONATHAN_MEMORY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FOONATHAN_MEMORY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/foonathan_memory
                 ${CMAKE_BINARY_DIR}/third_party/foonathan_memory)

# Add custom Find module path so Fast-DDS can find foonathan_memory target
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/Fast-DDS
                 ${CMAKE_BINARY_DIR}/third_party/Fast-DDS)

add_executable(fastdds_benchmark
    fastdds_benchmark.cpp)

target_link_libraries(fastdds_benchmark
    PRIVATE
      benchmark_common
      fastdds
      fastcdr)
