#pragma once

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

  ImageSample NextSample(uint16_t stream_id);

 private:
  uint64_t sequence_{0};
  uint32_t width_;
  uint32_t height_;
  uint32_t channels_;
  std::vector<uint8_t> scratch_;
};

}  // namespace benchmarks
