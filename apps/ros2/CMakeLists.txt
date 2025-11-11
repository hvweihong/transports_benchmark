find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)

add_executable(ros2_benchmark
    ros2_benchmark.cpp)

target_link_libraries(ros2_benchmark
    PRIVATE
      benchmark_common
      rclcpp::rclcpp
      sensor_msgs::sensor_msgs
      std_msgs::std_msgs
      builtin_interfaces::builtin_interfaces)
