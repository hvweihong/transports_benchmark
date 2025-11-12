#include "benchmarks/common/data_generators.hpp"

#include <cmath>

#include "benchmarks/common/time.hpp"

namespace benchmarks {

ImuGenerator::ImuGenerator()
    : rng_(static_cast<uint32_t>(NowNs())) {}

ImuSample ImuGenerator::NextSample() {
  constexpr float kBaseAccel = 9.81F;
  const double t = static_cast<double>(sequence_) / static_cast<double>(kImuRateHz);
  ImuSample sample;
  sample.sequence = sequence_++;
  sample.publish_ts = NowNs();
  sample.accel[0] = kBaseAccel * std::sin(t) + noise_(rng_);
  sample.accel[1] = kBaseAccel * std::cos(t) + noise_(rng_);
  sample.accel[2] = kBaseAccel + noise_(rng_);
  sample.gyro[0] = 0.1F * std::sin(t * 0.5) + noise_(rng_);
  sample.gyro[1] = 0.1F * std::cos(t * 0.5) + noise_(rng_);
  sample.gyro[2] = 0.1F + noise_(rng_);
  return sample;
}

ImageGenerator::ImageGenerator(uint32_t width, uint32_t height, uint32_t channels)
    : width_(width), height_(height), channels_(channels), scratch_(ImagePayloadBytes(width, height, channels)) {}

ImageSample ImageGenerator::NextSample(uint16_t stream_id) {
  ImageSample img;
  img.sequence = sequence_++;
  img.publish_ts = NowNs();
  img.stream_id = stream_id;
  img.width = width_;
  img.height = height_;
  img.channels = channels_;
  img.data.resize(scratch_.size());

  // Generate simple color bars plus noise pattern
  // for (size_t row = 0; row < height_; ++row) {
  //   for (size_t col = 0; col < width_; ++col) {
  //     const size_t idx = (row * width_ + col) * channels_;
  //     const uint8_t base = static_cast<uint8_t>((col + row + stream_id * 23) % 255);
  //     img.data[idx + 0] = base;
  //     img.data[idx + 1] = static_cast<uint8_t>((base + 85) % 255);
  //     img.data[idx + 2] = static_cast<uint8_t>((base + 170) % 255);
  //   }
  // }

  return img;
}

}  // namespace benchmarks
