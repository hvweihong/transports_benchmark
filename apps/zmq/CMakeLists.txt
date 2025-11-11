add_executable(zmq_benchmark
    zmq_benchmark.cpp)

target_include_directories(zmq_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(zmq_benchmark
    PRIVATE
      benchmark_common
      libzmq)
