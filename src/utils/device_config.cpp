#include "utils/device_config.h"

#include "cp0_config_json.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace util {
namespace {

constexpr CameraResolutionConfig kDefaultResolution{1280, 720};

bool is_supported_resolution(int width, int height) {
  return (width == 1280 && height == 720) || (width == 640 && height == 480);
}

bool parse_int(const std::string& value, int& output) {
  if (value.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || end != value.c_str() + value.size() || parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }
  output = static_cast<int>(parsed);
  return true;
}

}  // namespace

std::string device_config_path() {
  const char* home = std::getenv("HOME");
  return std::string(home && home[0] ? home : "/root") +
         "/.config/cardputerzero/config.json";
}

CameraResolutionConfig load_camera_resolution_config(const std::string& path) {
  std::ifstream input(path.empty() ? device_config_path() : path, std::ios::binary);
  if (!input) {
    return kDefaultResolution;
  }

  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  std::vector<std::pair<std::string, std::string>> entries;
  if (!cp0cfg::from_json(content, entries)) {
    return kDefaultResolution;
  }

  int width = kDefaultResolution.width;
  int height = kDefaultResolution.height;
  for (const auto& entry : entries) {
    if (entry.first == "camera.resolution.width") {
      if (!parse_int(entry.second, width)) {
        return kDefaultResolution;
      }
    } else if (entry.first == "camera.resolution.height") {
      if (!parse_int(entry.second, height)) {
        return kDefaultResolution;
      }
    }
  }
  return is_supported_resolution(width, height) ? CameraResolutionConfig{width, height}
                                                 : kDefaultResolution;
}

}  // namespace util
