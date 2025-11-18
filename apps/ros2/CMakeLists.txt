cmake_minimum_required(VERSION 3.16)
project(ros2_benchmark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(ament_cmake REQUIRED)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/benchmark_common.cmake)
benchmarks_add_common_library()

find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(rosidl_default_generators REQUIRED)

set(ros2_interfaces_target ${PROJECT_NAME}__interfaces)

rosidl_generate_interfaces(${ros2_interfaces_target}
  "msg/ImageSample.msg"
  LIBRARY_NAME ${PROJECT_NAME}
  SKIP_INSTALL
  SKIP_GROUP_MEMBERSHIP_CHECK
)

rosidl_get_typesupport_target(cpp_typesupport_target ${ros2_interfaces_target} "rosidl_typesupport_cpp")
add_executable(ros2_benchmark
    ros2_benchmark.cpp)

rosidl_target_interfaces(ros2_benchmark
  ${ros2_interfaces_target} "rosidl_typesupport_cpp")

target_link_libraries(ros2_benchmark
    benchmark_common
    Threads::Threads
    ${cpp_typesupport_target})

# Ensure generated type support libraries next to the binary are discoverable at runtime.
target_link_options(ros2_benchmark PRIVATE "-Wl,-rpath,$ORIGIN")

ament_target_dependencies(ros2_benchmark
    rclcpp
    sensor_msgs
    builtin_interfaces
    rosidl_default_runtime)

install(TARGETS ros2_benchmark
  DESTINATION lib/${PROJECT_NAME})
