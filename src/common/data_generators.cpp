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

  FillSample(stream_id, buffer);
  return buffer;
}

void ImageGenerator::FillSample(uint16_t stream_id, ImageSample* sample) {
  if (!sample) {
    throw std::invalid_argument("ImageGenerator::FillSample sample is null");
  }
  const uint32_t payload = static_cast<uint32_t>(ImagePayloadBytes(width_, height_, channels_));
  if (payload > kImageBytesPerFrame) {
    throw std::runtime_error("ImageGenerator payload exceeds static buffer size");
  }
  sample->sequence = sequence_++;
  sample->publish_ts = NowNs();
  sample->stream_id = stream_id;
  sample->width = width_;
  sample->height = height_;
  sample->channels = channels_;
  sample->payload_bytes = payload;
  // Optional pattern generation (disabled to save time)
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
