#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "benchmarks/common/latency_tracker.hpp"
#include "benchmarks/common/load_monitor.hpp"

namespace benchmarks {

class MetricsPrinter {
 public:
  MetricsPrinter();
  ~MetricsPrinter();

  void Start();
  void Stop();
  void AttachTracker(LatencyTracker* tracker);

 private:
  void Run();

  std::atomic<bool> running_{false};
  std::thread worker_;
  LoadMonitor monitor_;
  LatencyTracker* tracker_{nullptr};
};

}  // namespace benchmarks
