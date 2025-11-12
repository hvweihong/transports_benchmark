#include "benchmarks/common/metrics_printer.hpp"

#include <iomanip>
#include <iostream>

namespace benchmarks {

MetricsPrinter::MetricsPrinter() = default;
MetricsPrinter::~MetricsPrinter() { Stop(); }

void MetricsPrinter::AttachTracker(LatencyTracker* tracker) { tracker_ = tracker; }
void MetricsPrinter::AttachTrafficCounter(TrafficCounter* counter) { traffic_ = counter; }

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
  constexpr auto kInterval = 1s;
  const double interval_sec = std::chrono::duration<double>(kInterval).count();
  while (running_.load()) {
    std::this_thread::sleep_for(kInterval);
    auto load = monitor_.Sample();
    LatencyStats stats;
    if (tracker_) {
      stats = tracker_->SnapshotAndReset();
    }
    TrafficSnapshot traffic_stats;
    const bool has_traffic = traffic_ != nullptr;
    if (has_traffic) {
      traffic_stats = traffic_->SnapshotAndReset();
    }
    std::cout << std::fixed << std::setprecision(1)
              << "[metrics] cpu=" << load.cpu_percent
              << "% rss=" << load.rss_bytes / (1024.0 * 1024.0)
              << "MB vmem=" << load.virt_bytes / (1024.0 * 1024.0)
              << "MB";
    if (stats.samples > 0) {
      std::cout << std::setprecision(2)
                << " latency_us(avg=" << stats.avg_us
                << ",max=" << stats.max_us << ")";
    }
    if (has_traffic) {
      auto Rate = [&](uint64_t count) {
        return static_cast<double>(count) / interval_sec;
      };
      std::cout << std::setprecision(2)
                << " traffic(imu_pub=" << Rate(traffic_stats.imu_published)
                << "hz,imu_sub=" << Rate(traffic_stats.imu_received)
                << "hz,img_pub=" << Rate(traffic_stats.image_published)
                << "hz,img_sub=" << Rate(traffic_stats.image_received)
                << "hz)";
    }
    std::cout << std::endl;
  }
}

}  // namespace benchmarks
