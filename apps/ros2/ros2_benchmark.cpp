#include <builtin_interfaces/msg/time.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>

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
#include <cstddef>
#include <tuple>
#include <utility>
#include <type_traits>
#include <vector>

#include "benchmarks/common/data_generators.hpp"
#include "benchmarks/common/latency_tracker.hpp"
#include "benchmarks/common/metrics_printer.hpp"
#include "benchmarks/common/runtime.hpp"
#include "benchmarks/common/time.hpp"
#include "benchmarks/common/traffic_counter.hpp"
#include "benchmarks/common/types.hpp"
#include "ros2_benchmark/msg/image_sample.hpp"

namespace benchmarks {
namespace {

using RosImageMsg = ros2_benchmark::msg::ImageSample;

inline ImageSample* ToImageSample(RosImageMsg* msg) {
  static_assert(std::is_standard_layout_v<RosImageMsg>, "ROS image message must be standard layout");
  static_assert(std::tuple_size<RosImageMsg::_data_type>::value == kImageBytesPerFrame,
                "ROS image message payload size mismatch");
  static_assert(alignof(RosImageMsg) == alignof(ImageSample), "ImageSample alignment mismatch");
  static_assert(sizeof(RosImageMsg) == sizeof(ImageSample), "ImageSample and ROS message must match in size");
  static_assert(offsetof(RosImageMsg, data) == offsetof(ImageSample, data),
                "ImageSample and ROS message layouts diverged");
  return reinterpret_cast<ImageSample*>(msg);
}

builtin_interfaces::msg::Time MakeStamp(Nanoseconds now) {
  builtin_interfaces::msg::Time stamp;
  stamp.sec = static_cast<int32_t>(now / 1'000'000'000ULL);
  stamp.nanosec = static_cast<uint32_t>(now % 1'000'000'000ULL);
  return stamp;
}

class Ros2ImuNode : public rclcpp::Node {
 public:
  Ros2ImuNode(const BenchmarkConfig& config,
              LatencyTracker* tracker,
              TrafficCounter* traffic,
              bool enable_intra)
      : rclcpp::Node("ros2_benchmark_imu", rclcpp::NodeOptions{}.use_intra_process_comms(enable_intra)),
        tracker_(tracker),
        traffic_(traffic) {
    if (config.role == Role::kPublisher || config.role == Role::kMono) {
      imu_pub_ = this->create_publisher<sensor_msgs::msg::Imu>(config.imu_topic, 10);
      auto period = std::chrono::nanoseconds(1'000'000'000ULL / kImuRateHz);
      imu_timer_ = this->create_wall_timer(period, std::bind(&Ros2ImuNode::PublishImu, this));
    }
    if (config.role == Role::kSubscriber || config.role == Role::kMono) {
      imu_sub_ = this->create_subscription<sensor_msgs::msg::Imu>(
          config.imu_topic, 50,
          std::bind(&Ros2ImuNode::OnImu, this, std::placeholders::_1));
    }
  }

  void Stop() {
    if (imu_timer_) {
      imu_timer_->cancel();
      imu_timer_.reset();
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

  LatencyTracker* tracker_{nullptr};
  TrafficCounter* traffic_{nullptr};
  ImuGenerator imu_gen_;

  rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr imu_pub_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr imu_sub_;
  rclcpp::TimerBase::SharedPtr imu_timer_;
};

class Ros2ImageNode : public rclcpp::Node {
 public:
  Ros2ImageNode(const BenchmarkConfig& config,
                LatencyTracker* tracker,
                TrafficCounter* traffic,
                bool enable_intra)
      : rclcpp::Node("ros2_benchmark_image"),
        tracker_(tracker),
        traffic_(traffic) {
    image_timers_.resize(kImageStreams);
    image_pubs_.resize(kImageStreams);
    image_subs_.resize(kImageStreams);
    for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
      const auto topic = config.image_topic + "_" + std::to_string(stream);
      if (config.role == Role::kPublisher || config.role == Role::kMono) {
        image_pubs_[stream] = this->create_publisher<RosImageMsg>(topic, rclcpp::QoS(5).reliable());
        auto callback = [this, stream]() { PublishImage(stream); };
        auto period = std::chrono::milliseconds(1000 / kImageRatePerStreamHz);
        image_timers_[stream] = this->create_wall_timer(period, callback);
      }
      if (config.role == Role::kSubscriber || config.role == Role::kMono) {
        image_subs_[stream] = this->create_subscription<RosImageMsg>(
            topic, rclcpp::QoS(5).reliable(),
            [this](RosImageMsg::ConstSharedPtr msg) { OnImage(msg); });
      }
    }
  }

  void Stop() {
    for (auto& timer : image_timers_) {
      if (timer) {
        timer->cancel();
        timer.reset();
      }
    }
  }

 private:
  void PublishImage(uint16_t stream_id) {
    if (stream_id >= image_pubs_.size() || !image_pubs_[stream_id]) {
      return;
    }
    if (!image_pubs_[stream_id]->can_loan_messages()) {
      RCLCPP_ERROR(get_logger(), "Loaned messages are not supported by the current RMW implementation");
      return;
    }
    auto loaned = image_pubs_[stream_id]->borrow_loaned_message();
    ImageSample* sample = ToImageSample(&loaned.get());
    image_gen_.FillSample(stream_id, sample);
    image_pubs_[stream_id]->publish(std::move(loaned));
    if (traffic_) {
      traffic_->IncrementImagePublished();
    }
  }

  void OnImage(const RosImageMsg::ConstSharedPtr& msg) {
    if (!tracker_) {
      return;
    }
    tracker_->AddSample(NowNs() - msg->publish_ts);
    if (traffic_) {
      traffic_->IncrementImageReceived();
    }
  }

  LatencyTracker* tracker_{nullptr};
  TrafficCounter* traffic_{nullptr};
  ImageGenerator image_gen_;

  std::vector<rclcpp::Publisher<RosImageMsg>::SharedPtr> image_pubs_;
  std::vector<rclcpp::Subscription<RosImageMsg>::SharedPtr> image_subs_;
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
  LatencyTracker* tracker_ptr = nullptr;
  if (config.role == Role::kSubscriber || config.role == Role::kMono) {
    tracker_ptr = &tracker;
    printer.AttachTracker(tracker_ptr);
  }
  TrafficCounter traffic;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();

  const bool enable_intra = config.mode == Mode::kIntra;
  std::shared_ptr<Ros2ImuNode> imu_node;
  std::shared_ptr<Ros2ImageNode> image_node;
  if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
    imu_node = std::make_shared<Ros2ImuNode>(config, tracker_ptr, &traffic, enable_intra);
  }
  if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
    image_node = std::make_shared<Ros2ImageNode>(config, tracker_ptr, &traffic, enable_intra);
  }

  rclcpp::executors::MultiThreadedExecutor executor;
  if (imu_node) {
    executor.add_node(imu_node);
  }
  if (image_node) {
    executor.add_node(image_node);
  }
  std::thread spin_thread([&]() { executor.spin(); });

  WaitForShutdown(config.duration_sec, [] { return !rclcpp::ok(); });
  if (imu_node) {
    imu_node->Stop();
  }
  if (image_node) {
    image_node->Stop();
  }
  executor.cancel();
  spin_thread.join();
  printer.Stop();
}

}  // namespace
}  // namespace benchmarks

int main(int argc, char** argv) {
  using namespace benchmarks;
  try {
    std::cerr << "[ros2_benchmark] starting" << std::endl;
    InstallSignalHandlers();
    auto config = ParseArgs(argc, argv);
    if (config.mode == Mode::kInter && config.role == Role::kMono) {
      throw std::invalid_argument("mono role is only supported for intra mode");
    }
    setenv("ROS_LOG_DIR", "./runs", 0);
    std::cerr << "[ros2_benchmark] init rclcpp" << std::endl;
    rclcpp::init(argc, argv);
    std::cerr << "[ros2_benchmark] run" << std::endl;
    RunRos2(config);
    std::cerr << "[ros2_benchmark] shutdown" << std::endl;
    rclcpp::shutdown();
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << std::endl;
    return 1;
  }
  return 0;
}
