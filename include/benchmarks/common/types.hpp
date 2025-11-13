#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace benchmarks {

using Nanoseconds = uint64_t;

enum class StreamType { kImu, kImage, kBoth };

enum class Mode { kIntra, kInter };

enum class Role { kPublisher, kSubscriber, kMono };

struct BenchmarkConfig {
  Mode mode{Mode::kIntra};
  Role role{Role::kMono};
  StreamType stream{StreamType::kBoth};
  std::string imu_endpoint{"tcp://127.0.0.1:6000"};
  std::string image_endpoint{"tcp://127.0.0.1:6001"};
  std::string image_topic{"image"};
  std::string imu_topic{"imu"};
  double duration_sec{30.0};
  bool enable_logging{true};
};

constexpr uint32_t kImuRateHz = 200;
constexpr uint32_t kImageWidth = 1280;
constexpr uint32_t kImageHeight = 1080;
constexpr uint32_t kImageChannels = 3;
constexpr uint32_t kImageStride = kImageWidth * kImageChannels;
constexpr uint32_t kImageBytesPerFrame = kImageWidth * kImageHeight * kImageChannels;
constexpr uint32_t kImageStreams = 6;
constexpr uint32_t kImageRatePerStreamHz = 20;

struct alignas(16) ImuSample {
  uint64_t sequence{0};
  Nanoseconds publish_ts{0};
  float accel[3]{0.0F, 0.0F, 0.0F};
  float gyro[3]{0.0F, 0.0F, 0.0F};
};

struct alignas(16) ImageSample {
  uint64_t sequence{0};
  Nanoseconds publish_ts{0};
  uint16_t stream_id{0};
  uint32_t width{kImageWidth};
  uint32_t height{kImageHeight};
  uint32_t channels{kImageChannels};
  uint32_t payload_bytes{kImageBytesPerFrame};
  uint8_t data[kImageBytesPerFrame]{};
};

inline constexpr size_t ImagePayloadBytes(uint32_t width = kImageWidth,
                                          uint32_t height = kImageHeight,
                                          uint32_t channels = kImageChannels) {
  return static_cast<size_t>(width) * static_cast<size_t>(height) * channels;
}

StreamType ParseStreamType(std::string_view text);
Mode ParseMode(std::string_view text);
Role ParseRole(std::string_view text);

}  // namespace benchmarks
