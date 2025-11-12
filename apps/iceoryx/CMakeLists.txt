cmake_minimum_required(VERSION 3.22)
project(iceoryx_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/benchmark_common.cmake)
benchmarks_add_common_library()

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_CXX ON CACHE BOOL "" FORCE)
set(IOX2_FEATURE_DEV_PERMISSIONS OFF CACHE BOOL "" FORCE)
set(IOX2_FEATURE_LIBC_PLATFORM OFF CACHE BOOL "" FORCE)
set(IOX2_FEATURE_LOGGER_LOG OFF CACHE BOOL "" FORCE)
set(IOX2_FEATURE_LOGGER_TRACING OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/iceoryx2
                 ${CMAKE_BINARY_DIR}/third_party/iceoryx2)

add_executable(iceoryx_benchmark
    iceoryx_benchmark.cpp)

target_link_libraries(iceoryx_benchmark
    PRIVATE
      benchmark_common
      iceoryx2-cxx::static-lib-cxx)
