add_executable(iceoryx_benchmark
    iceoryx_benchmark.cpp)

target_link_libraries(iceoryx_benchmark
    PRIVATE
      benchmark_common
      iceoryx_posh::iceoryx_posh
      iceoryx_hoofs::iceoryx_hoofs)
