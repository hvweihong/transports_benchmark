#include <zmq.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
  uint32_t payload_size;
};

volatile std::sig_atomic_t g_stop_flag = 0;

void SignalHandler(int) { g_stop_flag = 1; }

void* CreateSocket(void* context, int type, const std::string& endpoint, bool bind_socket) {
  void* socket = zmq_socket(context, type);
  if (!socket) {
    throw std::runtime_error("failed to create socket");
  }
  const int hwm = 10;
  zmq_setsockopt(socket, ZMQ_SNDHWM, &hwm, sizeof(hwm));
  zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
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

  while (running.load() && !g_stop_flag) {
    const auto now = std::chrono::steady_clock::now();
    if (imu_socket && now >= next_imu) {
      next_imu += imu_period;
      const auto sample = imu_gen.NextSample();
      ImuWireMessage wire{sample.sequence, sample.publish_ts,
                          {sample.accel[0], sample.accel[1], sample.accel[2]},
                          {sample.gyro[0], sample.gyro[1], sample.gyro[2]}};
      zmq_send(imu_socket, &wire, sizeof(wire), 0);
    }

    if (image_socket) {
      for (uint16_t stream_id = 0; stream_id < kImageStreams; ++stream_id) {
        const auto image_period = std::chrono::nanoseconds(1'000'000'000ULL / kImageRatePerStreamHz);
        if (now >= next_image[stream_id]) {
          next_image[stream_id] += image_period;
          auto image = image_gen.NextSample(stream_id);
          ImageWireHeader header{image.sequence, image.publish_ts, stream_id, image.width,
                                 image.height, image.channels,
                                 static_cast<uint32_t>(image.data.size())};
          std::vector<uint8_t> buffer(sizeof(header) + image.data.size());
          std::memcpy(buffer.data(), &header, sizeof(header));
          std::memcpy(buffer.data() + sizeof(header), image.data.data(), image.data.size());
          zmq_send(image_socket, buffer.data(), buffer.size(), 0);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  CloseSocket(imu_socket);
  CloseSocket(image_socket);
  if (owns_context) {
    zmq_ctx_term(context);
  }
}

void SubscriberLoop(const BenchmarkConfig& config,
                    LatencyTracker& tracker,
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

  while (running.load() && !g_stop_flag) {
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
      }
    }
    if (image_index >= 0 && (items[image_index].revents & ZMQ_POLLIN)) {
      zmq_msg_t msg;
      zmq_msg_init(&msg);
      const int rc = zmq_msg_recv(&msg, image_socket, 0);
      if (rc > static_cast<int>(sizeof(ImageWireHeader))) {
        const auto* header = reinterpret_cast<const ImageWireHeader*>(zmq_msg_data(&msg));
        tracker.AddSample(NowNs() - header->publish_ts);
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
  BenchmarkConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto NextValue = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + name);
      }
      return argv[++i];
    };
    if (arg == "--mode") {
      config.mode = ParseMode(NextValue(arg.c_str()));
    } else if (arg == "--role") {
      config.role = ParseRole(NextValue(arg.c_str()));
    } else if (arg == "--stream") {
      config.stream = ParseStreamType(NextValue(arg.c_str()));
    } else if (arg == "--imu-endpoint") {
      config.imu_endpoint = NextValue(arg.c_str());
    } else if (arg == "--image-endpoint") {
      config.image_endpoint = NextValue(arg.c_str());
    } else if (arg == "--duration") {
      config.duration_sec = std::stod(NextValue(arg.c_str()));
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: zmq_benchmark [--mode intra|inter] [--role mono|pub|sub] [--stream imu|image|both]\n"
                << "                      [--imu-endpoint tcp://host:port] [--image-endpoint tcp://host:port]\n"
                << "                      [--duration seconds]\n";
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
  void* context = zmq_ctx_new();
  if (!context) {
    throw std::runtime_error("failed to create context");
  }
  std::thread pub([&] { PublisherLoop(config, running, true, context); });
  std::thread sub([&] { SubscriberLoop(config, tracker, running, true, context); });
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(config.duration_sec * 1000)));
  running = false;
  pub.join();
  sub.join();
  zmq_ctx_term(context);
  printer.Stop();
}

void RunInterPublisher(const BenchmarkConfig& config) {
  std::atomic<bool> running{true};
  std::thread pub([&] { PublisherLoop(config, running, false); });
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(config.duration_sec * 1000)));
  running = false;
  pub.join();
}

void RunInterSubscriber(const BenchmarkConfig& config) {
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  printer.Start();
  std::atomic<bool> running{true};
  std::thread sub([&] { SubscriberLoop(config, tracker, running, false); });
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(config.duration_sec * 1000)));
  running = false;
  sub.join();
  printer.Stop();
}

}  // namespace
}  // namespace benchmarks

int main(int argc, char** argv) {
  using namespace benchmarks;
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
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
