#include <iceoryx_posh/capro/service_description.hpp>
#include <iceoryx_posh/popo/publisher_options.hpp>
#include <iceoryx_posh/popo/subscriber_options.hpp>
#include <iceoryx_posh/popo/untyped_publisher.hpp>
#include <iceoryx_posh/popo/untyped_subscriber.hpp>
#include <iceoryx_posh/iceoryx_posh_types.hpp>
#include <iceoryx_posh/runtime/posh_runtime.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <iox/string.hpp>

#include "benchmarks/common/data_generators.hpp"
#include "benchmarks/common/latency_tracker.hpp"
#include "benchmarks/common/metrics_printer.hpp"
#include "benchmarks/common/time.hpp"
#include "benchmarks/common/types.hpp"

namespace benchmarks {
namespace {

volatile std::sig_atomic_t g_stop = 0;

void SignalHandler(int) { g_stop = 1; }

iox::RuntimeName_t MakeRuntimeName(const std::string& name) {
  return iox::RuntimeName_t(iox::TruncateToCapacity, name.c_str());
}

iox::capro::IdString_t MakeId(const std::string& value) {
  return iox::capro::IdString_t(iox::TruncateToCapacity, value.c_str());
}

iox::capro::IdString_t MakeId(const char* value) {
  return iox::capro::IdString_t(iox::TruncateToCapacity, value);
}

struct ImuWireMessage {
  uint64_t sequence;
  Nanoseconds publish_ts;
  float accel[3];
  float gyro[3];
};

struct ImageWireHeader {
  uint64_t sequence;
  Nanoseconds publish_ts;
  uint16_t stream_id;
  uint32_t width;
  uint32_t height;
  uint32_t channels;
};

class IceoryxPublisher {
 public:
  explicit IceoryxPublisher(const BenchmarkConfig& config)
      : config_(config) {
    auto app_name = std::string("iox_pub_") + std::to_string(::getpid());
    iox::runtime::PoshRuntime::initRuntime(MakeRuntimeName(app_name));

    iox::popo::PublisherOptions options;
    options.historyCapacity = 4U;

    if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
      imu_publisher_ = std::make_unique<iox::popo::UntypedPublisher>(
          iox::capro::ServiceDescription{MakeId(config.imu_topic), MakeId("Benchmark"), MakeId("IMU")}, options);
    }

    if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
      image_publishers_.resize(kImageStreams);
      for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
        auto event = std::string("cam_") + std::to_string(stream);
        image_publishers_[stream] = std::make_unique<iox::popo::UntypedPublisher>(
            iox::capro::ServiceDescription{MakeId(config.image_topic), MakeId("Benchmark"), MakeId(event)}, options);
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

    while (running.load() && !g_stop) {
      const auto now = std::chrono::steady_clock::now();
      if (imu_publisher_ && now >= next_imu) {
        next_imu += imu_period;
        auto sample = imu_gen.NextSample();
        auto loanResult = imu_publisher_->loan(sizeof(ImuWireMessage), alignof(ImuWireMessage));
        if (!loanResult.has_error()) {
          auto* msg = static_cast<ImuWireMessage*>(loanResult.value());
          msg->sequence = sample.sequence;
          msg->publish_ts = sample.publish_ts;
          std::memcpy(msg->accel, sample.accel, sizeof(sample.accel));
          std::memcpy(msg->gyro, sample.gyro, sizeof(sample.gyro));
          imu_publisher_->publish(msg);
        }
      }

      if (!image_publishers_.empty()) {
        for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
          if (!image_publishers_[stream]) {
            continue;
          }
          if (now >= next_image[stream]) {
            next_image[stream] += image_period;
            auto sample = image_gen.NextSample(stream);
            const size_t payload_size = sizeof(ImageWireHeader) + sample.data.size();
            auto loanResult = image_publishers_[stream]->loan(payload_size, alignof(ImageWireHeader));
            if (!loanResult.has_error()) {
              auto* buffer = static_cast<uint8_t*>(loanResult.value());
              auto* header = reinterpret_cast<ImageWireHeader*>(buffer);
              header->sequence = sample.sequence;
              header->publish_ts = sample.publish_ts;
              header->stream_id = stream;
              header->width = sample.width;
              header->height = sample.height;
              header->channels = sample.channels;
              std::memcpy(buffer + sizeof(ImageWireHeader), sample.data.data(), sample.data.size());
              image_publishers_[stream]->publish(buffer);
            }
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

 private:
  BenchmarkConfig config_;
  std::unique_ptr<iox::popo::UntypedPublisher> imu_publisher_;
  std::vector<std::unique_ptr<iox::popo::UntypedPublisher>> image_publishers_;
};

class IceoryxSubscriber {
 public:
  IceoryxSubscriber(const BenchmarkConfig& config, LatencyTracker& tracker)
      : config_(config), tracker_(tracker) {
    auto app_name = std::string("iox_sub_") + std::to_string(::getpid());
    iox::runtime::PoshRuntime::initRuntime(MakeRuntimeName(app_name));

    iox::popo::SubscriberOptions options;
    options.queueCapacity = 64U;

    if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
      imu_subscriber_ = std::make_unique<iox::popo::UntypedSubscriber>(
          iox::capro::ServiceDescription{MakeId(config.imu_topic), MakeId("Benchmark"), MakeId("IMU")}, options);
    }

    if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
      image_subscribers_.resize(kImageStreams);
      for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
        auto event = std::string("cam_") + std::to_string(stream);
        image_subscribers_[stream] = std::make_unique<iox::popo::UntypedSubscriber>(
            iox::capro::ServiceDescription{MakeId(config.image_topic), MakeId("Benchmark"), MakeId(event)}, options);
      }
    }
  }

  void Run(std::atomic<bool>& running) {
    while (running.load() && !g_stop) {
      if (imu_subscriber_) {
        auto take = imu_subscriber_->take();
        while (!take.has_error()) {
          const auto* msg = static_cast<const ImuWireMessage*>(take.value());
          tracker_.AddSample(NowNs() - msg->publish_ts);
          imu_subscriber_->release(msg);
          take = imu_subscriber_->take();
        }
      }
      for (auto& sub : image_subscribers_) {
        if (!sub) {
          continue;
        }
        auto take = sub->take();
        while (!take.has_error()) {
          const auto* buffer = static_cast<const uint8_t*>(take.value());
          const auto* header = reinterpret_cast<const ImageWireHeader*>(buffer);
          tracker_.AddSample(NowNs() - header->publish_ts);
          sub->release(buffer);
          take = sub->take();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

 private:
  BenchmarkConfig config_;
  LatencyTracker& tracker_;
  std::unique_ptr<iox::popo::UntypedSubscriber> imu_subscriber_;
  std::vector<std::unique_ptr<iox::popo::UntypedSubscriber>> image_subscribers_;
};

BenchmarkConfig ParseArgs(int argc, char** argv) {
  BenchmarkConfig config;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto Next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + name);
      }
      return argv[++i];
    };
    if (arg == "--mode") {
      config.mode = ParseMode(Next(arg.c_str()));
    } else if (arg == "--role") {
      config.role = ParseRole(Next(arg.c_str()));
    } else if (arg == "--stream") {
      config.stream = ParseStreamType(Next(arg.c_str()));
    } else if (arg == "--imu-topic") {
      config.imu_topic = Next(arg.c_str());
    } else if (arg == "--image-topic") {
      config.image_topic = Next(arg.c_str());
    } else if (arg == "--duration") {
      config.duration_sec = std::stod(Next(arg.c_str()));
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: iceoryx_benchmark [--mode intra|inter] [--role mono|pub|sub] [--stream ...]\n";
      std::exit(0);
    }
  }
  return config;
}

void RunIntra(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  printer.Start();
  std::atomic<bool> running{true};
  IceoryxPublisher publisher(config);
  IceoryxSubscriber subscriber(config, tracker);
  std::thread pub_thread([&] { publisher.Run(running); });
  std::thread sub_thread([&] { subscriber.Run(running); });
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(config.duration_sec * 1000)));
  running = false;
  pub_thread.join();
  sub_thread.join();
  printer.Stop();
}

void RunPublisher(const BenchmarkConfig& config) {
  std::atomic<bool> running{true};
  IceoryxPublisher publisher(config);
  std::thread pub_thread([&] { publisher.Run(running); });
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(config.duration_sec * 1000)));
  running = false;
  pub_thread.join();
}

void RunSubscriber(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  printer.Start();
  std::atomic<bool> running{true};
  IceoryxSubscriber subscriber(config, tracker);
  std::thread sub_thread([&] { subscriber.Run(running); });
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(config.duration_sec * 1000)));
  running = false;
  sub_thread.join();
  printer.Stop();
}

}  // namespace
}  // namespace benchmarks

int main(int argc, char** argv) {
  using namespace benchmarks;
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
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
