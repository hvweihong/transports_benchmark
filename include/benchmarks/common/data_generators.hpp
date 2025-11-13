#pragma once

#include <atomic>
#include <cstddef>
#include <random>
#include <vector>

#include "benchmarks/common/types.hpp"

namespace benchmarks {

class ImuGenerator {
 public:
  ImuGenerator();
  ImuSample NextSample();

 private:
  uint64_t sequence_{0};
  std::mt19937 rng_;
  std::normal_distribution<float> noise_{0.0F, 0.02F};
};

class ImageGenerator {
 public:
  ImageGenerator(uint32_t width = kImageWidth,
                 uint32_t height = kImageHeight,
                 uint32_t channels = kImageChannels);

  const ImageSample* NextSample(uint16_t stream_id);
  void FillSample(uint16_t stream_id, ImageSample* sample);
  void ReleaseSample(const ImageSample* sample);

 private:
  static constexpr size_t kPoolSize = 8;

  uint64_t sequence_{0};
  uint32_t width_;
  uint32_t height_;
  uint32_t channels_;
  std::vector<ImageSample> pool_;
  std::vector<std::atomic<bool>> in_use_;
};

}  // namespace benchmarks
