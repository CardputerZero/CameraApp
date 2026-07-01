#include "services/libcamera_backend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <utility>

#include "services/camera_backend_utils.h"
#include "services/jpeg_metadata.h"
#include "services/video_recorder.h"
#include "utils/logger.h"

#if !USE_DESKTOP
#include <fcntl.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/control_ids.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/property_ids.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <map>
#include <vector>
#endif

namespace service {
namespace {

using namespace camera_backend;
constexpr unsigned int kHighResolutionBufferCount = 4;
constexpr unsigned int kFallbackBufferCount       = 3;

#if !USE_DESKTOP
void sync_dma_buf(int fd, uint64_t flags) {
  if (fd < 0) {
    return;
  }

  struct dma_buf_sync sync{flags};
  (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

PixelFormat map_libcamera_format(const libcamera::PixelFormat& format) {
  if (format == libcamera::formats::YUV420) return PixelFormat::YUV420;
  if (format == libcamera::formats::YUYV) return PixelFormat::YUYV;
  if (format == libcamera::formats::UYVY) return PixelFormat::UYVY;
  if (format == libcamera::formats::RGB565) return PixelFormat::RGB565;
  if (format == libcamera::formats::RGB888) return PixelFormat::RGB888;
  if (format == libcamera::formats::BGR888) return PixelFormat::BGR888;
  if (format == libcamera::formats::XRGB8888) return PixelFormat::XRGB8888;
  return PixelFormat::XBGR8888;
}

std::string escape_json_string(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

template <typename T>
void append_json_value(std::ostringstream& json, const char* key, const std::optional<T>& value) {
  if (!value) {
    return;
  }
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":" << *value;
}

template <typename T>
void append_json_value(std::ostringstream& json, const char* key, T value) {
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":" << value;
}

void append_json_bool(std::ostringstream& json, const char* key, const std::optional<bool>& value) {
  if (!value) {
    return;
  }
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":" << (*value ? "true" : "false");
}

void append_json_bool(std::ostringstream& json, const char* key, bool value) {
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":" << (value ? "true" : "false");
}

void append_json_string(std::ostringstream& json, const char* key, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":\"" << escape_json_string(value) << '"';
}

void append_json_size(std::ostringstream& json, const char* key, const libcamera::Size& value) {
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":{\"width\":" << value.width << ",\"height\":" << value.height << '}';
}

void append_json_rectangle(std::ostringstream& json,
                           const char* key,
                           const libcamera::Rectangle& value) {
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":{\"x\":" << value.x << ",\"y\":" << value.y
       << ",\"width\":" << value.width << ",\"height\":" << value.height << '}';
}

template <typename Span>
void append_json_array(std::ostringstream& json, const char* key, const Span& value) {
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":[";
  for (size_t i = 0; i < value.size(); ++i) {
    if (i > 0) {
      json << ',';
    }
    json << value[i];
  }
  json << ']';
}

template <typename Span>
void append_json_rectangles(std::ostringstream& json, const char* key, const Span& value) {
  if (json.tellp() > 1) {
    json << ',';
  }
  json << '"' << key << "\":[";
  for (size_t i = 0; i < value.size(); ++i) {
    if (i > 0) {
      json << ',';
    }
    json << "{\"x\":" << value[i].x << ",\"y\":" << value[i].y << ",\"width\":" << value[i].width
         << ",\"height\":" << value[i].height << '}';
  }
  json << ']';
}

uint16_t exif_metering_mode(int32_t mode) {
  switch (mode) {
    case libcamera::controls::MeteringCentreWeighted:
      return 2;
    case libcamera::controls::MeteringSpot:
      return 3;
    case libcamera::controls::MeteringMatrix:
      return 5;
    default:
      return 255;
  }
}

uint16_t exif_light_source(int32_t awb_mode) {
  switch (awb_mode) {
    case libcamera::controls::AwbDaylight:
      return 1;
    case libcamera::controls::AwbFluorescent:
      return 2;
    case libcamera::controls::AwbIncandescent:
    case libcamera::controls::AwbTungsten:
      return 3;
    case libcamera::controls::AwbCloudy:
      return 10;
    case libcamera::controls::AwbAuto:
      return 0;
    default:
      return 255;
  }
}

std::string control_info_value_string(const libcamera::ControlValue& value) {
  return value.isNone() ? std::string{} : value.toString();
}
#endif

}  // namespace

struct LibcameraBackend::Impl {
#if !USE_DESKTOP
  struct FocusCapability {
    bool af_mode{false};
    bool af_trigger{false};
    bool af_state{false};
    bool focus_fom{false};
    bool lens_position{false};
    std::string lens_position_min;
    std::string lens_position_max;
    std::string lens_position_default;
  };

  struct MappedBuffer {
    struct Plane {
      void* addr{nullptr};
      size_t size{0};
      int fd{-1};
      size_t data_offset{0};
      size_t data_size{0};
    };

    std::vector<Plane> planes;
  };

  std::unique_ptr<libcamera::CameraManager> manager;
  std::shared_ptr<libcamera::Camera> camera;
  std::unique_ptr<libcamera::CameraConfiguration> config;
  std::unique_ptr<libcamera::FrameBufferAllocator> allocator;
  libcamera::Stream* preview_stream{nullptr};
  libcamera::Stream* still_stream{nullptr};
  std::vector<std::unique_ptr<libcamera::Request>> requests;
  std::vector<libcamera::FrameBuffer*> preview_buffers;
  std::vector<libcamera::FrameBuffer*> free_still_buffers;
  std::map<const libcamera::FrameBuffer*, MappedBuffer> mapped_buffers;
  std::mutex mutex;
  CameraFrame pending_frame;
  std::vector<uint8_t> still_rgb;
  bool new_frame{false};
  bool opened{false};
  bool streaming{false};
  std::atomic<bool> capture_requested{false};
  CaptureState capture_state{CaptureState::Idle};
  VideoState video_state{VideoState::Idle};
  std::string last_capture_path;
  std::string last_video_path_value;
  std::string last_error;
  MjpegAviWriter video_writer;
  int video_quality{80};
  CameraResolution capture_resolution{kSensorMaxWidth, kSensorMaxHeight};
  CameraZoomState zoom_state{};
  libcamera::Rectangle scaler_crop_max{};
  int preview_w{kPreviewWidth};
  int preview_h{kPreviewHeight};
  int preview_stride{kPreviewWidth * 2};
  libcamera::PixelFormat preview_format{libcamera::formats::RGB565};
  int still_w{kDefaultCaptureWidth};
  int still_h{kDefaultCaptureHeight};
  int still_stride{kDefaultCaptureWidth};
  libcamera::PixelFormat still_format{libcamera::formats::YUV420};
  std::string camera_model{"CardputerZero IMX219"};
  float sensor_sensitivity{100.0f};
  bool has_sensor_sensitivity{false};
  FocusCapability focus_capability{};

  static bool is_supported(const libcamera::PixelFormat& format) {
    return format == libcamera::formats::YUV420 || format == libcamera::formats::YUYV ||
           format == libcamera::formats::UYVY || format == libcamera::formats::RGB565 ||
           format == libcamera::formats::RGB888 || format == libcamera::formats::BGR888 ||
           format == libcamera::formats::XRGB8888 || format == libcamera::formats::XBGR8888;
  }

  bool configure_stream(CameraResolution resolution, unsigned int buffer_count) {
    config = camera->generateConfiguration(
        {libcamera::StreamRole::Viewfinder, libcamera::StreamRole::StillCapture});
    if (!config || config->size() < 2) {
      last_error = "Camera configuration generation failed";
      LOG_ERROR("{}", last_error);
      return false;
    }

    libcamera::StreamConfiguration& preview_cfg = config->at(0);
    preview_cfg.size.width                      = kPreviewWidth;
    preview_cfg.size.height                     = kPreviewHeight;
    preview_cfg.pixelFormat                     = libcamera::formats::RGB565;
    preview_cfg.bufferCount                     = buffer_count;

    libcamera::StreamConfiguration& still_cfg = config->at(1);
    still_cfg.size.width                      = static_cast<unsigned int>(resolution.width);
    still_cfg.size.height                     = static_cast<unsigned int>(resolution.height);
    still_cfg.pixelFormat                     = libcamera::formats::YUV420;
    still_cfg.bufferCount                     = 1;

    if (config->validate() == libcamera::CameraConfiguration::Invalid) {
      last_error = "Invalid camera configuration";
      LOG_WARN("{}: {}x{} buffers={}",
               last_error,
               resolution.width,
               resolution.height,
               buffer_count);
      return false;
    }

    if (camera->configure(config.get())) {
      last_error = "Camera configure failed";
      LOG_WARN("{}: {}x{} buffers={}",
               last_error,
               resolution.width,
               resolution.height,
               buffer_count);
      return false;
    }

    libcamera::StreamConfiguration& active_preview_cfg = config->at(0);
    libcamera::StreamConfiguration& active_still_cfg   = config->at(1);
    if (!is_supported(active_preview_cfg.pixelFormat) ||
        !is_supported(active_still_cfg.pixelFormat)) {
      last_error =
          "Unsupported camera stream format: preview=" + active_preview_cfg.pixelFormat.toString() +
          " still=" + active_still_cfg.pixelFormat.toString();
      LOG_WARN("{}", last_error);
      return false;
    }

    preview_stream = active_preview_cfg.stream();
    still_stream   = active_still_cfg.stream();
    preview_w      = static_cast<int>(active_preview_cfg.size.width);
    preview_h      = static_cast<int>(active_preview_cfg.size.height);
    preview_stride = static_cast<int>(active_preview_cfg.stride);
    preview_format = active_preview_cfg.pixelFormat;
    still_w        = static_cast<int>(active_still_cfg.size.width);
    still_h        = static_cast<int>(active_still_cfg.size.height);
    still_stride   = static_cast<int>(active_still_cfg.stride);
    still_format   = active_still_cfg.pixelFormat;

    const auto crop_max = camera->properties().get(libcamera::properties::ScalerCropMaximum);
    scaler_crop_max =
        crop_max ? *crop_max : libcamera::Rectangle(0, 0, kSensorMaxWidth, kSensorMaxHeight);

    {
      std::lock_guard<std::mutex> lock(mutex);
      pending_frame.width  = kPreviewWidth;
      pending_frame.height = kPreviewHeight;
      pending_frame.rgb565.assign(kPreviewWidth * kPreviewHeight, 0);
      still_rgb.assign(still_w * still_h * 3, 0);
    }

    allocator                   = std::make_unique<libcamera::FrameBufferAllocator>(camera);
    const int preview_allocated = allocator->allocate(preview_stream);
    const int still_allocated   = allocator->allocate(still_stream);
    if (preview_allocated < 0 || still_allocated < 0) {
      last_error = "Camera framebuffer allocation failed";
      LOG_WARN("{}: preview={}x{} still={}x{} buffers={}",
               last_error,
               preview_w,
               preview_h,
               still_w,
               still_h,
               buffer_count);
      allocator.reset();
      return false;
    }

    if (allocator->buffers(preview_stream).empty() || allocator->buffers(still_stream).empty()) {
      last_error = "Camera framebuffer allocation returned no buffers";
      LOG_WARN("{}: preview={}x{} still={}x{}", last_error, preview_w, preview_h, still_w, still_h);
      allocator.reset();
      return false;
    }

    return true;
  }

  void release_stream_resources() {
    if (camera && streaming) {
      camera->requestCompleted.disconnect(this);
      streaming = false;
      camera->stop();
    }
    streaming = false;

    requests.clear();

    for (auto& item : mapped_buffers) {
      for (auto& plane : item.second.planes) {
        if (plane.addr && plane.addr != MAP_FAILED) {
          ::munmap(plane.addr, plane.size);
        }
      }
    }
    mapped_buffers.clear();
    preview_buffers.clear();
    free_still_buffers.clear();
    allocator.reset();
    preview_stream = nullptr;
    still_stream   = nullptr;
  }

  bool map_buffer(const libcamera::FrameBuffer* buffer) {
    const auto planes = buffer->planes();
    if (planes.empty()) {
      return false;
    }

    MappedBuffer mapped;
    for (const auto& plane : planes) {
      const long page_size_value = ::sysconf(_SC_PAGE_SIZE);
      const size_t page_size    = page_size_value > 0 ? static_cast<size_t>(page_size_value) : 4096;
      const size_t plane_offset = static_cast<size_t>(plane.offset);
      const size_t map_offset   = plane_offset & ~(page_size - 1);
      const size_t data_offset  = plane_offset - map_offset;
      const size_t map_length   = data_offset + static_cast<size_t>(plane.length);

      void* memory = ::mmap(nullptr,
                            map_length,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            plane.fd.get(),
                            map_offset);
      if (memory == MAP_FAILED) {
        LOG_WARN("Camera framebuffer mmap failed");
        break;
      }

      mapped.planes.push_back(
          {memory, map_length, plane.fd.get(), data_offset, static_cast<size_t>(plane.length)});
    }

    if (mapped.planes.size() != planes.size()) {
      for (auto& plane : mapped.planes) {
        if (plane.addr && plane.addr != MAP_FAILED) {
          ::munmap(plane.addr, plane.size);
        }
      }
      return false;
    }

    mapped_buffers[buffer] = std::move(mapped);
    return true;
  }

  bool create_requests() {
    for (const auto& buffer : allocator->buffers(preview_stream)) {
      if (!map_buffer(buffer.get())) {
        continue;
      }
      preview_buffers.push_back(buffer.get());

      auto request = camera->createRequest();
      if (!request || request->addBuffer(preview_stream, buffer.get()) < 0) {
        LOG_WARN("Camera request creation failed");
        continue;
      }

      apply_request_controls(request.get());
      requests.push_back(std::move(request));
    }

    for (const auto& buffer : allocator->buffers(still_stream)) {
      if (!map_buffer(buffer.get())) {
        continue;
      }
      free_still_buffers.push_back(buffer.get());
    }

    if (requests.empty()) {
      last_error = "No camera requests created";
      LOG_WARN("{}", last_error);
      return false;
    }
    if (free_still_buffers.empty()) {
      last_error = "No still capture buffers created";
      LOG_WARN("{}", last_error);
      return false;
    }

    return true;
  }

  bool start_stream(CameraResolution resolution, unsigned int buffer_count) {
    release_stream_resources();

    if (!configure_stream(resolution, buffer_count)) {
      release_stream_resources();
      return false;
    }

    if (!create_requests()) {
      release_stream_resources();
      return false;
    }

    camera->requestCompleted.connect(this, &Impl::request_complete);

    if (camera->start()) {
      last_error = "Camera start failed";
      LOG_WARN("{}: preview={}x{} still={}x{} buffers={}",
               last_error,
               preview_w,
               preview_h,
               still_w,
               still_h,
               buffer_count);
      camera->requestCompleted.disconnect(this);
      release_stream_resources();
      return false;
    }

    streaming = true;
    for (auto& request : requests) {
      camera->queueRequest(request.get());
    }

    opened = true;
    LOG_INFO(
        "Camera streams started: preview={}x{} stride={} format={} still={}x{} stride={} format={}",
        preview_w,
        preview_h,
        preview_stride,
        preview_format.toString(),
        still_w,
        still_h,
        still_stride,
        still_format.toString());
    return true;
  }

  void detect_focus_capability() {
    focus_capability = {};
    if (!camera) {
      return;
    }

    const libcamera::ControlInfoMap& controls = camera->controls();
    focus_capability.af_mode = controls.find(libcamera::controls::AfMode.id()) != controls.end();
    focus_capability.af_trigger =
        controls.find(libcamera::controls::AfTrigger.id()) != controls.end();
    focus_capability.af_state = controls.find(libcamera::controls::AfState.id()) != controls.end();
    focus_capability.focus_fom =
        controls.find(libcamera::controls::FocusFoM.id()) != controls.end();

    const auto lens_position       = controls.find(libcamera::controls::LensPosition.id());
    focus_capability.lens_position = lens_position != controls.end();
    if (focus_capability.lens_position) {
      focus_capability.lens_position_min = control_info_value_string(lens_position->second.min());
      focus_capability.lens_position_max = control_info_value_string(lens_position->second.max());
      focus_capability.lens_position_default =
          control_info_value_string(lens_position->second.def());
    }

    LOG_INFO(
        "Focus controls: LensPosition={} range=[{},{}] default={} AfMode={} AfTrigger={} "
        "AfState={} FocusFoM={}",
        focus_capability.lens_position,
        focus_capability.lens_position_min,
        focus_capability.lens_position_max,
        focus_capability.lens_position_default,
        focus_capability.af_mode,
        focus_capability.af_trigger,
        focus_capability.af_state,
        focus_capability.focus_fom);
  }

  bool open() {
    manager = std::make_unique<libcamera::CameraManager>();
    if (manager->start()) {
      last_error = "CameraManager start failed";
      LOG_ERROR("{}", last_error);
      return false;
    }

    std::shared_ptr<libcamera::Camera> selected;
    for (const auto& cam : manager->cameras()) {
      std::string model_text = cam->id();
      if (auto model = cam->properties().get(libcamera::properties::Model)) {
        model_text = *model;
      }
      LOG_INFO("Found camera: {}", model_text);

      const std::string lower = lower_string(model_text);
      if (!selected || lower.find("imx219") != std::string::npos) {
        selected = cam;
        if (lower.find("imx219") != std::string::npos) {
          break;
        }
      }
    }

    if (!selected) {
      last_error = "No libcamera camera found. Check libcamera IPA modules and ABI version.";
      LOG_ERROR("{}", last_error);
      return false;
    }

    camera       = selected;
    camera_model = "CardputerZero IMX219";
    if (auto model = camera->properties().get(libcamera::properties::Model)) {
      const std::string model_value(model->begin(), model->end());
      if (!model_value.empty()) {
        camera_model = model_value;
      }
    }
    if (auto sensitivity = camera->properties().get(libcamera::properties::SensorSensitivity)) {
      sensor_sensitivity     = *sensitivity;
      has_sensor_sensitivity = true;
    }
    detect_focus_capability();
    if (camera->acquire()) {
      last_error = "Camera acquire failed";
      LOG_ERROR("{}", last_error);
      camera.reset();
      return false;
    }

    bool started = false;
    for (CameraResolution candidate : capture_resolution_candidates(capture_resolution)) {
      LOG_INFO("Trying camera resolution {}x{}", candidate.width, candidate.height);
      if (start_stream(candidate, kHighResolutionBufferCount)) {
        started = true;
        break;
      }
    }

    if (!started) {
      for (CameraResolution candidate : capture_resolution_candidates({1640, 1232})) {
        LOG_INFO("Trying fallback camera resolution {}x{}", candidate.width, candidate.height);
        if (start_stream(candidate, kFallbackBufferCount)) {
          started = true;
          break;
        }
      }
    }

    if (!started) {
      if (last_error.empty()) {
        last_error = "Camera stream configuration failed";
      }
      LOG_ERROR("{}", last_error);
      close();
      return false;
    }
    return true;
  }

  void close() {
    (void)stop_video_recording();
    if (camera) {
      release_stream_resources();
      camera->release();
      camera.reset();
    }

    if (manager) {
      manager->stop();
      manager.reset();
    }

    opened = false;
  }

  bool consume_frame(CameraFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!new_frame) {
      return false;
    }
    frame     = pending_frame;
    new_frame = false;
    return true;
  }

  bool request_capture() {
    if (!opened || !streaming) {
      return false;
    }

    last_capture_path = make_photo_path();
    {
      std::lock_guard<std::mutex> lock(mutex);
      capture_state = CaptureState::Requested;
    }
    capture_requested = true;
    return true;
  }

  bool start_video_recording(int fps, int quality) {
    if (!opened || !streaming || preview_w <= 0 || preview_h <= 0) {
      video_state = VideoState::Failed;
      last_error  = "Camera preview stream is not ready for video recording";
      return false;
    }
    if (video_writer.is_open()) {
      return true;
    }

    last_video_path_value = make_video_path();
    video_quality         = clamp_int(quality, 1, 100);
    if (!video_writer.open(last_video_path_value, preview_w, preview_h, std::max(1, fps))) {
      video_state = VideoState::Failed;
      return false;
    }
    video_state = VideoState::Recording;
    LOG_INFO("Video recording started: {}", last_video_path_value);
    return true;
  }

  bool stop_video_recording() {
    if (!video_writer.is_open()) {
      return video_state != VideoState::Failed;
    }

    const std::string path = video_writer.path();
    const uint32_t frames  = video_writer.frame_count();
    const bool saved       = video_writer.close() && frames > 0;
    last_video_path_value  = path;
    video_state            = saved ? VideoState::Saved : VideoState::Failed;
    LOG_INFO("Video recording stopped: {} frames={} saved={}", path, frames, saved);
    return saved;
  }

  VideoState consume_video_state(std::string* path) {
    const VideoState state = video_state;
    if (path) {
      *path = last_video_path_value;
    }
    if (video_state == VideoState::Saved || video_state == VideoState::Failed) {
      video_state = VideoState::Idle;
    }
    return state;
  }

  void set_capture_resolution(CameraResolution resolution) {
    capture_resolution.width  = clamp_int(resolution.width, 1, kSensorMaxWidth);
    capture_resolution.height = clamp_int(resolution.height, 1, kSensorMaxHeight);
  }

  void set_zoom_state(CameraZoomState state) {
    std::lock_guard<std::mutex> lock(mutex);
    zoom_state.zoom_percent   = normalize_zoom_percent(state.zoom_percent);
    zoom_state.view_x_percent = clamp_int(state.view_x_percent, 0, 100);
    zoom_state.view_y_percent = clamp_int(state.view_y_percent, 0, 100);
    if (zoom_state.zoom_percent == kMinZoomPercent) {
      zoom_state.view_x_percent = 50;
      zoom_state.view_y_percent = 50;
    }
  }

  CameraZoomState current_zoom_state() {
    std::lock_guard<std::mutex> lock(mutex);
    return zoom_state;
  }

  libcamera::Rectangle scaler_crop_for_zoom_state(const CameraZoomState& state) const {
    const libcamera::Rectangle full =
        scaler_crop_max.isNull() ? libcamera::Rectangle(0, 0, kSensorMaxWidth, kSensorMaxHeight)
                                 : scaler_crop_max;
    const int zoom      = clamp_int(state.zoom_percent, kMinZoomPercent, kMaxZoomPercent);
    unsigned int crop_w = std::max(1u, full.width * 100u / static_cast<unsigned int>(zoom));
    unsigned int crop_h = std::max(1u, full.height * 100u / static_cast<unsigned int>(zoom));

    if (crop_w * 3 > crop_h * 4) {
      crop_w = std::max(1u, crop_h * 4 / 3);
    } else {
      crop_h = std::max(1u, crop_w * 3 / 4);
    }

    crop_w = std::min(crop_w, full.width);
    crop_h = std::min(crop_h, full.height);

    const int max_x  = static_cast<int>(full.width - crop_w);
    const int max_y  = static_cast<int>(full.height - crop_h);
    const int crop_x = full.x + max_x * clamp_int(state.view_x_percent, 0, 100) / 100;
    const int crop_y = full.y + max_y * clamp_int(state.view_y_percent, 0, 100) / 100;
    return {crop_x, crop_y, crop_w, crop_h};
  }

  libcamera::Rectangle full_scaler_crop() const {
    return scaler_crop_max.isNull() ? libcamera::Rectangle(0, 0, kSensorMaxWidth, kSensorMaxHeight)
                                    : scaler_crop_max;
  }

  void apply_request_controls(libcamera::Request* request, bool full_resolution_still = false) {
    if (!request) {
      return;
    }

    if (full_resolution_still) {
      request->controls().set(libcamera::controls::ScalerCrop, full_scaler_crop());
    } else {
      const CameraZoomState state = current_zoom_state();
      request->controls().set(libcamera::controls::ScalerCrop, scaler_crop_for_zoom_state(state));
    }
  }

  ExifMetadata build_still_exif_metadata(const libcamera::Request* request, int width, int height) {
    ExifMetadata metadata         = make_default_exif_metadata(width, height);
    metadata.model                = "CardputerZero IMX219";
    metadata.software             = "Camera 0.2.0";
    metadata.f_number_x100        = 200;
    metadata.focal_length_mm_x100 = 285;
    metadata.lens_make            = "M5Stack";
    metadata.lens_model           = "IMX219_PLCC 1/4 inch 2.85mm F2.0";

    const libcamera::ControlList* request_metadata = request ? &request->metadata() : nullptr;
    const auto exposure_time_us =
        request_metadata ? request_metadata->get(libcamera::controls::ExposureTime) : std::nullopt;
    const auto exposure_time_mode =
        request_metadata ? request_metadata->get(libcamera::controls::ExposureTimeMode)
                         : std::nullopt;
    const auto exposure_value =
        request_metadata ? request_metadata->get(libcamera::controls::ExposureValue) : std::nullopt;
    const auto analogue_gain =
        request_metadata ? request_metadata->get(libcamera::controls::AnalogueGain) : std::nullopt;
    const auto analogue_gain_mode =
        request_metadata ? request_metadata->get(libcamera::controls::AnalogueGainMode)
                         : std::nullopt;
    const auto digital_gain =
        request_metadata ? request_metadata->get(libcamera::controls::DigitalGain) : std::nullopt;
    const auto ae_enable =
        request_metadata ? request_metadata->get(libcamera::controls::AeEnable) : std::nullopt;
    const auto ae_state =
        request_metadata ? request_metadata->get(libcamera::controls::AeState) : std::nullopt;
    const auto ae_metering_mode = request_metadata
                                      ? request_metadata->get(libcamera::controls::AeMeteringMode)
                                      : std::nullopt;
    const auto ae_constraint_mode =
        request_metadata ? request_metadata->get(libcamera::controls::AeConstraintMode)
                         : std::nullopt;
    const auto ae_exposure_mode = request_metadata
                                      ? request_metadata->get(libcamera::controls::AeExposureMode)
                                      : std::nullopt;
    const auto ae_flicker_mode =
        request_metadata ? request_metadata->get(libcamera::controls::AeFlickerMode) : std::nullopt;
    const auto ae_flicker_period = request_metadata
                                       ? request_metadata->get(libcamera::controls::AeFlickerPeriod)
                                       : std::nullopt;
    const auto ae_flicker_detected =
        request_metadata ? request_metadata->get(libcamera::controls::AeFlickerDetected)
                         : std::nullopt;
    const auto lux =
        request_metadata ? request_metadata->get(libcamera::controls::Lux) : std::nullopt;
    const auto brightness =
        request_metadata ? request_metadata->get(libcamera::controls::Brightness) : std::nullopt;
    const auto contrast =
        request_metadata ? request_metadata->get(libcamera::controls::Contrast) : std::nullopt;
    const auto saturation =
        request_metadata ? request_metadata->get(libcamera::controls::Saturation) : std::nullopt;
    const auto sharpness =
        request_metadata ? request_metadata->get(libcamera::controls::Sharpness) : std::nullopt;
    const auto awb_enable =
        request_metadata ? request_metadata->get(libcamera::controls::AwbEnable) : std::nullopt;
    const auto awb_mode =
        request_metadata ? request_metadata->get(libcamera::controls::AwbMode) : std::nullopt;
    const auto awb_locked =
        request_metadata ? request_metadata->get(libcamera::controls::AwbLocked) : std::nullopt;
    const auto colour_gains =
        request_metadata ? request_metadata->get(libcamera::controls::ColourGains) : std::nullopt;
    const auto colour_temperature =
        request_metadata ? request_metadata->get(libcamera::controls::ColourTemperature)
                         : std::nullopt;
    const auto sensor_black_levels =
        request_metadata ? request_metadata->get(libcamera::controls::SensorBlackLevels)
                         : std::nullopt;
    const auto focus_fom =
        request_metadata ? request_metadata->get(libcamera::controls::FocusFoM) : std::nullopt;
    const auto frame_duration_us =
        request_metadata ? request_metadata->get(libcamera::controls::FrameDuration) : std::nullopt;
    const auto frame_duration_limits =
        request_metadata ? request_metadata->get(libcamera::controls::FrameDurationLimits)
                         : std::nullopt;
    const auto sensor_timestamp_ns =
        request_metadata ? request_metadata->get(libcamera::controls::SensorTimestamp)
                         : std::nullopt;
    const auto sensor_temperature =
        request_metadata ? request_metadata->get(libcamera::controls::SensorTemperature)
                         : std::nullopt;
    const auto af_mode =
        request_metadata ? request_metadata->get(libcamera::controls::AfMode) : std::nullopt;
    const auto af_range =
        request_metadata ? request_metadata->get(libcamera::controls::AfRange) : std::nullopt;
    const auto af_speed =
        request_metadata ? request_metadata->get(libcamera::controls::AfSpeed) : std::nullopt;
    const auto af_state =
        request_metadata ? request_metadata->get(libcamera::controls::AfState) : std::nullopt;
    const auto af_pause_state =
        request_metadata ? request_metadata->get(libcamera::controls::AfPauseState) : std::nullopt;
    const auto lens_position =
        request_metadata ? request_metadata->get(libcamera::controls::LensPosition) : std::nullopt;
    const auto hdr_mode =
        request_metadata ? request_metadata->get(libcamera::controls::HdrMode) : std::nullopt;
    const auto hdr_channel =
        request_metadata ? request_metadata->get(libcamera::controls::HdrChannel) : std::nullopt;
    const auto gamma =
        request_metadata ? request_metadata->get(libcamera::controls::Gamma) : std::nullopt;
    const auto frame_wall_clock = request_metadata
                                      ? request_metadata->get(libcamera::controls::FrameWallClock)
                                      : std::nullopt;
    const auto wdr_mode =
        request_metadata ? request_metadata->get(libcamera::controls::WdrMode) : std::nullopt;
    const auto wdr_strength =
        request_metadata ? request_metadata->get(libcamera::controls::WdrStrength) : std::nullopt;
    const auto wdr_max_bright_pixels =
        request_metadata ? request_metadata->get(libcamera::controls::WdrMaxBrightPixels)
                         : std::nullopt;
    const auto lens_dewarp_enable =
        request_metadata ? request_metadata->get(libcamera::controls::LensDewarpEnable)
                         : std::nullopt;
    const auto lens_shading_correction_enable =
        request_metadata ? request_metadata->get(libcamera::controls::LensShadingCorrectionEnable)
                         : std::nullopt;
    const auto awb_state = request_metadata
                               ? request_metadata->get(libcamera::controls::draft::AwbState)
                               : std::nullopt;
    const auto sensor_rolling_shutter_skew =
        request_metadata
            ? request_metadata->get(libcamera::controls::draft::SensorRollingShutterSkew)
            : std::nullopt;
    const auto lens_shading_map_mode =
        request_metadata ? request_metadata->get(libcamera::controls::draft::LensShadingMapMode)
                         : std::nullopt;
    const auto pipeline_depth =
        request_metadata ? request_metadata->get(libcamera::controls::draft::PipelineDepth)
                         : std::nullopt;
    const auto max_latency = request_metadata
                                 ? request_metadata->get(libcamera::controls::draft::MaxLatency)
                                 : std::nullopt;
    const auto noise_reduction_mode =
        request_metadata ? request_metadata->get(libcamera::controls::draft::NoiseReductionMode)
                         : std::nullopt;
    const auto color_correction_aberration_mode =
        request_metadata
            ? request_metadata->get(libcamera::controls::draft::ColorCorrectionAberrationMode)
            : std::nullopt;
    const auto scaler_crop =
        request_metadata ? request_metadata->get(libcamera::controls::ScalerCrop) : std::nullopt;

    if (exposure_time_us) {
      metadata.exposure_time_us = *exposure_time_us;
    }
    if (exposure_value) {
      metadata.exposure_bias_value = static_cast<int32_t>(std::lround(*exposure_value * 100.0f));
    }
    if (ae_metering_mode) {
      metadata.metering_mode = exif_metering_mode(*ae_metering_mode);
    }
    if (awb_mode) {
      metadata.light_source = exif_light_source(*awb_mode);
    }

    if (analogue_gain) {
      const float base_iso     = has_sensor_sensitivity ? sensor_sensitivity : 100.0f;
      const float gain         = *analogue_gain * digital_gain.value_or(1.0f);
      const auto estimated_iso = static_cast<int>(std::lround(std::max(1.0f, base_iso * gain)));
      metadata.iso_speed       = static_cast<uint16_t>(clamp_int(estimated_iso, 1, 65535));
    }

    std::ostringstream json;
    json << '{';
    append_json_string(json, "backend", "libcamera");
    append_json_string(json, "sensor_model", camera_model);
    append_json_string(json, "module_sensor", "IMX219_PLCC");
    append_json_string(json, "module_pixels", "8M");
    append_json_string(json, "lens_type", "1/4 inch");
    append_json_value(json, "lens_focal_length_mm", 2.85f);
    append_json_value(json, "lens_focal_length_tolerance_percent", 5.0f);
    append_json_value(json, "lens_f_number", 2.0f);
    append_json_value(json, "lens_f_number_tolerance_percent", 5.0f);
    append_json_value(json, "lens_field_of_view_degrees", 76.9f);
    append_json_value(json, "lens_distortion_max_percent", 1.5f);
    append_json_value(json, "depth_of_field_near_cm", 10);
    append_json_string(json, "depth_of_field_far", "infinity");
    append_json_value(json, "calibration_distance_cm", 80);
    append_json_bool(json, "focus_lens_position_supported", focus_capability.lens_position);
    append_json_bool(json, "focus_af_mode_supported", focus_capability.af_mode);
    append_json_bool(json, "focus_af_trigger_supported", focus_capability.af_trigger);
    append_json_bool(json, "focus_af_state_supported", focus_capability.af_state);
    append_json_bool(json, "focus_fom_supported", focus_capability.focus_fom);
    append_json_string(json, "focus_lens_position_min", focus_capability.lens_position_min);
    append_json_string(json, "focus_lens_position_max", focus_capability.lens_position_max);
    append_json_string(json, "focus_lens_position_default", focus_capability.lens_position_default);
    append_json_value(json, "exposure_time_us", exposure_time_us);
    append_json_value(json, "exposure_time_mode", exposure_time_mode);
    append_json_value(json, "exposure_value", exposure_value);
    append_json_value(json, "analogue_gain", analogue_gain);
    append_json_value(json, "analogue_gain_mode", analogue_gain_mode);
    append_json_value(json, "digital_gain", digital_gain);
    append_json_value(json, "estimated_iso", metadata.iso_speed);
    append_json_bool(json, "ae_enable", ae_enable);
    append_json_value(json, "ae_state", ae_state);
    append_json_value(json, "ae_metering_mode", ae_metering_mode);
    append_json_value(json, "ae_constraint_mode", ae_constraint_mode);
    append_json_value(json, "ae_exposure_mode", ae_exposure_mode);
    append_json_value(json, "ae_flicker_mode", ae_flicker_mode);
    append_json_value(json, "ae_flicker_period_us", ae_flicker_period);
    append_json_value(json, "ae_flicker_detected", ae_flicker_detected);
    append_json_value(json, "lux", lux);
    append_json_value(json, "brightness", brightness);
    append_json_value(json, "contrast", contrast);
    append_json_value(json, "saturation", saturation);
    append_json_value(json, "sharpness", sharpness);
    append_json_bool(json, "awb_enable", awb_enable);
    append_json_value(json, "awb_mode", awb_mode);
    append_json_bool(json, "awb_locked", awb_locked);
    if (colour_gains) {
      append_json_array(json, "colour_gains", *colour_gains);
    }
    append_json_value(json, "colour_temperature", colour_temperature);
    if (sensor_black_levels) {
      append_json_array(json, "sensor_black_levels", *sensor_black_levels);
    }
    append_json_value(json, "focus_fom", focus_fom);
    append_json_value(json, "frame_duration_us", frame_duration_us);
    if (frame_duration_limits) {
      append_json_array(json, "frame_duration_limits_us", *frame_duration_limits);
    }
    append_json_value(json, "sensor_timestamp_ns", sensor_timestamp_ns);
    append_json_value(json, "sensor_temperature_c", sensor_temperature);
    append_json_value(json, "af_mode", af_mode);
    append_json_value(json, "af_range", af_range);
    append_json_value(json, "af_speed", af_speed);
    append_json_value(json, "af_state", af_state);
    append_json_value(json, "af_pause_state", af_pause_state);
    append_json_value(json, "lens_position", lens_position);
    append_json_value(json, "hdr_mode", hdr_mode);
    append_json_value(json, "hdr_channel", hdr_channel);
    append_json_value(json, "gamma", gamma);
    append_json_value(json, "frame_wall_clock", frame_wall_clock);
    append_json_value(json, "wdr_mode", wdr_mode);
    append_json_value(json, "wdr_strength", wdr_strength);
    append_json_value(json, "wdr_max_bright_pixels", wdr_max_bright_pixels);
    append_json_bool(json, "lens_dewarp_enable", lens_dewarp_enable);
    append_json_bool(json, "lens_shading_correction_enable", lens_shading_correction_enable);
    append_json_value(json, "awb_state", awb_state);
    append_json_value(json, "sensor_rolling_shutter_skew_ns", sensor_rolling_shutter_skew);
    append_json_value(json, "lens_shading_map_mode", lens_shading_map_mode);
    append_json_value(json, "pipeline_depth", pipeline_depth);
    append_json_value(json, "max_latency", max_latency);
    append_json_value(json, "noise_reduction_mode", noise_reduction_mode);
    append_json_value(json, "color_correction_aberration_mode", color_correction_aberration_mode);
    if (auto pixel_array_size = camera->properties().get(libcamera::properties::PixelArraySize)) {
      append_json_size(json, "pixel_array_size", *pixel_array_size);
    }
    if (auto unit_cell_size = camera->properties().get(libcamera::properties::UnitCellSize)) {
      append_json_size(json, "unit_cell_size", *unit_cell_size);
    }
    if (auto sensor_sensitivity_value =
            camera->properties().get(libcamera::properties::SensorSensitivity)) {
      append_json_value(json, "sensor_sensitivity", sensor_sensitivity_value);
    }
    if (auto location = camera->properties().get(libcamera::properties::Location)) {
      append_json_value(json, "camera_location", location);
    }
    if (auto rotation = camera->properties().get(libcamera::properties::Rotation)) {
      append_json_value(json, "camera_rotation", rotation);
    }
    if (auto crop_max = camera->properties().get(libcamera::properties::ScalerCropMaximum)) {
      append_json_rectangle(json, "scaler_crop_maximum", *crop_max);
    }
    if (auto active_areas =
            camera->properties().get(libcamera::properties::PixelArrayActiveAreas)) {
      append_json_rectangles(json, "pixel_array_active_areas", *active_areas);
    }
    if (auto optical_black =
            camera->properties().get(libcamera::properties::PixelArrayOpticalBlackRectangles)) {
      append_json_rectangles(json, "pixel_array_optical_black_rectangles", *optical_black);
    }
    if (scaler_crop) {
      append_json_rectangle(json, "scaler_crop", *scaler_crop);
    }
    json << '}';
    metadata.user_comment = json.str();
    return metadata;
  }

  CaptureState consume_capture_state(std::string* path) {
    std::lock_guard<std::mutex> lock(mutex);
    const CaptureState state = capture_state;
    if (path) {
      *path = last_capture_path;
    }
    if (capture_state == CaptureState::Saved || capture_state == CaptureState::Failed) {
      capture_state = CaptureState::Idle;
    }
    return state;
  }

  void request_complete(libcamera::Request* request) {
    if (!request || request->status() == libcamera::Request::RequestCancelled) {
      return;
    }

    if (!streaming) {
      return;
    }

    const bool has_still = request->buffers().find(still_stream) != request->buffers().end();
    libcamera::FrameBuffer* preview_buffer = request->findBuffer(preview_stream);
    if (!has_still) {
      process_completed_stream_buffer(request,
                                      preview_stream,
                                      preview_w,
                                      preview_h,
                                      preview_stride,
                                      preview_format,
                                      false);
    }
    if (has_still) {
      process_completed_stream_buffer(request,
                                      still_stream,
                                      still_w,
                                      still_h,
                                      still_stride,
                                      still_format,
                                      true);
    }

    request->reuse();
    if (preview_buffer && request->addBuffer(preview_stream, preview_buffer) < 0) {
      LOG_WARN("Failed to re-add preview buffer");
      return;
    }

    bool queue_still_capture = false;
    if (capture_requested.exchange(false) && !free_still_buffers.empty()) {
      libcamera::FrameBuffer* still_buffer = free_still_buffers.back();
      free_still_buffers.pop_back();
      if (request->addBuffer(still_stream, still_buffer) < 0) {
        free_still_buffers.push_back(still_buffer);
        std::lock_guard<std::mutex> lock(mutex);
        capture_state = CaptureState::Failed;
      } else {
        queue_still_capture = true;
      }
    }

    apply_request_controls(request, queue_still_capture);

    if (camera && streaming) {
      camera->queueRequest(request);
    }
  }

  void process_completed_stream_buffer(libcamera::Request* request,
                                       libcamera::Stream* completed_stream,
                                       int width,
                                       int height,
                                       int stride,
                                       const libcamera::PixelFormat& format,
                                       bool is_still) {
    if (!request || !completed_stream) {
      return;
    }

    auto buffer_it = request->buffers().find(completed_stream);
    if (buffer_it == request->buffers().end()) {
      return;
    }
    libcamera::FrameBuffer* buffer = buffer_it->second;
    auto map_it                    = mapped_buffers.find(buffer);
    if (map_it == mapped_buffers.end()) {
      return;
    }

    const auto& mapped = map_it->second;
    std::vector<const uint8_t*> plane_data;
    std::vector<size_t> bytes_used;
    plane_data.reserve(mapped.planes.size());
    bytes_used.reserve(mapped.planes.size());
    for (const auto& plane : mapped.planes) {
      plane_data.push_back(static_cast<const uint8_t*>(plane.addr) + plane.data_offset);
      bytes_used.push_back(plane.data_size);
    }

    const auto& metadata = buffer->metadata();
    if (!metadata.planes().empty()) {
      const size_t plane_count = std::min(metadata.planes().size(), bytes_used.size());
      for (size_t i = 0; i < plane_count; ++i) {
        if (metadata.planes()[i].bytesused > 0) {
          bytes_used[i] =
              std::min(bytes_used[i], static_cast<size_t>(metadata.planes()[i].bytesused));
        }
      }
    }

    for (const auto& plane : mapped.planes) {
      sync_dma_buf(plane.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    }

    {
      std::lock_guard<std::mutex> lock(mutex);
      const bool converted = convert_frame_to_outputs(plane_data,
                                                      bytes_used,
                                                      width,
                                                      height,
                                                      stride,
                                                      map_libcamera_format(format),
                                                      is_still,
                                                      is_still ? nullptr : &pending_frame,
                                                      is_still ? &still_rgb : nullptr);
      if (converted && is_still) {
        const ExifMetadata exif_metadata = build_still_exif_metadata(request, width, height);
        const bool saved =
            save_jpeg_rgb888(last_capture_path, still_rgb, width, height, 92, &exif_metadata);
        capture_state = saved ? CaptureState::Saved : CaptureState::Failed;
      } else if (converted) {
        pending_frame.width  = width;
        pending_frame.height = height;
        new_frame            = true;
        if (video_writer.is_open() &&
            !video_writer.write_rgb565_frame(pending_frame, video_quality)) {
          video_state = VideoState::Failed;
          (void)video_writer.close();
        }
      }
    }

    for (const auto& plane : mapped.planes) {
      sync_dma_buf(plane.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    }

    if (is_still) {
      free_still_buffers.push_back(buffer);
    }
  }
#endif
};

LibcameraBackend::LibcameraBackend()
    : impl_(std::make_unique<Impl>()) {}

LibcameraBackend::~LibcameraBackend() { close(); }

bool LibcameraBackend::open() {
#if USE_DESKTOP
  return false;
#else
  return impl_->open();
#endif
}

void LibcameraBackend::close() {
#if !USE_DESKTOP
  if (impl_) {
    impl_->close();
  }
#endif
}

bool LibcameraBackend::consume_frame(CameraFrame& frame) {
#if USE_DESKTOP
  (void)frame;
  return false;
#else
  return impl_->consume_frame(frame);
#endif
}

bool LibcameraBackend::request_capture() {
#if USE_DESKTOP
  return false;
#else
  return impl_->request_capture();
#endif
}

bool LibcameraBackend::start_video_recording(int fps, int quality) {
#if USE_DESKTOP
  (void)fps;
  (void)quality;
  return false;
#else
  return impl_->start_video_recording(fps, quality);
#endif
}

bool LibcameraBackend::stop_video_recording() {
#if USE_DESKTOP
  return false;
#else
  return impl_->stop_video_recording();
#endif
}

void LibcameraBackend::set_capture_resolution(CameraResolution resolution) {
#if !USE_DESKTOP
  impl_->set_capture_resolution(resolution);
#else
  (void)resolution;
#endif
}

void LibcameraBackend::set_zoom_state(CameraZoomState state) {
#if !USE_DESKTOP
  impl_->set_zoom_state(state);
#else
  (void)state;
#endif
}

CaptureState LibcameraBackend::consume_capture_state(std::string* path) {
#if USE_DESKTOP
  if (path) {
    path->clear();
  }
  return CaptureState::Idle;
#else
  return impl_->consume_capture_state(path);
#endif
}

VideoState LibcameraBackend::consume_video_state(std::string* path) {
#if USE_DESKTOP
  if (path) {
    path->clear();
  }
  return VideoState::Idle;
#else
  return impl_->consume_video_state(path);
#endif
}

std::string LibcameraBackend::last_capture_path() const {
#if USE_DESKTOP
  return {};
#else
  return impl_->last_capture_path;
#endif
}

std::string LibcameraBackend::last_video_path() const {
#if USE_DESKTOP
  return {};
#else
  return impl_->last_video_path_value;
#endif
}

std::string LibcameraBackend::last_error() const {
#if USE_DESKTOP
  return "libcamera backend unavailable in desktop build";
#else
  return impl_->last_error;
#endif
}

}  // namespace service
