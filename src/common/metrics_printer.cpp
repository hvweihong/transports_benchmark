#include "benchmarks/common/metrics_printer.hpp"

#include <iomanip>
#include <iostream>

namespace benchmarks {

MetricsPrinter::MetricsPrinter() = default;
MetricsPrinter::~MetricsPrinter() { Stop(); }

void MetricsPrinter::AttachTracker(LatencyTracker* tracker) { tracker_ = tracker; }

void MetricsPrinter::Start() {
  if (running_.exchange(true)) {
    return;
  }
  worker_ = std::thread(&MetricsPrinter::Run, this);
}

void MetricsPrinter::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void MetricsPrinter::Run() {
  using namespace std::chrono_literals;
  while (running_.load()) {
    std::this_thread::sleep_for(1s);
    auto load = monitor_.Sample();
    LatencyStats stats;
    if (tracker_) {
      stats = tracker_->SnapshotAndReset();
    }
    std::cout << std::fixed << std::setprecision(1)
              << "[metrics] cpu=" << load.cpu_percent
              << "% rss=" << load.rss_bytes / (1024.0 * 1024.0)
              << "MB vmem=" << load.virt_bytes / (1024.0 * 1024.0)
              << "MB";
    if (stats.samples > 0) {
      std::cout << std::setprecision(2)
                << " latency_us(avg=" << stats.avg_us
                << ",p95=" << stats.p95_us
                << ",p99=" << stats.p99_us
                << ",max=" << stats.max_us
                << ",samples=" << stats.samples << ")";
    }
    std::cout << std::endl;
  }
}

}  // namespace benchmarks
