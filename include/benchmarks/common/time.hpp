#pragma once

#include <chrono>

#include "benchmarks/common/types.hpp"

namespace benchmarks {

inline Nanoseconds NowNs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}  // namespace benchmarks
