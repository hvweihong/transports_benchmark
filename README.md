# Multi-Transport Benchmark Suite

该工程提供 ZeroMQ、iceoryx2、Fast DDS、ROS 2 四种通信方式的发布/订阅基准测试，实现 IMU (200 Hz) 与 6 路 RGB 1280×1080@20 Hz 图像流的进程内（intra-process）以及进程间（inter-process）评测，并在日志中打印 CPU 占用、内存占用与端到端通信延迟统计。

## 设计要点

- **统一消息模型**：`benchmarks::ImuSample` 与 `benchmarks::ImageSample` 描述数据格式，便于四种通信方式共享。
- **统计模块**：`LoadMonitor` 周期性采集 `/proc/self/stat` 与 `/proc/self/status`，输出单核 CPU 百分比、常驻/虚拟内存；`LatencyTracker` 聚合均值/最大值/百分位。
- **场景驱动**：每个通信方式的可执行文件都支持 `--role pub|sub|mono`、`--stream imu|image|both`、`--duration` 等参数；其中 `mono` 用于运行 intra(进程内) 场景，而 `pub/sub` 用于运行 inter(进程间) 场景。
- **数据发生器**：IMU 使用正弦波叠加噪声模拟，图像使用伪随机图案；保证基准一致性并能控制频率。
- **构建策略**：每个 `apps/<transport>` 目录都包含独立的 CMake 工程（共用 `cmake/benchmark_common.cmake`），因此在构建某一基准时不会拉起其他传输方式的依赖；ZeroMQ/iceoryx2/Fast DDS 会在各自工程中 `add_subdirectory` 对应的 third_party 子模块，ROS 2 则直接 `find_package` 系统已安装的包。

## 构建

依赖前置：
- ROS 2：需要系统已安装ros humble，并执行：
```
source /opt/ros/<distro>/setup.bash
```
- iceoryx2： 依赖 Rust 工具链（`cargo`/`rustc`）。请先安装：
```
curl https://sh.rustup.rs -sSf | sh -s -- -y
source ~/.cargo/env
```

```bash
git submodule update --init --recursive

# 推荐：使用统一脚本
./scripts/build_benchmarks.py --transports fastdds,zmq,iceoryx

# 或手动执行 CMake
# 编译fastdds benchmark
cmake -S apps/fastdds -B build/fastdds
cmake --build build/fastdds -j$(nproc)
# 编译zmq benchmark
cmake -S apps/zmq     -B build/zmq
cmake --build build/zmq -j$(nproc)
# 编译iceoryx benchmark
cmake -S apps/iceoryx -B build/iceoryx
cmake --build build/iceoryx -j$(nproc)
# 编译ros2 benchmark
cmake -S apps/ros2    -B build/ros2
cmake --build build/ros2 -j$(nproc)
```

## 运行示例

```bash
# 推荐：使用统一脚本
./scripts/run_benchmarks.py --transports fastdds,zmq,iceoryx

# 或单独运行某种传输
./build/zmq/zmq_benchmark --role mono --stream both --duration 20
./build/zmq/zmq_benchmark --role pub --stream both --duration 20
./build/zmq/zmq_benchmark --role sub --stream both --duration 20
# ros2 由于启用loan message 依赖一些IDL库，需要指定 LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/home/hv/Documents/project/test/benchmark/build/ros2:$LD_LIBRARY_PATH
./build/ros2/ros2_benchmark --role mono --stream both --duration 20
```

`scripts/run_benchmarks.py` 会对每种传输依次执行 `--role mono/pub/sub` 三个场景（pub/sub 会并行运行形成 inter
测试），stdout/stderr 保存到 `runs/<timestamp>/<transport>_<role>.log` 并计算测试指标的平均值生成 `summary.txt`。

每个应用会在控制台打印周期性统计，例如：

```
[metrics] cpu=135.2% rss=210MB vmem=420MB latency_us(avg=430,p95=520,p99=600,max=720,samples=3000) \
          traffic(imu_pub=200.00/s,imu_sub=200.00/s,img_pub=120.00/s,img_sub=120.00/s)
```

更多细节见 `docs/architecture.md`。
