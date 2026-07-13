#pragma once

#include <string>

namespace util {

struct CameraResolutionConfig {
  int width{1280};
  int height{720};
};

std::string device_config_path();
CameraResolutionConfig load_camera_resolution_config(const std::string& path = {});

}  // namespace util
