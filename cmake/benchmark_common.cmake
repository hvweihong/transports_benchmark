include_guard(GLOBAL)

set(BENCHMARKS_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

if(NOT DEFINED BENCHMARKS_DEBUG_FLAGS_SET)
  set(BENCHMARKS_DEBUG_FLAGS_SET ON CACHE INTERNAL "" FORCE)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  endif()
  add_compile_options(
      $<$<COMPILE_LANGUAGE:C>:-g>
      $<$<COMPILE_LANGUAGE:CXX>:-g>
      $<$<COMPILE_LANGUAGE:C>:-fno-omit-frame-pointer>
      $<$<COMPILE_LANGUAGE:CXX>:-fno-omit-frame-pointer>)
endif()

function(benchmarks_add_common_library)
  if(TARGET benchmark_common)
    return()
  endif()

  add_library(benchmark_common STATIC
      ${BENCHMARKS_ROOT_DIR}/src/common/types.cpp
      ${BENCHMARKS_ROOT_DIR}/src/common/data_generators.cpp
      ${BENCHMARKS_ROOT_DIR}/src/common/load_monitor.cpp
      ${BENCHMARKS_ROOT_DIR}/src/common/metrics_printer.cpp
      ${BENCHMARKS_ROOT_DIR}/src/common/runtime.cpp
      ${BENCHMARKS_ROOT_DIR}/src/common/traffic_counter.cpp)

  target_include_directories(benchmark_common
      PUBLIC
        ${BENCHMARKS_ROOT_DIR}/include)

  find_package(Threads REQUIRED)
  target_link_libraries(benchmark_common PUBLIC Threads::Threads)
endfunction()
