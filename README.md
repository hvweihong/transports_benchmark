# Multi-Transport Benchmark Suite

该工程提供 ZeroMQ、iceoryx2、Fast DDS、ROS 2 四种通信方式的发布/订阅基准测试，实现 IMU (200 Hz) 与 6 路 RGB 1280×1080@20 Hz 图像流的进程内（intra-process）以及进程间（inter-process）评测，并在日志中打印 CPU 占用、内存占用与端到端通信延迟统计。

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
└── third_party         # git submodule 引入 libzmq、iceoryx2、Fast-DDS
```

## 设计要点

- **统一消息模型**：`benchmarks::ImuSample` 与 `benchmarks::ImageSample` 描述数据格式，便于四种通信方式共享。
- **统计模块**：`LoadMonitor` 周期性采集 `/proc/self/stat` 与 `/proc/self/status`，输出单核 CPU 百分比、常驻/虚拟内存；`LatencyTracker` 聚合均值/最大值/百分位。
- **场景驱动**：每个通信方式的可执行文件都支持 `--role pub|sub|mono`、`--stream imu|image|both`、`--duration` 等参数；其中 `mono` 自动运行 intra 场景，而 `pub/sub` 自动运行 inter 场景，无需手动再传 `--mode`。
- **数据发生器**：IMU 使用正弦波叠加噪声模拟，图像使用伪随机图案；保证基准一致性并能控制频率。
- **构建策略**：每个 `apps/<transport>` 目录都包含独立的 CMake 工程（共用 `cmake/benchmark_common.cmake`），因此在构建某一基准时不会拉起其他传输方式的依赖；ZeroMQ/iceoryx2/Fast DDS 会在各自工程中 `add_subdirectory` 对应的 third_party 子模块，ROS 2 则直接 `find_package` 系统已安装的包。

## 构建

```bash
git submodule update --init --recursive

# 以 Fast DDS 为例
cmake -S apps/fastdds -B build/fastdds -DCMAKE_BUILD_TYPE=Release
cmake --build build/fastdds -j$(nproc)

# 其他传输方式
cmake -S apps/zmq     -B build/zmq     -DCMAKE_BUILD_TYPE=Release
cmake -S apps/iceoryx -B build/iceoryx -DCMAKE_BUILD_TYPE=Release \
      -DRUST_BUILD_ARTIFACT_PATH=/path/to/iceoryx2/target/release  # 若系统未安装 cargo
cmake -S apps/ros2    -B build/ros2    -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

iceoryx2/Fast DDS 的 third_party 依赖会在对应工程中自动构建：  
- iceoryx2 依赖 Rust 工具链（`cargo`/`rustc`）。若无法安装，可先在有 Rust 的主机上执行 `cargo build --release --package iceoryx2-ffi-c`，然后通过 `-DRUST_BUILD_ARTIFACT_PATH=` 指向产物目录。否则 CMake 会尝试联网下载 `iceoryx_hoofs` 子集并调用 cargo。  
ROS 2 工程仍需在 shell 中 `source /opt/ros/<distro>/setup.bash`。

依赖前置：
- ROS 2：需要系统已安装 rclcpp/sensor_msgs/std_msgs/builtin_interfaces。
- iceoryx2：仅需满足共享内存权限，无需启动 RouDi。
- Fast DDS：使用仓库自带源码编译，无额外依赖。

## 运行示例

```bash
# ZeroMQ 进程内测试（单进程）——自动使用 inproc://
./build/zmq/zmq_benchmark --role mono --stream both --duration 15

# ZeroMQ 进程间（两个终端）
./build/zmq/zmq_benchmark --role pub --stream both \
    --imu-endpoint tcp://127.0.0.1:6000 --image-endpoint tcp://127.0.0.1:6001
./build/zmq/zmq_benchmark --role sub --stream both \
    --imu-endpoint tcp://127.0.0.1:6000 --image-endpoint tcp://127.0.0.1:6001

# Fast DDS（需要两个终端，intra 可用 --role mono）
./build/fastdds/fastdds_benchmark --role pub --stream both --duration 20
./build/fastdds/fastdds_benchmark --role sub --stream both --duration 20

# iceoryx2（不再需要 RouDi）
./build/iceoryx/iceoryx_benchmark --role pub --stream both
./build/iceoryx/iceoryx_benchmark --role sub --stream both

# ROS 2（需 source ROS 环境；intra 使用 --role mono）
./build/ros2/ros2_benchmark --role mono --stream both
./build/ros2/ros2_benchmark --role pub --stream both
./build/ros2/ros2_benchmark --role sub --stream both
```

每个应用会在控制台打印周期性统计，例如：

```
[metrics] cpu=135.2% rss=210MB vmem=420MB latency_us(avg=430,p95=520,p99=600,max=720,samples=3000) \
          traffic(imu_pub=200.00/s,imu_sub=200.00/s,img_pub=120.00/s,img_sub=120.00/s)
```

更多细节见 `docs/architecture.md`。
