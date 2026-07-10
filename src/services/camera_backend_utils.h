#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "camera_service.h"

namespace service::camera_backend {

struct ExifMetadata;

enum class PixelFormat {
  RGB565,
  RGB888,
  BGR888,
  XRGB8888,
  XBGR8888,
  YUV420,
  YUYV,
  UYVY,
};

constexpr int kSensorMaxWidth       = 3280;
constexpr int kSensorMaxHeight      = 2464;
constexpr int kDefaultCaptureWidth  = 1640;
constexpr int kDefaultCaptureHeight = 1232;
constexpr int kPreviewWidth         = 226;
constexpr int kPreviewHeight        = 170;
constexpr int kMinZoomPercent       = 100;
constexpr int kMidZoomPercent       = 250;
constexpr int kMaxZoomPercent       = 500;
constexpr int kPanStepPercent       = 8;

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b);
void rgb565_to_rgb888(uint16_t p, uint8_t& r, uint8_t& g, uint8_t& b);
uint8_t clip_u8(int value);
int clamp_int(int value, int min_value, int max_value);
int normalize_zoom_percent(int zoom_percent);
std::string lower_string(std::string s);
std::string make_unique_media_path(const std::string& dir,
                                   const char* prefix,
                                   const char* extension);
std::string make_photo_path();
bool save_jpeg_rgb888(const std::string& path,
                      const std::vector<uint8_t>& rgb,
                      int width,
                      int height,
                      int quality                       = 90,
                      const ExifMetadata* exif_metadata = nullptr);
std::vector<CameraResolution> capture_resolution_candidates(CameraResolution preferred);
bool convert_frame_to_outputs(const std::vector<const uint8_t*>& planes,
                              const std::vector<size_t>& bytes_used,
                              int width,
                              int height,
                              int stride,
                              PixelFormat format,
                              bool is_still,
                              CameraFrame* preview_frame,
                              std::vector<uint8_t>* still_rgb);

}  // namespace service::camera_backend
