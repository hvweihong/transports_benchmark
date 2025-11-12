# 架构说明

## 1. 公共组件

- `include/benchmarks/common/types.hpp`：定义 `Timestamp`, `ImuSample`, `ImageSample` 以及 `BenchmarkConfig`。
- `include/benchmarks/common/data_generators.hpp` + `src/common/data_generators.cpp`：
  - `ImuGenerator` 提供 `NextSample()`，按 200 Hz 生成样本。
  - `ImageGenerator` 支持设置分辨率/帧率/路数，生成伪随机 RGB 图像缓冲区。
- `include/benchmarks/common/load_monitor.hpp` + `src/common/load_monitor.cpp`：提供 `LoadSample SampleLoad()`，以 `/proc/stat` 与 `/proc/self/stat` 计算进程 CPU 百分比，并从 `/proc/self/status` 解析 RSS/VmSize。
- `include/benchmarks/common/latency_tracker.hpp`：维护滑动窗口并输出 `LatencyStats`（平均、标准差、最大、P95/P99）。
- `include/benchmarks/common/runtime.hpp`：提供 `ArgParser` + `ParseArguments` 统一处理 `--role/--stream/--duration` 及扩展参数，同时封装 `InstallSignalHandlers` / `WaitForShutdown` 帮助各 benchmark 在捕获 Ctrl-C 时立即收敛线程。

## 2. 基准运行模式

| 模式      | 说明                                                                                                   |
|-----------|--------------------------------------------------------------------------------------------------------|
| `intra`   | 发布者与订阅者在同一进程内运行；应用以 `--role mono` 启动，内部创建线程驱动发布/订阅。                 |
| `inter`   | 发布者与订阅者在不同进程/终端中运行；应用根据 `--role pub` 或 `--role sub` 启动，仅执行单一角色逻辑。 |

所有基准共享 `--stream imu|image|both` 用于控制数据流；`--duration` 指定运行时间，默认 30 s。
命令行解析阶段会根据 `--role` 自动推导运行模式（`mono`→`intra`，`pub/sub`→`inter`），CLI 不再提供单独的 `--mode` 选项。

## 3. 通信方式实现摘要

### ZeroMQ
- 依赖 `libzmq` submodule。
- Intra 模式使用 `inproc://benchmark` endpoint，Inter 模式使用可配置的 `tcp://`/`ipc://` endpoint。
- IMU 数据使用 `ZMQ_DONTWAIT` push/pull，图像流使用 `PUB/SUB` 并启用 `ZMQ_SNDHWM`/`ZMQ_RCVHWM` 控制高水位。

### iceoryx2
- 依赖 `iceoryx2` 官方仓库（Rust 实现）以及其 `iceoryx_hoofs` 子集。
- 应用每个进程内部创建一个 `iox2::Node<ServiceType::Ipc>`，无需显式启动 RouDi。
- 发布/订阅均使用 iceoryx2 的 C++ binding（`iox2::Publisher`/`iox2::Subscriber`），通过 `ServiceName` 将 IMU 与图像流映射成不同的 publish-subscribe service。

### Fast DDS
- 基于 eProsima Fast DDS，图像流采用异步 `DataWriter` + `DataReader`，IMU 使用可靠 `BEST_EFFORT`。
- 提供 QoS 预设（history depth、reliability、async publishing 等）以匹配高带宽需求。

### ROS 2
- 直接链接系统 ROS 2 (rclcpp, sensor_msgs, std_msgs, builtin_interfaces)。
- Intra 模式使用 `rclcpp::NodeOptions().use_intra_process_comms(true)`。
- Inter 模式运行两个独立进程，依赖 DDS 层负责传输。

## 4. 指标采集

1. **CPU**：通过两次读取 `/proc/stat` 与 `/proc/self/stat` 计算 delta，换算为单核百分比。
2. **内存**：解析 `/proc/self/status` 中的 `VmRSS` 与 `VmSize`。
3. **延迟**：发布端在消息头写入 `uint64_t` 纳秒时间戳，订阅端读取并与当前时间比较。
4. **输出**：`MetricsPrinter` 线程每 5 秒打印一次，除 CPU/内存/延迟外还会输出 IMU 与图像的发布/订阅速率（以每秒计）。示例：
   `t=12.0s cpu=135.2% rss=210MB vmem=420MB latency_us(...) traffic(imu_pub=200/s,imu_sub=200/s,...)`

## 5. CMake 与目标

- 顶层不再统一驱动 CMake；`apps/zmq|iceoryx|fastdds|ros2` 各自是独立工程。
- `cmake/benchmark_common.cmake` 提供 `benchmarks_add_common_library()`，在任意工程内复用 `benchmark_common` 静态库（含公共头/源和 `Threads::Threads` 依赖）。
- ZeroMQ/iceoryx2/Fast DDS 工程内部调用 `add_subdirectory(../../third_party/...)` 拉起相应子模块，互不干扰；ROS 2 工程直接 `find_package` 系统安装的包。
- 构建可执行文件时统一链接 `benchmark_common`，保证统计/类型定义逻辑一致。

## 6. 后续工作

- 根据实际硬件调优 QoS/缓冲区。
- 扩展日志输出为 CSV/JSON，便于 post-processing。
- 增加自动运行脚本（可选）。
