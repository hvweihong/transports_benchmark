#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <vector>

namespace benchmarks {

struct LatencyStats {
  double avg_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
  double max_us{0.0};
  size_t samples{0};
};

class LatencyTracker {
 public:
  void AddSample(uint64_t latency_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back(static_cast<double>(latency_ns) / 1000.0);
  }

  LatencyStats SnapshotAndReset() {
    std::lock_guard<std::mutex> lock(mutex_);
    LatencyStats stats;
    if (samples_.empty()) {
      return stats;
    }
    const size_t n = samples_.size();
    stats.samples = n;
    stats.avg_us = std::accumulate(samples_.begin(), samples_.end(), 0.0) / static_cast<double>(n);
    std::sort(samples_.begin(), samples_.end());
    auto Percentile = [&](double p) {
      const double idx = p * (n - 1);
      const size_t lower = static_cast<size_t>(idx);
      const size_t upper = std::min(lower + 1, n - 1);
      const double fraction = idx - lower;
      return samples_[lower] * (1.0 - fraction) + samples_[upper] * fraction;
    };
    stats.p95_us = Percentile(0.95);
    stats.p99_us = Percentile(0.99);
    stats.max_us = samples_.back();
    samples_.clear();
    return stats;
  }

 private:
  std::vector<double> samples_;
  std::mutex mutex_;
};

}  // namespace benchmarks
