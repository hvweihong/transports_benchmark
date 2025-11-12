#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "benchmarks/common/types.hpp"

namespace benchmarks {

class ArgParser {
 public:
  ArgParser(int argc, char** argv);

  bool HasNext() const;

  std::string Next();

  std::string ConsumeValue(std::string_view option);

 private:
  int argc_;
  char** argv_;
  int index_{1};
};

using ArgHandler = std::function<bool(std::string_view option,
                                      ArgParser& parser,
                                      BenchmarkConfig& config)>;

BenchmarkConfig ParseArguments(int argc,
                               char** argv,
                               const ArgHandler& handler,
                               std::string_view usage);

void InstallSignalHandlers();

bool StopRequested();

void RequestStop();

bool WaitForShutdown(double duration_sec,
                     const std::function<bool()>& external_stop = {});

}  // namespace benchmarks
