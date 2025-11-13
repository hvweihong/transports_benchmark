#include "benchmarks/common/data_generators.hpp"

#include <cmath>
#include <stdexcept>
#include <thread>

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
    : width_(width), height_(height), channels_(channels),
      pool_(kPoolSize),
      in_use_(kPoolSize) {
  for (auto& flag : in_use_) {
    flag.store(false);
  }
}

const ImageSample* ImageGenerator::NextSample(uint16_t stream_id) {
  const uint32_t payload = static_cast<uint32_t>(ImagePayloadBytes(width_, height_, channels_));
  if (payload > kImageBytesPerFrame) {
    throw std::runtime_error("ImageGenerator payload exceeds static buffer size");
  }

  ImageSample* buffer = nullptr;
  while (!buffer) {
    for (size_t idx = 0; idx < pool_.size(); ++idx) {
      bool expected = false;
      if (in_use_[idx].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        buffer = &pool_[idx];
        break;
      }
    }
    if (!buffer) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }

  buffer->sequence = sequence_++;
  buffer->publish_ts = NowNs();
  buffer->stream_id = stream_id;
  buffer->width = width_;
  buffer->height = height_;
  buffer->channels = channels_;
  buffer->payload_bytes = payload;

  // const uint32_t stride = width_ * channels_;
  // for (uint32_t row = 0; row < height_; ++row) {
  //   for (uint32_t col = 0; col < width_; ++col) {
  //     const size_t idx = static_cast<size_t>(row * stride + col * channels_);
  //     const uint8_t base = static_cast<uint8_t>((col + row + stream_id * 17) & 0xFF);
  //     buffer->data[idx + 0] = base;
  //     if (channels_ > 1) {
  //       buffer->data[idx + 1] = static_cast<uint8_t>((base + 85) & 0xFF);
  //     }
  //     if (channels_ > 2) {
  //       buffer->data[idx + 2] = static_cast<uint8_t>((base + 170) & 0xFF);
  //     }
  //   }
  // }

  return buffer;
}

void ImageGenerator::ReleaseSample(const ImageSample* sample) {
  if (!sample) {
    return;
  }
  const ptrdiff_t idx = sample - pool_.data();
  if (idx < 0 || static_cast<size_t>(idx) >= pool_.size()) {
    return;
  }
  in_use_[idx].store(false, std::memory_order_release);
}

}  // namespace benchmarks
