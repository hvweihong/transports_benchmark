source /opt/ros/humble/setup.bash
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export FASTRTPS_DEFAULT_PROFILES_FILE="$(pwd)/apps/ros2/configs/shm_fastdds.xml"
export RMW_FASTRTPS_USE_QOS_FROM_XML=1
export LD_LIBRARY_PATH="$(pwd)/build/ros2:$LD_LIBRARY_PATH"