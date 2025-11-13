#include "iox2/iceoryx2.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "benchmarks/common/data_generators.hpp"
#include "benchmarks/common/latency_tracker.hpp"
#include "benchmarks/common/metrics_printer.hpp"
#include "benchmarks/common/runtime.hpp"
#include "benchmarks/common/time.hpp"
#include "benchmarks/common/traffic_counter.hpp"
#include "benchmarks/common/types.hpp"

namespace benchmarks {
namespace {

struct ImuWireMessage {
  uint64_t sequence;
  Nanoseconds publish_ts;
  float accel[3];
  float gyro[3];
};

bool IsImuEnabled(StreamType stream) {
  return stream == StreamType::kImu || stream == StreamType::kBoth;
}

bool IsImageEnabled(StreamType stream) {
  return stream == StreamType::kImage || stream == StreamType::kBoth;
}

std::string SanitizeServiceComponent(std::string_view text) {
  std::string sanitized;
  sanitized.reserve(text.size());
  for (const char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) != 0 || ch == '_' || ch == '-') {
      sanitized.push_back(static_cast<char>(std::tolower(c)));
    } else {
      sanitized.push_back('_');
    }
  }
  if (sanitized.empty()) {
    sanitized = "default";
  }
  return sanitized;
}

iox2::ServiceName BuildServiceName(std::string_view topic,
                                   std::string_view category,
                                   std::optional<uint16_t> stream = std::nullopt) {
  std::string name = "benchmarks/";
  name += SanitizeServiceComponent(category);
  name += "/";
  name += SanitizeServiceComponent(topic);
  if (stream.has_value()) {
    name += "/";
    name += std::to_string(stream.value());
  }
  return iox2::ServiceName::create(name.c_str()).expect("valid iceoryx2 service name");
}

using NodeType = iox2::Node<iox2::ServiceType::Ipc>;
using ImuPublisher = iox2::Publisher<iox2::ServiceType::Ipc, ImuWireMessage, void>;
using ImagePublisher = iox2::Publisher<iox2::ServiceType::Ipc, ImageSample, void>;
using ImuSubscriber = iox2::Subscriber<iox2::ServiceType::Ipc, ImuWireMessage, void>;
using ImageSubscriber = iox2::Subscriber<iox2::ServiceType::Ipc, ImageSample, void>;

class IceoryxPublisher {
 public:
  IceoryxPublisher(const BenchmarkConfig& config, TrafficCounter* counter)
      : config_(config),
        traffic_(counter),
        node_(iox2::NodeBuilder().create<iox2::ServiceType::Ipc>().expect("create iceoryx2 node")) {
    if (IsImuEnabled(config_.stream)) {
      auto service_name = BuildServiceName(config_.imu_topic, "imu");
      auto service = node_.service_builder(service_name)
                         .publish_subscribe<ImuWireMessage>()
                         .open_or_create()
                         .expect("create imu service");
      imu_publisher_.emplace(service.publisher_builder().create().expect("create imu publisher"));
    }

    if (IsImageEnabled(config_.stream)) {
      image_publishers_.resize(kImageStreams);
      for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
        auto service_name = BuildServiceName(config_.image_topic, "image", stream);
        auto service = node_.service_builder(service_name)
                           .publish_subscribe<ImageSample>()
                           .open_or_create()
                           .expect("create image service");
        image_publishers_[stream].emplace(
            service.publisher_builder().create().expect("create image publisher"));
      }
    }
  }

  void Run(std::atomic<bool>& running) {
    ImuGenerator imu_gen;
    ImageGenerator image_gen;
    const auto imu_period = std::chrono::nanoseconds(1'000'000'000ULL / kImuRateHz);
    const auto image_period = std::chrono::nanoseconds(1'000'000'000ULL / kImageRatePerStreamHz);
    auto next_imu = std::chrono::steady_clock::now();
    std::array<std::chrono::steady_clock::time_point, kImageStreams> next_image;
    next_image.fill(std::chrono::steady_clock::now());

    while (running.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (imu_publisher_.has_value() && now >= next_imu) {
        next_imu += imu_period;
        const auto sample = imu_gen.NextSample();
        ImuWireMessage msg{};
        msg.sequence = sample.sequence;
        msg.publish_ts = sample.publish_ts;
        std::memcpy(msg.accel, sample.accel, sizeof(sample.accel));
        std::memcpy(msg.gyro, sample.gyro, sizeof(sample.gyro));
        const auto send_result = imu_publisher_->send_copy(msg);
        if (!send_result.has_error() && traffic_) {
          traffic_->IncrementImuPublished();
        }
      }

      for (uint16_t stream = 0; stream < image_publishers_.size(); ++stream) {
        auto& publisher = image_publishers_[stream];
        if (!publisher.has_value()) {
          continue;
        }
        if (now < next_image[stream]) {
          continue;
        }
        next_image[stream] += image_period;
        auto loan = publisher->loan_uninit();
        if (loan.has_error()) {
          continue;
        }
        auto sample_uninit = std::move(loan.value());
        auto& payload = sample_uninit.payload_mut();
        image_gen.FillSample(stream, &payload);
        auto ready = iox2::assume_init(std::move(sample_uninit));
        // std::cout << "seq: " << payload.sequence << " stream_id: " << payload.stream_id << " send ptr: "
        // << static_cast<void*>(&payload) << std::endl;
        auto send_result = iox2::send(std::move(ready));
        if (!send_result.has_error() && traffic_) {
          traffic_->IncrementImagePublished();
        }
      }

      std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
  }

 private:
  BenchmarkConfig config_;
  TrafficCounter* traffic_{nullptr};
  NodeType node_;
  std::optional<ImuPublisher> imu_publisher_;
  std::vector<std::optional<ImagePublisher>> image_publishers_;
};

class IceoryxSubscriber {
 public:
  IceoryxSubscriber(const BenchmarkConfig& config, LatencyTracker& tracker, TrafficCounter* counter)
      : config_(config),
        tracker_(tracker),
        traffic_(counter),
        node_(iox2::NodeBuilder().create<iox2::ServiceType::Ipc>().expect("create iceoryx2 node")) {
    if (IsImuEnabled(config_.stream)) {
      auto service_name = BuildServiceName(config_.imu_topic, "imu");
      auto service = node_.service_builder(service_name)
                         .publish_subscribe<ImuWireMessage>()
                         .open_or_create()
                         .expect("open imu service");
      imu_subscriber_.emplace(service.subscriber_builder().create().expect("create imu subscriber"));
    }

    if (IsImageEnabled(config_.stream)) {
      image_subscribers_.resize(kImageStreams);
      for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
        auto service_name = BuildServiceName(config_.image_topic, "image", stream);
        auto service = node_.service_builder(service_name)
                           .publish_subscribe<ImageSample>()
                           .open_or_create()
                           .expect("open image service");
        image_subscribers_[stream].emplace(service.subscriber_builder().create().expect("create image subscriber"));
      }
    }
  }

  void Run(std::atomic<bool>& running) {
    while (running.load()) {
      DrainImu();
      DrainImages();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

 private:
  void DrainImu() {
    if (!imu_subscriber_.has_value()) {
      return;
    }
    while (true) {
      auto take = imu_subscriber_->receive();
      if (take.has_error()) {
        break;
      }
      auto sample = std::move(take.value());
      if (!sample.has_value()) {
        break;
      }
      const auto& payload = sample->payload();
      tracker_.AddSample(NowNs() - payload.publish_ts);
      if (traffic_) {
        traffic_->IncrementImuReceived();
      }
    }
  }

  void DrainImages() {
    for (auto& subscriber : image_subscribers_) {
      if (!subscriber.has_value()) {
        continue;
      }
      while (true) {
        auto take = subscriber->receive();
        if (take.has_error()) {
          break;
        }
        auto sample = std::move(take.value());
        if (!sample.has_value()) {
          break;
        }
        const auto& payload = sample->payload();
        // std::cout << "seq: " << payload.sequence << " stream_id: " << payload.stream_id << " recv ptr: "
        // << static_cast<void*>(const_cast<ImageSample*>(&payload)) << std::endl;
        tracker_.AddSample(NowNs() - payload.publish_ts);
        if (traffic_) {
          traffic_->IncrementImageReceived();
        }
      }
    }
  }

  BenchmarkConfig config_;
  LatencyTracker& tracker_;
  TrafficCounter* traffic_{nullptr};
  NodeType node_;
  std::optional<ImuSubscriber> imu_subscriber_;
  std::vector<std::optional<ImageSubscriber>> image_subscribers_;
};

BenchmarkConfig ParseArgs(int argc, char** argv) {
  constexpr std::string_view kUsage =
      "Usage: iceoryx_benchmark [--role mono|pub|sub] [--stream imu|image|both]\n"
      "                         [--imu-topic name] [--image-topic base] [--duration seconds]";
  auto handler = [](std::string_view option, ArgParser& parser, BenchmarkConfig& config) -> bool {
    if (option == "--imu-topic") {
      config.imu_topic = parser.ConsumeValue(option);
      return true;
    }
    if (option == "--image-topic") {
      config.image_topic = parser.ConsumeValue(option);
      return true;
    }
    return false;
  };
  return ParseArguments(argc, argv, handler, kUsage);
}

void RunIntra(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  TrafficCounter traffic;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();
  std::atomic<bool> running{true};
  IceoryxPublisher publisher(config, &traffic);
  IceoryxSubscriber subscriber(config, tracker, &traffic);
  std::thread pub_thread([&] { publisher.Run(running); });
  std::thread sub_thread([&] { subscriber.Run(running); });
  WaitForShutdown(config.duration_sec);
  running = false;
  pub_thread.join();
  sub_thread.join();
  printer.Stop();
}

void RunPublisher(const BenchmarkConfig& config) {
  TrafficCounter traffic;
  MetricsPrinter printer;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();
  std::atomic<bool> running{true};
  IceoryxPublisher publisher(config, &traffic);
  std::thread pub_thread([&] { publisher.Run(running); });
  WaitForShutdown(config.duration_sec);
  running = false;
  pub_thread.join();
  printer.Stop();
}

void RunSubscriber(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  TrafficCounter traffic;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();
  std::atomic<bool> running{true};
  IceoryxSubscriber subscriber(config, tracker, &traffic);
  std::thread sub_thread([&] { subscriber.Run(running); });
  WaitForShutdown(config.duration_sec);
  running = false;
  sub_thread.join();
  printer.Stop();
}

}  // namespace
}  // namespace benchmarks

int main(int argc, char** argv) {
  using namespace benchmarks;
  InstallSignalHandlers();
  try {
    auto config = ParseArgs(argc, argv);
    if (config.mode == Mode::kIntra) {
      RunIntra(config);
    } else {
      if (config.role == Role::kPublisher) {
        RunPublisher(config);
      } else if (config.role == Role::kSubscriber) {
        RunSubscriber(config);
      } else {
        throw std::invalid_argument("mono role invalid for inter mode");
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << std::endl;
    return 1;
  }
  return 0;
}
