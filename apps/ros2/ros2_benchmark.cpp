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
#include <thread>
#include <vector>

#include "benchmarks/common/data_generators.hpp"
#include "benchmarks/common/latency_tracker.hpp"
#include "benchmarks/common/metrics_printer.hpp"
#include "benchmarks/common/time.hpp"
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
                    bool enable_intra)
      : rclcpp::Node("ros2_benchmark", rclcpp::NodeOptions{}.use_intra_process_comms(enable_intra)),
        config_(config),
        tracker_(tracker) {
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
        const auto topic = config.image_topic + "/" + std::to_string(stream);
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
  }

  void PublishImage(uint16_t stream_id) {
    if (stream_id >= image_pubs_.size() || !image_pubs_[stream_id]) {
      return;
    }
    auto sample = image_gen_.NextSample(stream_id);
    sensor_msgs::msg::Image msg;
    msg.header.frame_id = "camera_" + std::to_string(stream_id);
    msg.header.stamp = MakeStamp(sample.publish_ts);
    msg.height = sample.height;
    msg.width = sample.width;
    msg.encoding = "rgb8";
    msg.step = sample.width * sample.channels;
    msg.is_bigendian = 0;
    msg.data = std::move(sample.data);
    image_pubs_[stream_id]->publish(std::move(msg));
  }

  void OnImu(const sensor_msgs::msg::Imu::ConstSharedPtr& msg) {
    if (!tracker_) {
      return;
    }
    const Nanoseconds pub_ts = static_cast<Nanoseconds>(msg->header.stamp.sec) * 1'000'000'000ULL + msg->header.stamp.nanosec;
    tracker_->AddSample(NowNs() - pub_ts);
  }

  void OnImage(const sensor_msgs::msg::Image::ConstSharedPtr& msg) {
    if (!tracker_) {
      return;
    }
    const Nanoseconds pub_ts = static_cast<Nanoseconds>(msg->header.stamp.sec) * 1'000'000'000ULL + msg->header.stamp.nanosec;
    tracker_->AddSample(NowNs() - pub_ts);
  }

  BenchmarkConfig config_;
  LatencyTracker* tracker_{nullptr};
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
  BenchmarkConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
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
      std::cout << "Usage: ros2_benchmark [--mode intra|inter] [--role mono|pub|sub] [--stream ...]\n"
                << "                      [--imu-topic /imu] [--image-topic /image] [--duration sec]\n";
      std::exit(0);
    }
  }
  return config;
}

void RunRos2(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  if (config.role == Role::kSubscriber || config.role == Role::kMono) {
    printer.AttachTracker(&tracker);
  }
  printer.Start();

  const bool enable_intra = config.mode == Mode::kIntra;
  auto node = std::make_shared<Ros2BenchmarkNode>(config,
                                                  (config.role == Role::kSubscriber || config.role == Role::kMono)
                                                      ? &tracker
                                                      : nullptr,
                                                  enable_intra);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  std::thread spin_thread([&]() { executor.spin(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(config.duration_sec * 1000)));
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
