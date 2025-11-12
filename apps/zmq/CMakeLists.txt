cmake_minimum_required(VERSION 3.16)
project(zmq_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/benchmark_common.cmake)
benchmarks_add_common_library()

set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libzmq
                 ${CMAKE_BINARY_DIR}/third_party/libzmq)

add_executable(zmq_benchmark
    zmq_benchmark.cpp)

target_include_directories(zmq_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(zmq_benchmark
    PRIVATE
      benchmark_common
      libzmq)
