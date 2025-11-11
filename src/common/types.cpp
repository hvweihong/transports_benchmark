#include "benchmarks/common/types.hpp"

#include <algorithm>
#include <stdexcept>

namespace benchmarks {
namespace {
std::string ToLower(std::string_view text) {
  std::string s{text};
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
}  // namespace

StreamType ParseStreamType(std::string_view text) {
  auto t = ToLower(text);
  if (t == "imu") {
    return StreamType::kImu;
  }
  if (t == "image") {
    return StreamType::kImage;
  }
  if (t == "both") {
    return StreamType::kBoth;
  }
  throw std::invalid_argument("unknown stream type: " + std::string{text});
}

Mode ParseMode(std::string_view text) {
  auto t = ToLower(text);
  if (t == "intra") {
    return Mode::kIntra;
  }
  if (t == "inter") {
    return Mode::kInter;
  }
  throw std::invalid_argument("unknown mode: " + std::string{text});
}

Role ParseRole(std::string_view text) {
  auto t = ToLower(text);
  if (t == "pub" || t == "publisher") {
    return Role::kPublisher;
  }
  if (t == "sub" || t == "subscriber") {
    return Role::kSubscriber;
  }
  if (t == "mono" || t == "both") {
    return Role::kMono;
  }
  throw std::invalid_argument("unknown role: " + std::string{text});
}

}  // namespace benchmarks
