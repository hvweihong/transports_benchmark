#include "benchmarks/common/load_monitor.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <fstream>
#include <sstream>
#include <string>

namespace benchmarks {
namespace {
uint64_t ReadTotalJiffies() {
  std::ifstream file("/proc/stat");
  std::string line;
  if (!file.is_open() || !std::getline(file, line)) {
    return 0;
  }
  std::istringstream iss(line);
  std::string cpu;
  uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
  iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
  return user + nice + system + idle + iowait + irq + softirq + steal;
}

uint64_t ReadProcessJiffies() {
  std::ifstream file("/proc/self/stat");
  std::string token;
  for (int i = 0; i < 13; ++i) {
    file >> token;
  }
  uint64_t utime = 0;
  uint64_t stime = 0;
  file >> utime >> stime;
  return utime + stime;
}

std::pair<double, double> ReadMemoryBytes() {
  std::ifstream file("/proc/self/status");
  std::string line;
  double rss = 0.0;
  double vms = 0.0;
  while (std::getline(file, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream iss(line.substr(6));
      double kb = 0.0;
      iss >> kb;
      rss = kb * 1024.0;
    } else if (line.rfind("VmSize:", 0) == 0) {
      std::istringstream iss(line.substr(7));
      double kb = 0.0;
      iss >> kb;
      vms = kb * 1024.0;
    }
  }
  return {rss, vms};
}

long CpuCount() {
  static long count = [] {
    long value = sysconf(_SC_NPROCESSORS_ONLN);
    return value > 0 ? value : 1;
  }();
  return count;
}

}  // namespace

LoadMonitor::LoadMonitor()
    : last_total_jiffies_(ReadTotalJiffies()),
      last_proc_jiffies_(ReadProcessJiffies()),
      last_wall_(std::chrono::steady_clock::now()) {}

LoadSample LoadMonitor::Sample() {
  const auto total = ReadTotalJiffies();
  const auto proc = ReadProcessJiffies();
  const auto delta_total = total - last_total_jiffies_;
  const auto delta_proc = proc - last_proc_jiffies_;
  last_total_jiffies_ = total;
  last_proc_jiffies_ = proc;

  LoadSample sample;
  if (delta_total > 0) {
    const double usage = static_cast<double>(delta_proc) / static_cast<double>(delta_total);
    sample.cpu_percent = usage * static_cast<double>(CpuCount()) * 100.0;
  }
  auto [rss, vms] = ReadMemoryBytes();
  sample.rss_bytes = rss;
  sample.virt_bytes = vms;
  return sample;
}

}  // namespace benchmarks
