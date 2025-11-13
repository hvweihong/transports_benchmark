#include <builtin_interfaces/msg/time.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

builtin_interfaces::msg::Time MakeStamp(Nanoseconds now) {
  builtin_interfaces::msg::Time stamp;
  stamp.sec = static_cast<int32_t>(now / 1'000'000'000ULL);
  stamp.nanosec = static_cast<uint32_t>(now % 1'000'000'000ULL);
  return stamp;
}

class Ros2BenchmarkNode : public rclcpp::Node {
 public:
  Ros2BenchmarkNode(const BenchmarkConfig& config,
                    LatencyTracker* tracker,
                    TrafficCounter* traffic,
                    bool enable_intra)
      : rclcpp::Node("ros2_benchmark", rclcpp::NodeOptions{}.use_intra_process_comms(enable_intra)),
        config_(config),
        tracker_(tracker),
        traffic_(traffic) {
    if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
      if (config.role == Role::kPublisher || config.role == Role::kMono) {
        imu_pub_ = this->create_publisher<sensor_msgs::msg::Imu>(config.imu_topic, 10);
        auto period = std::chrono::nanoseconds(1'000'000'000ULL / kImuRateHz);
        imu_timer_ = this->create_wall_timer(period, std::bind(&Ros2BenchmarkNode::PublishImu, this));
      }
      if (config.role == Role::kSubscriber || config.role == Role::kMono) {
        imu_sub_ = this->create_subscription<sensor_msgs::msg::Imu>(
            config.imu_topic, 50,
            std::bind(&Ros2BenchmarkNode::OnImu, this, std::placeholders::_1));
      }
    }

    if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
      image_timers_.resize(kImageStreams);
      image_pubs_.resize(kImageStreams);
      image_subs_.resize(kImageStreams);
      for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
        const auto topic = config.image_topic + "_" + std::to_string(stream);
        if (config.role == Role::kPublisher || config.role == Role::kMono) {
          image_pubs_[stream] = this->create_publisher<sensor_msgs::msg::Image>(topic, rclcpp::QoS(5).reliable());
          auto callback = [this, stream]() { PublishImage(stream); };
          auto period = std::chrono::milliseconds(1000 / kImageRatePerStreamHz);
          image_timers_[stream] = this->create_wall_timer(period, callback);
        }
        if (config.role == Role::kSubscriber || config.role == Role::kMono) {
          image_subs_[stream] = this->create_subscription<sensor_msgs::msg::Image>(
              topic, rclcpp::QoS(5).reliable(),
              [this](sensor_msgs::msg::Image::ConstSharedPtr msg) { OnImage(msg); });
        }
      }
    }
  }

  void Stop() {
    if (imu_timer_) {
      imu_timer_->cancel();
      imu_timer_.reset();
    }
    for (auto& timer : image_timers_) {
      if (timer) {
        timer->cancel();
        timer.reset();
      }
    }
  }

 private:
  void PublishImu() {
    if (!imu_pub_) {
      return;
    }
    auto sample = imu_gen_.NextSample();
    sensor_msgs::msg::Imu msg;
    msg.header.frame_id = "imu";
    msg.header.stamp = MakeStamp(sample.publish_ts);
    msg.angular_velocity.x = sample.gyro[0];
    msg.angular_velocity.y = sample.gyro[1];
    msg.angular_velocity.z = sample.gyro[2];
    msg.linear_acceleration.x = sample.accel[0];
    msg.linear_acceleration.y = sample.accel[1];
    msg.linear_acceleration.z = sample.accel[2];
    imu_pub_->publish(std::move(msg));
    if (traffic_) {
      traffic_->IncrementImuPublished();
    }
  }

  void PublishImage(uint16_t stream_id) {
    if (stream_id >= image_pubs_.size() || !image_pubs_[stream_id]) {
      return;
    }
    const ImageSample* sample = image_gen_.NextSample(stream_id);
    sensor_msgs::msg::Image msg;
    msg.header.frame_id = "camera_" + std::to_string(stream_id);
    msg.header.stamp = MakeStamp(sample->publish_ts);
    msg.height = sample->height;
    msg.width = sample->width;
    msg.encoding = "rgb8";
    msg.step = sample->width * sample->channels;
    msg.is_bigendian = 0;
    msg.data.assign(sample->data, sample->data + sample->payload_bytes);
    image_pubs_[stream_id]->publish(std::move(msg));
    image_gen_.ReleaseSample(sample);
    if (traffic_) {
      traffic_->IncrementImagePublished();
    }
  }

  void OnImu(const sensor_msgs::msg::Imu::ConstSharedPtr& msg) {
    if (!tracker_) {
      return;
    }
    const Nanoseconds pub_ts = static_cast<Nanoseconds>(msg->header.stamp.sec) * 1'000'000'000ULL + msg->header.stamp.nanosec;
    tracker_->AddSample(NowNs() - pub_ts);
    if (traffic_) {
      traffic_->IncrementImuReceived();
    }
  }

  void OnImage(const sensor_msgs::msg::Image::ConstSharedPtr& msg) {
    if (!tracker_) {
      return;
    }
    const Nanoseconds pub_ts = static_cast<Nanoseconds>(msg->header.stamp.sec) * 1'000'000'000ULL + msg->header.stamp.nanosec;
    tracker_->AddSample(NowNs() - pub_ts);
    if (traffic_) {
      traffic_->IncrementImageReceived();
    }
  }

  BenchmarkConfig config_;
  LatencyTracker* tracker_{nullptr};
  TrafficCounter* traffic_{nullptr};
  ImuGenerator imu_gen_;
  ImageGenerator image_gen_;

  rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr imu_pub_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr imu_sub_;
  rclcpp::TimerBase::SharedPtr imu_timer_;

  std::vector<rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr> image_pubs_;
  std::vector<rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr> image_subs_;
  std::vector<rclcpp::TimerBase::SharedPtr> image_timers_;
};

BenchmarkConfig ParseArgs(int argc, char** argv) {
  constexpr std::string_view kUsage =
      "Usage: ros2_benchmark [--role mono|pub|sub] [--stream imu|image|both]\n"
      "                      [--imu-topic /imu] [--image-topic /image] [--duration sec]";
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

void RunRos2(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  if (config.role == Role::kSubscriber || config.role == Role::kMono) {
    printer.AttachTracker(&tracker);
  }
  TrafficCounter traffic;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();

  const bool enable_intra = config.mode == Mode::kIntra;
  auto node = std::make_shared<Ros2BenchmarkNode>(config,
                                                  (config.role == Role::kSubscriber || config.role == Role::kMono)
                                                      ? &tracker
                                                      : nullptr,
                                                  &traffic,
                                                  enable_intra);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  std::thread spin_thread([&]() { executor.spin(); });

  WaitForShutdown(config.duration_sec, [] { return !rclcpp::ok(); });
  node->Stop();
  executor.cancel();
  spin_thread.join();
  printer.Stop();
}

}  // namespace
}  // namespace benchmarks

int main(int argc, char** argv) {
  using namespace benchmarks;
  try {
    InstallSignalHandlers();
    auto config = ParseArgs(argc, argv);
    if (config.mode == Mode::kInter && config.role == Role::kMono) {
      throw std::invalid_argument("mono role is only supported for intra mode");
    }
    rclcpp::init(argc, argv);
    RunRos2(config);
    rclcpp::shutdown();
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << std::endl;
    return 1;
  }
  return 0;
}
