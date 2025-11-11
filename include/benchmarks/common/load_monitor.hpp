#pragma once

#include <chrono>

namespace benchmarks {

struct LoadSample {
  double cpu_percent{0.0};
  double rss_bytes{0.0};
  double virt_bytes{0.0};
};

class LoadMonitor {
 public:
  LoadMonitor();
  LoadSample Sample();

 private:
  uint64_t last_total_jiffies_{0};
  uint64_t last_proc_jiffies_{0};
  std::chrono::steady_clock::time_point last_wall_;
};

}  // namespace benchmarks
