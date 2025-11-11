# Multi-Transport Benchmark Suite

该工程提供 ZeroMQ、iceoryx、Fast DDS、ROS 2 四种通信方式的发布/订阅基准测试，实现 IMU (200 Hz) 与 6 路 RGB 1280×1080@20 Hz 图像流的进程内（intra-process）以及进程间（inter-process）评测，并在日志中打印 CPU 占用、内存占用与端到端通信延迟统计。

## 目录结构

```
├── apps                # 各通信方式独立测试程序
│   ├── fastdds
│   ├── iceoryx
│   ├── ros2
│   └── zmq
├── cmake               # CMake 辅助脚本
├── docs                # 设计/使用文档
├── include/benchmarks  # 公共头文件
├── src/common          # 公共实现（消息模型、统计工具等）
└── third_party         # git submodule 引入 libzmq、iceoryx、Fast-DDS
```

## 设计要点

- **统一消息模型**：`benchmarks::ImuSample` 与 `benchmarks::ImageSample` 描述数据格式，便于四种通信方式共享。
- **统计模块**：`LoadMonitor` 周期性采集 `/proc/self/stat` 与 `/proc/self/status`，输出单核 CPU 百分比、常驻/虚拟内存；`LatencyTracker` 聚合均值/最大值/百分位。
- **场景驱动**：每个通信方式的可执行文件都支持 `--mode intra|inter`、`--role pub|sub|mono`、`--stream imu|image|both` 参数，用于区分同进程模式（单进程运行 `mono`）与跨进程模式（pub/sub 分别运行）。
- **数据发生器**：IMU 使用正弦波叠加噪声模拟，图像使用伪随机图案；保证基准一致性并能控制频率。
- **构建策略**：
  - ZeroMQ / iceoryx / Fast DDS 通过 `add_subdirectory(third_party/...)` 直接构建。
  - ROS 2 依赖系统安装，使用 `find_package(rclcpp sensor_msgs std_msgs)`。
  - 统一 `CMakeLists.txt`，可通过 `-DBUILD_<TRANSPORT>=ON/OFF` 控制需要的基准。

## 构建

```bash
git submodule update --init --recursive

# 如未安装 ROS 2，可暂时关闭 BUILD_ROS2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_ZMQ=ON -DBUILD_ICEORYX=ON -DBUILD_FASTDDS=ON -DBUILD_ROS2=ON
cmake --build build -j$(nproc)
```

> iceoryx 会默认尝试下载 cpptoml；已在 CMake 中强制 `TOML_CONFIG=OFF`，无需外网即可完成配置。

依赖前置：
- ROS 2：需在 shell 中 `source /opt/ros/<distro>/setup.bash`，并保持 `BUILD_ROS2=ON`。
- iceoryx：运行任何基准前先启动 `iox-roudi`（inter 模式需要第二进程；intra 模式使用同一 RouDi）。
- Fast DDS：使用仓库自带源码编译，无额外依赖。

## 运行示例

```bash
# ZeroMQ 进程内测试（单进程）——自动使用 inproc://
./build/apps/zmq/zmq_benchmark --mode intra --role mono --stream both --duration 15

# ZeroMQ 进程间（两个终端）
./build/apps/zmq/zmq_benchmark --mode inter --role pub --stream both \
    --imu-endpoint tcp://127.0.0.1:6000 --image-endpoint tcp://127.0.0.1:6001
./build/apps/zmq/zmq_benchmark --mode inter --role sub --stream both \
    --imu-endpoint tcp://127.0.0.1:6000 --image-endpoint tcp://127.0.0.1:6001

# Fast DDS（需要两个终端，intra 可用 --role mono）
./build/apps/fastdds/fastdds_benchmark --mode inter --role pub --stream both --duration 20
./build/apps/fastdds/fastdds_benchmark --mode inter --role sub --stream both --duration 20

# iceoryx（确保已有 iox-roudi 运行）
./build/apps/iceoryx/iceoryx_benchmark --mode inter --role pub --stream both
./build/apps/iceoryx/iceoryx_benchmark --mode inter --role sub --stream both

# ROS 2（需 source ROS 环境；intra 使用 --role mono）
./build/apps/ros2/ros2_benchmark --mode intra --role mono --stream both
./build/apps/ros2/ros2_benchmark --mode inter --role pub --stream both
./build/apps/ros2/ros2_benchmark --mode inter --role sub --stream both
```

每个应用会在控制台打印周期性统计，例如：

```
[metrics] cpu=135.2%_single, rss=210MB, latency_us: avg=430, p99=600, max=720
```

更多细节见 `docs/architecture.md`。
