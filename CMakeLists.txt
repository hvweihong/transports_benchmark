cmake_minimum_required(VERSION 3.16)
project(multi_transport_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_ZMQ "Build ZeroMQ benchmark" ON)
option(BUILD_ICEORYX "Build iceoryx benchmark" ON)
option(BUILD_FASTDDS "Build Fast DDS benchmark" ON)
option(BUILD_ROS2 "Build ROS 2 benchmark" ON)

# 启用 ccache 支持（仅当系统中存在 ccache）
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

add_library(benchmark_common STATIC
    src/common/types.cpp
    src/common/data_generators.cpp
    src/common/load_monitor.cpp
    src/common/metrics_printer.cpp)

target_include_directories(benchmark_common
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(benchmark_common PUBLIC Threads::Threads)

if(BUILD_ZMQ)
  set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/libzmq)
  add_subdirectory(apps/zmq)
endif()

if(BUILD_ICEORYX)
  set(IOX_PLATFORM_FEATURE_ACL OFF CACHE BOOL "" FORCE)
  set(BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(EXAMPLES OFF CACHE BOOL "" FORCE)
  set(INTROSPECTION OFF CACHE BOOL "" FORCE)
  set(BINDING_C OFF CACHE BOOL "" FORCE)
  set(TOML_CONFIG OFF CACHE BOOL "" FORCE)
  set(DOWNLOAD_TOML_LIB OFF CACHE BOOL "" FORCE)
  set(BUILD_DOC OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/iceoryx/iceoryx_meta)
  add_subdirectory(apps/iceoryx)
endif()

if(BUILD_FASTDDS)

endif()

if(BUILD_ROS2)
  add_subdirectory(apps/ros2)
endif()
