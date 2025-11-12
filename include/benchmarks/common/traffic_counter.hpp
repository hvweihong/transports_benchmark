#pragma once

#include <atomic>
#include <cstdint>

namespace benchmarks {

struct TrafficSnapshot {
  uint64_t imu_published{0};
  uint64_t imu_received{0};
  uint64_t image_published{0};
  uint64_t image_received{0};
};

class TrafficCounter {
 public:
  void IncrementImuPublished(uint64_t count = 1);
  void IncrementImuReceived(uint64_t count = 1);
  void IncrementImagePublished(uint64_t count = 1);
  void IncrementImageReceived(uint64_t count = 1);

  TrafficSnapshot SnapshotAndReset();

 private:
  std::atomic<uint64_t> imu_published_{0};
  std::atomic<uint64_t> imu_received_{0};
  std::atomic<uint64_t> image_published_{0};
  std::atomic<uint64_t> image_received_{0};
};

}  // namespace benchmarks
