#include "benchmarks/common/traffic_counter.hpp"

namespace benchmarks {

void TrafficCounter::IncrementImuPublished(uint64_t count) {
  imu_published_.fetch_add(count, std::memory_order_relaxed);
}

void TrafficCounter::IncrementImuReceived(uint64_t count) {
  imu_received_.fetch_add(count, std::memory_order_relaxed);
}

void TrafficCounter::IncrementImagePublished(uint64_t count) {
  image_published_.fetch_add(count, std::memory_order_relaxed);
}

void TrafficCounter::IncrementImageReceived(uint64_t count) {
  image_received_.fetch_add(count, std::memory_order_relaxed);
}

TrafficSnapshot TrafficCounter::SnapshotAndReset() {
  TrafficSnapshot snapshot;
  snapshot.imu_published = imu_published_.exchange(0, std::memory_order_relaxed);
  snapshot.imu_received = imu_received_.exchange(0, std::memory_order_relaxed);
  snapshot.image_published = image_published_.exchange(0, std::memory_order_relaxed);
  snapshot.image_received = image_received_.exchange(0, std::memory_order_relaxed);
  return snapshot;
}

}  // namespace benchmarks
