#include <zmq.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

struct ImuWireMessage {
  uint64_t sequence;
  Nanoseconds publish_ts;
  float accel[3];
  float gyro[3];
};

struct ZmqImageContext {
  ImageGenerator* generator;
  const ImageSample* sample;
};

void ZmqFreeImageMessage(void*, void* hint) {
  auto* ctx = static_cast<ZmqImageContext*>(hint);
  ctx->generator->ReleaseSample(ctx->sample);
  delete ctx;
}

void* CreateSocket(void* context, int type, const std::string& endpoint, bool bind_socket) {
  void* socket = zmq_socket(context, type);
  if (!socket) {
    throw std::runtime_error("failed to create socket");
  }
  const int hwm = 10;
  zmq_setsockopt(socket, ZMQ_SNDHWM, &hwm, sizeof(hwm));
  zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
  const int linger = 0;
  zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
  int rc = bind_socket ? zmq_bind(socket, endpoint.c_str()) : zmq_connect(socket, endpoint.c_str());
  if (rc != 0) {
    zmq_close(socket);
    throw std::runtime_error("failed to bind/connect to " + endpoint);
  }
  return socket;
}

void CloseSocket(void* socket) {
  if (socket) {
    zmq_close(socket);
  }
}

void PublisherLoop(const BenchmarkConfig& config,
                   std::atomic<bool>& running,
                   TrafficCounter* traffic,
                   bool intra_process,
                   void* shared_context = nullptr) {
  void* context = shared_context ? shared_context : zmq_ctx_new();
  const bool owns_context = shared_context == nullptr;
  if (!context) {
    throw std::runtime_error("failed to create context");
  }
  const std::string imu_endpoint = intra_process ? "inproc://imu" : config.imu_endpoint;
  const std::string image_endpoint = intra_process ? "inproc://image" : config.image_endpoint;

  void* imu_socket = nullptr;
  void* image_socket = nullptr;

  if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
    imu_socket = CreateSocket(context, ZMQ_PUSH, imu_endpoint, true);
  }
  if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
    image_socket = CreateSocket(context, ZMQ_PUSH, image_endpoint, true);
  }

  ImuGenerator imu_gen;
  ImageGenerator image_gen;

  const auto imu_period = std::chrono::nanoseconds(1'000'000'000ULL / kImuRateHz);
  std::array<std::chrono::steady_clock::time_point, kImageStreams> next_image{};
  const auto start = std::chrono::steady_clock::now();
  auto next_imu = start;
  for (uint16_t i = 0; i < kImageStreams; ++i) {
    next_image[i] = start;
  }

  while (running.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (imu_socket && now >= next_imu) {
      next_imu += imu_period;
      const auto sample = imu_gen.NextSample();
      ImuWireMessage wire{sample.sequence, sample.publish_ts,
                          {sample.accel[0], sample.accel[1], sample.accel[2]},
                          {sample.gyro[0], sample.gyro[1], sample.gyro[2]}};
      int rc = zmq_send(imu_socket, &wire, sizeof(wire), ZMQ_DONTWAIT);
      if (rc == -1 && errno == EAGAIN) {
        // drop sample when queue is full
      }
      if (traffic) {
        traffic->IncrementImuPublished();
      }
    }

    if (image_socket) {
      for (uint16_t stream_id = 0; stream_id < kImageStreams; ++stream_id) {
        const auto image_period = std::chrono::nanoseconds(1'000'000'000ULL / kImageRatePerStreamHz);
        if (now >= next_image[stream_id]) {
          next_image[stream_id] += image_period;
          const ImageSample* sample = image_gen.NextSample(stream_id);
          zmq_msg_t msg;
          zmq_msg_init_data(&msg,
                            const_cast<ImageSample*>(sample),
                            sizeof(ImageSample),
                            &ZmqFreeImageMessage,
                            new ZmqImageContext{&image_gen, sample});
          int rc = zmq_msg_send(&msg, image_socket, ZMQ_DONTWAIT);
          // std::cout << "seq: " << sample->sequence << " stream_id: " << sample->stream_id << " send ptr: "
          // << static_cast<void*>(const_cast<ImageSample*>(sample)) << std::endl;
          if (rc == -1) {
            if (errno == EAGAIN) {
              zmq_msg_close(&msg);
              continue;  // drop frame when queue is full
            }
            zmq_msg_close(&msg);
            break;
          }
          zmq_msg_close(&msg);
          if (traffic) {
            traffic->IncrementImagePublished();
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(1000));
  }

  CloseSocket(imu_socket);
  CloseSocket(image_socket);
  if (owns_context) {
    zmq_ctx_term(context);
  }
}

void SubscriberLoop(const BenchmarkConfig& config,
                    LatencyTracker& tracker,
                    TrafficCounter* traffic,
                    std::atomic<bool>& running,
                    bool intra_process,
                    void* shared_context = nullptr) {
  void* context = shared_context ? shared_context : zmq_ctx_new();
  const bool owns_context = shared_context == nullptr;
  if (!context) {
    throw std::runtime_error("failed to create context");
  }
  const std::string imu_endpoint = intra_process ? "inproc://imu" : config.imu_endpoint;
  const std::string image_endpoint = intra_process ? "inproc://image" : config.image_endpoint;

  void* imu_socket = nullptr;
  void* image_socket = nullptr;

  if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
    imu_socket = CreateSocket(context, ZMQ_PULL, imu_endpoint, false);
  }
  if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
    image_socket = CreateSocket(context, ZMQ_PULL, image_endpoint, false);
  }

  while (running.load()) {
    zmq_pollitem_t items[2]{};
    int item_count = 0;
    int imu_index = -1;
    int image_index = -1;
    if (imu_socket) {
      imu_index = item_count;
      items[item_count++] = {imu_socket, 0, ZMQ_POLLIN, 0};
    }
    if (image_socket) {
      image_index = item_count;
      items[item_count++] = {image_socket, 0, ZMQ_POLLIN, 0};
    }
    if (item_count == 0) {
      break;
    }
    zmq_poll(items, item_count, 100);

    if (imu_index >= 0 && (items[imu_index].revents & ZMQ_POLLIN)) {
      ImuWireMessage wire{};
      const int bytes = zmq_recv(imu_socket, &wire, sizeof(wire), ZMQ_DONTWAIT);
      if (bytes == sizeof(wire)) {
        tracker.AddSample(NowNs() - wire.publish_ts);
        if (traffic) {
          traffic->IncrementImuReceived();
        }
      }
    }
    if (image_index >= 0 && (items[image_index].revents & ZMQ_POLLIN)) {
      zmq_msg_t msg;
      zmq_msg_init(&msg);
      const int bytes = zmq_msg_recv(&msg, image_socket, ZMQ_DONTWAIT);
      if (bytes == static_cast<int>(sizeof(ImageSample))) {
        const auto* sample = static_cast<const ImageSample*>(zmq_msg_data(&msg));
        // std::cout << "seq: " << sample->sequence << " stream_id: " << sample->stream_id << " recv ptr: "
        // << static_cast<void*>(const_cast<ImageSample*>(sample)) << std::endl;
        tracker.AddSample(NowNs() - sample->publish_ts);
        if (traffic) {
          traffic->IncrementImageReceived();
        }
      }
      zmq_msg_close(&msg);
    }
  }

  CloseSocket(imu_socket);
  CloseSocket(image_socket);
  if (owns_context) {
    zmq_ctx_term(context);
  }
}

BenchmarkConfig ParseArgs(int argc, char** argv) {
  constexpr std::string_view kUsage =
      "Usage: zmq_benchmark [--role mono|pub|sub] [--stream imu|image|both]\n"
      "                      [--imu-endpoint tcp://host:port] [--image-endpoint tcp://host:port]\n"
      "                      [--duration seconds]";
  auto handler = [](std::string_view option, ArgParser& parser, BenchmarkConfig& config) -> bool {
    if (option == "--imu-endpoint") {
      config.imu_endpoint = parser.ConsumeValue(option);
      return true;
    }
    if (option == "--image-endpoint") {
      config.image_endpoint = parser.ConsumeValue(option);
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
  void* context = zmq_ctx_new();
  if (!context) {
    throw std::runtime_error("failed to create context");
  }
  std::thread pub([&] { PublisherLoop(config, running, &traffic, true, context); });
  std::thread sub([&] { SubscriberLoop(config, tracker, &traffic, running, true, context); });
  WaitForShutdown(config.duration_sec);
  running = false;
  pub.join();
  sub.join();
  printer.Stop();
  zmq_ctx_term(context);
}

void RunInterPublisher(const BenchmarkConfig& config) {
  TrafficCounter traffic;
  MetricsPrinter printer;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();
  std::atomic<bool> running{true};
  std::thread pub([&] { PublisherLoop(config, running, &traffic, false); });
  WaitForShutdown(config.duration_sec);
  running = false;
  pub.join();
  printer.Stop();
}

void RunInterSubscriber(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  TrafficCounter traffic;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();
  std::atomic<bool> running{true};
  std::thread sub([&] { SubscriberLoop(config, tracker, &traffic, running, false); });
  WaitForShutdown(config.duration_sec);
  running = false;
  sub.join();
  printer.Stop();
}

}  // namespace
}  // namespace benchmarks

int main(int argc, char** argv) {
  using namespace benchmarks;
  InstallSignalHandlers();
  try {
    const auto config = ParseArgs(argc, argv);
    if (config.mode == Mode::kIntra) {
      RunIntra(config);
    } else {
      if (config.role == Role::kPublisher) {
        RunInterPublisher(config);
      } else if (config.role == Role::kSubscriber) {
        RunInterSubscriber(config);
      } else {
        throw std::invalid_argument("mono role is not valid for inter mode");
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << std::endl;
    return 1;
  }
  return 0;
}
