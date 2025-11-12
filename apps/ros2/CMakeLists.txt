cmake_minimum_required(VERSION 3.16)
project(ros2_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(ament_cmake REQUIRED)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/benchmark_common.cmake)
benchmarks_add_common_library()

find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)

add_executable(ros2_benchmark
    ros2_benchmark.cpp)

target_link_libraries(ros2_benchmark
    benchmark_common
    Threads::Threads)

ament_target_dependencies(ros2_benchmark
    rclcpp
    sensor_msgs
    std_msgs
    builtin_interfaces)
