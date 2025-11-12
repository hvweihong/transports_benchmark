#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/core/status/StatusMask.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
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

namespace dds = eprosima::fastdds::dds;
namespace rtps = eprosima::fastdds::rtps;

constexpr size_t kImuSerializedSize = sizeof(uint64_t) + sizeof(Nanoseconds) + sizeof(float) * 6;
constexpr size_t kImageHeaderSize = sizeof(uint64_t) + sizeof(Nanoseconds) + sizeof(uint16_t) + sizeof(uint32_t) * 4;
constexpr size_t kImageSerializedSize = kImageHeaderSize + ImagePayloadBytes();
constexpr uint32_t kShmSegmentSize = 64 * 1024 * 1024;  // 64 MB to accommodate large frames.

struct ImuWireMessage {
  uint64_t sequence{0};
  Nanoseconds publish_ts{0};
  float accel[3]{0.0F, 0.0F, 0.0F};
  float gyro[3]{0.0F, 0.0F, 0.0F};
};

struct ImageWireMessage {
  uint64_t sequence{0};
  Nanoseconds publish_ts{0};
  uint16_t stream_id{0};
  uint32_t width{kImageWidth};
  uint32_t height{kImageHeight};
  uint32_t channels{kImageChannels};
  std::vector<uint8_t> data;
};

class ImuTypeSupport : public dds::TopicDataType {
 public:
  ImuTypeSupport() {
    set_name("benchmarks::ImuWireMessage");
    max_serialized_type_size = static_cast<uint32_t>(kImuSerializedSize);
    is_compute_key_provided = false;
  }

  bool serialize(const void* const data,
                 rtps::SerializedPayload_t& payload,
                 dds::DataRepresentationId_t) override {
    if (payload.max_size < kImuSerializedSize) {
      return false;
    }
    const auto* sample = static_cast<const ImuWireMessage*>(data);
    uint8_t* cursor = payload.data;
    auto Copy = [&](const void* src, size_t len) {
      std::memcpy(cursor, src, len);
      cursor += len;
    };
    Copy(&sample->sequence, sizeof(sample->sequence));
    Copy(&sample->publish_ts, sizeof(sample->publish_ts));
    Copy(sample->accel, sizeof(sample->accel));
    Copy(sample->gyro, sizeof(sample->gyro));
    payload.length = static_cast<uint32_t>(kImuSerializedSize);
    return true;
  }

  bool deserialize(rtps::SerializedPayload_t& payload, void* data) override {
    if (payload.length < kImuSerializedSize) {
      return false;
    }
    auto* sample = static_cast<ImuWireMessage*>(data);
    const uint8_t* cursor = payload.data;
    auto Copy = [&](void* dst, size_t len) {
      std::memcpy(dst, cursor, len);
      cursor += len;
    };
    Copy(&sample->sequence, sizeof(sample->sequence));
    Copy(&sample->publish_ts, sizeof(sample->publish_ts));
    Copy(sample->accel, sizeof(sample->accel));
    Copy(sample->gyro, sizeof(sample->gyro));
    return true;
  }

  uint32_t calculate_serialized_size(const void* const,
                                     dds::DataRepresentationId_t) override {
    return static_cast<uint32_t>(kImuSerializedSize);
  }

  void* create_data() override { return new ImuWireMessage(); }

  void delete_data(void* data) override { delete static_cast<ImuWireMessage*>(data); }

  bool compute_key(rtps::SerializedPayload_t&, rtps::InstanceHandle_t&, bool) override { return false; }

  bool compute_key(const void* const, rtps::InstanceHandle_t&, bool) override { return false; }
};

class ImageTypeSupport : public dds::TopicDataType {
 public:
  ImageTypeSupport() {
    set_name("benchmarks::ImageWireMessage");
    max_serialized_type_size = static_cast<uint32_t>(kImageSerializedSize);
    is_compute_key_provided = false;
  }

  bool serialize(const void* const data,
                 rtps::SerializedPayload_t& payload,
                 dds::DataRepresentationId_t) override {
    const auto* sample = static_cast<const ImageWireMessage*>(data);
    const uint32_t payload_size = static_cast<uint32_t>(sample->data.size());
    const size_t total = kImageHeaderSize + payload_size;
    if (payload.max_size < total) {
      return false;
    }
    uint8_t* cursor = payload.data;
    auto Copy = [&](const void* src, size_t len) {
      std::memcpy(cursor, src, len);
      cursor += len;
    };
    Copy(&sample->sequence, sizeof(sample->sequence));
    Copy(&sample->publish_ts, sizeof(sample->publish_ts));
    Copy(&sample->stream_id, sizeof(sample->stream_id));
    Copy(&sample->width, sizeof(sample->width));
    Copy(&sample->height, sizeof(sample->height));
    Copy(&sample->channels, sizeof(sample->channels));
    Copy(&payload_size, sizeof(payload_size));
    if (payload_size > 0) {
      Copy(sample->data.data(), payload_size);
    }
    payload.length = static_cast<uint32_t>(total);
    return true;
  }

  bool deserialize(rtps::SerializedPayload_t& payload, void* data) override {
    if (payload.length < kImageHeaderSize) {
      return false;
    }
    auto* sample = static_cast<ImageWireMessage*>(data);
    const uint8_t* cursor = payload.data;
    auto Copy = [&](void* dst, size_t len) {
      std::memcpy(dst, cursor, len);
      cursor += len;
    };
    Copy(&sample->sequence, sizeof(sample->sequence));
    Copy(&sample->publish_ts, sizeof(sample->publish_ts));
    Copy(&sample->stream_id, sizeof(sample->stream_id));
    Copy(&sample->width, sizeof(sample->width));
    Copy(&sample->height, sizeof(sample->height));
    Copy(&sample->channels, sizeof(sample->channels));
    uint32_t payload_size = 0;
    Copy(&payload_size, sizeof(payload_size));
    const size_t remaining = payload.length - kImageHeaderSize;
    if (remaining < payload_size) {
      return false;
    }
    sample->data.resize(payload_size);
    if (payload_size > 0) {
      std::memcpy(sample->data.data(), cursor, payload_size);
    }
    return true;
  }

  uint32_t calculate_serialized_size(const void* const data,
                                     dds::DataRepresentationId_t) override {
    const auto* sample = static_cast<const ImageWireMessage*>(data);
    return static_cast<uint32_t>(kImageHeaderSize + sample->data.size());
  }

  void* create_data() override { return new ImageWireMessage(); }

  void delete_data(void* data) override { delete static_cast<ImageWireMessage*>(data); }

  bool compute_key(rtps::SerializedPayload_t&, rtps::InstanceHandle_t&, bool) override { return false; }

  bool compute_key(const void* const, rtps::InstanceHandle_t&, bool) override { return false; }
};

class FastDdsContext {
 public:
  explicit FastDdsContext(const BenchmarkConfig& config)
      : config_(config), imu_type_(new ImuTypeSupport()), image_type_(new ImageTypeSupport()) {
    image_topics_.resize(kImageStreams, nullptr);
    image_writers_.resize(kImageStreams, nullptr);
    image_readers_.resize(kImageStreams, nullptr);
  }

  ~FastDdsContext() { Shutdown(); }

  void Initialize() {
    if (participant_) {
      return;
    }

    dds::DomainParticipantQos qos = dds::PARTICIPANT_QOS_DEFAULT;
    qos.name("fastdds_benchmark");
    qos.transport().use_builtin_transports = false;
    std::shared_ptr<rtps::SharedMemTransportDescriptor> shm;
    try {
      shm = std::make_shared<rtps::SharedMemTransportDescriptor>();
      shm->segment_size(std::max<uint32_t>(kShmSegmentSize,
                                           static_cast<uint32_t>(kImageSerializedSize + (4 * 1024 * 1024))));
      shm->max_message_size(static_cast<uint32_t>(kImageSerializedSize));
      qos.transport().user_transports.push_back(shm);
    } catch (const std::exception& ex) {
      std::cerr << "[fastdds] failed to create shared memory transport: " << ex.what() << std::endl;
    }
    if (config_.mode == Mode::kInter) {
      auto udp = std::make_shared<rtps::UDPv4TransportDescriptor>();
      qos.transport().user_transports.push_back(udp);
    } else if (!shm) {
      auto udp = std::make_shared<rtps::UDPv4TransportDescriptor>();
      qos.transport().user_transports.push_back(udp);
    }

    auto* factory = dds::DomainParticipantFactory::get_instance();
    participant_ = factory->create_participant(0, qos);
    if (!participant_) {
      throw std::runtime_error("failed to create Fast DDS participant");
    }
    if (imu_type_.register_type(participant_) != dds::RETCODE_OK) {
      throw std::runtime_error("failed to register IMU type");
    }
    if (image_type_.register_type(participant_) != dds::RETCODE_OK) {
      throw std::runtime_error("failed to register Image type");
    }
  }

  dds::DataWriter* CreateImuWriter() {
    Initialize();
    if (imu_writer_) {
      return imu_writer_;
    }
    auto* publisher = EnsurePublisher();
    auto* topic = EnsureImuTopic();
    dds::DataWriterQos qos = dds::DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = dds::BEST_EFFORT_RELIABILITY_QOS;
    qos.history().kind = dds::KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 64;
    qos.resource_limits().max_samples = 128;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 128;
    imu_writer_ = publisher->create_datawriter(topic, qos, nullptr, dds::StatusMask::none());
    if (!imu_writer_) {
      throw std::runtime_error("failed to create IMU DataWriter");
    }
    return imu_writer_;
  }

  dds::DataWriter* CreateImageWriter(uint16_t stream) {
    Initialize();
    if (stream >= kImageStreams) {
      throw std::out_of_range("image stream index out of range");
    }
    if (image_writers_[stream]) {
      return image_writers_[stream];
    }
    auto* publisher = EnsurePublisher();
    auto* topic = EnsureImageTopic(stream);
    dds::DataWriterQos qos = dds::DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
    qos.history().kind = dds::KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 8;
    qos.resource_limits().max_samples = 16;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 16;
    qos.publish_mode().kind = dds::ASYNCHRONOUS_PUBLISH_MODE;
    image_writers_[stream] = publisher->create_datawriter(topic, qos, nullptr, dds::StatusMask::none());
    if (!image_writers_[stream]) {
      throw std::runtime_error("failed to create image DataWriter");
    }
    return image_writers_[stream];
  }

  dds::DataReader* CreateImuReader(dds::DataReaderListener* listener) {
    Initialize();
    if (imu_reader_) {
      return imu_reader_;
    }
    auto* subscriber = EnsureSubscriber();
    auto* topic = EnsureImuTopic();
    dds::DataReaderQos qos = dds::DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = dds::BEST_EFFORT_RELIABILITY_QOS;
    qos.history().kind = dds::KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 256;
    qos.resource_limits().max_samples = 512;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 512;
    imu_reader_ = subscriber->create_datareader(topic, qos, listener, dds::StatusMask::data_available());
    if (!imu_reader_) {
      throw std::runtime_error("failed to create IMU DataReader");
    }
    return imu_reader_;
  }

  dds::DataReader* CreateImageReader(uint16_t stream, dds::DataReaderListener* listener) {
    Initialize();
    if (stream >= kImageStreams) {
      throw std::out_of_range("image stream index out of range");
    }
    if (image_readers_[stream]) {
      return image_readers_[stream];
    }
    auto* subscriber = EnsureSubscriber();
    auto* topic = EnsureImageTopic(stream);
    dds::DataReaderQos qos = dds::DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
    qos.history().kind = dds::KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 16;
    qos.resource_limits().max_samples = 32;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 32;
    image_readers_[stream] = subscriber->create_datareader(topic, qos, listener, dds::StatusMask::data_available());
    if (!image_readers_[stream]) {
      throw std::runtime_error("failed to create image DataReader");
    }
    return image_readers_[stream];
  }

  dds::DataWriter* imu_writer() const { return imu_writer_; }

  const std::vector<dds::DataWriter*>& image_writers() const { return image_writers_; }

 private:
  dds::Publisher* EnsurePublisher() {
    if (!publisher_) {
      publisher_ = participant_->create_publisher(dds::PUBLISHER_QOS_DEFAULT, nullptr);
      if (!publisher_) {
        throw std::runtime_error("failed to create publisher");
      }
    }
    return publisher_;
  }

  dds::Subscriber* EnsureSubscriber() {
    if (!subscriber_) {
      subscriber_ = participant_->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT, nullptr);
      if (!subscriber_) {
        throw std::runtime_error("failed to create subscriber");
      }
    }
    return subscriber_;
  }

  dds::Topic* EnsureImuTopic() {
    if (!imu_topic_) {
      imu_topic_ = participant_->create_topic(config_.imu_topic, imu_type_.get_type_name(), dds::TOPIC_QOS_DEFAULT);
      if (!imu_topic_) {
        throw std::runtime_error("failed to create IMU topic");
      }
    }
    return imu_topic_;
  }

  dds::Topic* EnsureImageTopic(uint16_t stream) {
    if (stream >= kImageStreams) {
      throw std::out_of_range("image stream index out of range");
    }
    if (!image_topics_[stream]) {
      std::string topic_name = config_.image_topic + "/" + std::to_string(stream);
      image_topics_[stream] = participant_->create_topic(topic_name, image_type_.get_type_name(), dds::TOPIC_QOS_DEFAULT);
      if (!image_topics_[stream]) {
        throw std::runtime_error("failed to create image topic");
      }
    }
    return image_topics_[stream];
  }

  void Shutdown() {
    if (!participant_) {
      return;
    }

    participant_->delete_contained_entities();

    if (imu_topic_) {
      participant_->delete_topic(imu_topic_);
      imu_topic_ = nullptr;
    }
    for (auto& topic : image_topics_) {
      if (topic) {
        participant_->delete_topic(topic);
        topic = nullptr;
      }
    }

    dds::DomainParticipantFactory::get_instance()->delete_participant(participant_);
    participant_ = nullptr;
    publisher_ = nullptr;
    subscriber_ = nullptr;
    imu_writer_ = nullptr;
    imu_reader_ = nullptr;
    std::fill(image_writers_.begin(), image_writers_.end(), nullptr);
    std::fill(image_readers_.begin(), image_readers_.end(), nullptr);
  }

  BenchmarkConfig config_;
  dds::TypeSupport imu_type_;
  dds::TypeSupport image_type_;
  dds::DomainParticipant* participant_{nullptr};
  dds::Publisher* publisher_{nullptr};
  dds::Subscriber* subscriber_{nullptr};
  dds::Topic* imu_topic_{nullptr};
  std::vector<dds::Topic*> image_topics_;
  dds::DataWriter* imu_writer_{nullptr};
  std::vector<dds::DataWriter*> image_writers_;
  dds::DataReader* imu_reader_{nullptr};
  std::vector<dds::DataReader*> image_readers_;
};

class ImuReaderListener : public dds::DataReaderListener {
 public:
  ImuReaderListener(LatencyTracker& tracker, TrafficCounter* traffic)
      : tracker_(tracker), traffic_(traffic) {}

  void on_data_available(dds::DataReader* reader) override {
    dds::SampleInfo info;
    while (reader->take_next_sample(&sample_, &info) == dds::RETCODE_OK) {
      if (info.instance_state == dds::ALIVE_INSTANCE_STATE) {
        tracker_.AddSample(NowNs() - sample_.publish_ts);
        if (traffic_) {
          traffic_->IncrementImuReceived();
        }
      }
    }
  }

 private:
  LatencyTracker& tracker_;
  TrafficCounter* traffic_{nullptr};
  ImuWireMessage sample_;
};

class ImageReaderListener : public dds::DataReaderListener {
 public:
  ImageReaderListener(LatencyTracker& tracker, TrafficCounter* traffic)
      : tracker_(tracker), traffic_(traffic) {}

  void on_data_available(dds::DataReader* reader) override {
    dds::SampleInfo info;
    while (reader->take_next_sample(&sample_, &info) == dds::RETCODE_OK) {
      if (info.instance_state == dds::ALIVE_INSTANCE_STATE) {
        tracker_.AddSample(NowNs() - sample_.publish_ts);
        if (traffic_) {
          traffic_->IncrementImageReceived();
        }
      }
    }
  }

 private:
  LatencyTracker& tracker_;
  TrafficCounter* traffic_{nullptr};
  ImageWireMessage sample_;
};

class FastDdsPublisherWorker {
 public:
  FastDdsPublisherWorker(dds::DataWriter* imu_writer,
                         const std::vector<dds::DataWriter*>& image_writers,
                         TrafficCounter* traffic)
      : imu_writer_(imu_writer), image_writers_(image_writers), traffic_(traffic) {}

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
      if (imu_writer_ && now >= next_imu) {
        next_imu += imu_period;
        auto sample = imu_gen.NextSample();
        ImuWireMessage msg;
        msg.sequence = sample.sequence;
        msg.publish_ts = sample.publish_ts;
        std::memcpy(msg.accel, sample.accel, sizeof(sample.accel));
        std::memcpy(msg.gyro, sample.gyro, sizeof(sample.gyro));
        const auto rc = imu_writer_->write(&msg);
        if (rc != dds::RETCODE_OK) {
          std::cerr << "Fast DDS IMU write error: " << rc << std::endl;
        } else if (traffic_) {
          traffic_->IncrementImuPublished();
        }
      }

      for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
        auto* writer = (stream < image_writers_.size()) ? image_writers_[stream] : nullptr;
        if (!writer) {
          continue;
        }
        if (now >= next_image[stream]) {
          next_image[stream] += image_period;
          auto sample = image_gen.NextSample(stream);
          ImageWireMessage msg;
          msg.sequence = sample.sequence;
          msg.publish_ts = sample.publish_ts;
          msg.stream_id = sample.stream_id;
          msg.width = sample.width;
          msg.height = sample.height;
          msg.channels = sample.channels;
          msg.data = std::move(sample.data);
          const auto rc = writer->write(&msg);
          if (rc != dds::RETCODE_OK) {
            std::cerr << "Fast DDS image write error: " << rc << std::endl;
          } else if (traffic_) {
            traffic_->IncrementImagePublished();
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

 private:
  dds::DataWriter* imu_writer_{nullptr};
  std::vector<dds::DataWriter*> image_writers_;
  TrafficCounter* traffic_{nullptr};
};

BenchmarkConfig ParseArgs(int argc, char** argv) {
  constexpr std::string_view kUsage =
      "Usage: fastdds_benchmark --role mono|pub|sub\n"
      "       --stream imu|image|both [--imu-topic name] [--image-topic base] [--duration seconds]";
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

void RunPublisher(const BenchmarkConfig& config) {
  FastDdsContext context(config);
  context.Initialize();
  TrafficCounter traffic;
  MetricsPrinter printer;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();
  if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
    context.CreateImuWriter();
  }
  if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
    for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
      context.CreateImageWriter(stream);
    }
  }
  FastDdsPublisherWorker worker(context.imu_writer(), context.image_writers(), &traffic);
  std::atomic<bool> running{true};
  std::thread thread([&] { worker.Run(running); });
  WaitForShutdown(config.duration_sec);
  running = false;
  thread.join();
  printer.Stop();
}

void RunSubscriber(const BenchmarkConfig& config) {
  FastDdsContext context(config);
  context.Initialize();
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  TrafficCounter traffic;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();

  std::unique_ptr<ImuReaderListener> imu_listener;
  std::vector<std::unique_ptr<ImageReaderListener>> image_listeners;

  if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
    imu_listener = std::make_unique<ImuReaderListener>(tracker, &traffic);
    context.CreateImuReader(imu_listener.get());
  }
  if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
    image_listeners.resize(kImageStreams);
    for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
      image_listeners[stream] = std::make_unique<ImageReaderListener>(tracker, &traffic);
      context.CreateImageReader(stream, image_listeners[stream].get());
    }
  }

  WaitForShutdown(config.duration_sec);
  printer.Stop();
}

void RunIntra(const BenchmarkConfig& config) {
  FastDdsContext context(config);
  context.Initialize();
  LatencyTracker tracker;
  MetricsPrinter printer;
  printer.AttachTracker(&tracker);
  TrafficCounter traffic;
  printer.AttachTrafficCounter(&traffic);
  printer.Start();

  if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
    context.CreateImuWriter();
  }
  if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
    for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
      context.CreateImageWriter(stream);
    }
  }

  std::unique_ptr<ImuReaderListener> imu_listener;
  std::vector<std::unique_ptr<ImageReaderListener>> image_listeners;
  if (config.stream == StreamType::kImu || config.stream == StreamType::kBoth) {
    imu_listener = std::make_unique<ImuReaderListener>(tracker, &traffic);
    context.CreateImuReader(imu_listener.get());
  }
  if (config.stream == StreamType::kImage || config.stream == StreamType::kBoth) {
    image_listeners.resize(kImageStreams);
    for (uint16_t stream = 0; stream < kImageStreams; ++stream) {
      image_listeners[stream] = std::make_unique<ImageReaderListener>(tracker, &traffic);
      context.CreateImageReader(stream, image_listeners[stream].get());
    }
  }

  FastDdsPublisherWorker worker(context.imu_writer(), context.image_writers(), &traffic);
  std::atomic<bool> running{true};
  std::thread thread([&] { worker.Run(running); });
  WaitForShutdown(config.duration_sec);
  running = false;
  thread.join();
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
      if (config.role != Role::kMono) {
        throw std::invalid_argument("intra mode requires --role mono");
      }
      RunIntra(config);
    } else {
      if (config.role == Role::kPublisher) {
        RunPublisher(config);
      } else if (config.role == Role::kSubscriber) {
        RunSubscriber(config);
      } else {
        throw std::invalid_argument("mono role is only valid in intra mode");
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << std::endl;
    return 1;
  }
  return 0;
}
