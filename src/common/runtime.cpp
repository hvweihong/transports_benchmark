#include "benchmarks/common/runtime.hpp"

#include <csignal>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "benchmarks/common/types.hpp"

namespace benchmarks {
namespace {
volatile std::sig_atomic_t g_stop_flag = 0;

void GlobalSignalHandler(int) {
  g_stop_flag = 1;
}
}  // namespace

ArgParser::ArgParser(int argc, char** argv) : argc_(argc), argv_(argv) {}

bool ArgParser::HasNext() const { return index_ < argc_; }

std::string ArgParser::Next() {
  if (!HasNext()) {
    return {};
  }
  return std::string(argv_[index_++]);
}

std::string ArgParser::ConsumeValue(std::string_view option) {
  if (!HasNext()) {
    throw std::invalid_argument("missing value for " + std::string(option));
  }
  return std::string(argv_[index_++]);
}

BenchmarkConfig ParseArguments(int argc,
                               char** argv,
                               const ArgHandler& handler,
                               std::string_view usage) {
  ArgParser parser(argc, argv);
  BenchmarkConfig config;
  while (parser.HasNext()) {
    const std::string arg = parser.Next();
    if (arg == "--role") {
      config.role = ParseRole(parser.ConsumeValue(arg));
    } else if (arg == "--stream") {
      config.stream = ParseStreamType(parser.ConsumeValue(arg));
    } else if (arg == "--duration") {
      config.duration_sec = std::stod(parser.ConsumeValue(arg));
    } else if (arg == "--help" || arg == "-h") {
      if (!usage.empty()) {
        std::cout << usage << std::endl;
      }
      std::exit(0);
    } else if (handler && handler(arg, parser, config)) {
      continue;
    } else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }
  config.mode = (config.role == Role::kMono) ? Mode::kIntra : Mode::kInter;
  return config;
}

void InstallSignalHandlers() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::signal(SIGINT, GlobalSignalHandler);
    std::signal(SIGTERM, GlobalSignalHandler);
  });
}

bool StopRequested() { return g_stop_flag != 0; }

void RequestStop() { g_stop_flag = 1; }

bool WaitForShutdown(double duration_sec, const std::function<bool()>& external_stop) {
  if (duration_sec <= 0.0) {
    return StopRequested() || (external_stop && external_stop());
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration_sec);
  while (std::chrono::steady_clock::now() < deadline) {
    if (StopRequested()) {
      break;
    }
    if (external_stop && external_stop()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return StopRequested() || (external_stop && external_stop());
}

}  // namespace benchmarks
