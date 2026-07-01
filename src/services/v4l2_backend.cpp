#include "services/v4l2_backend.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "services/camera_backend_utils.h"
#include "services/video_recorder.h"
#include "utils/logger.h"

#if !USE_DESKTOP
#include <fcntl.h>
#include <jpeglib.h>
#include <libv4l2.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace service {
namespace {

using namespace camera_backend;

#if !USE_DESKTOP
struct Buffer {
  void* start{nullptr};
  size_t length{0};
};

std::vector<std::string> device_candidates() {
  std::vector<std::string> devices;
  auto add_unique = [&devices](std::string device) {
    if (device.empty()) {
      return;
    }
    if (std::find(devices.begin(), devices.end(), device) == devices.end()) {
      devices.push_back(std::move(device));
    }
  };

  if (const char* env_device = std::getenv("CAMERA_APP_V4L2_DEVICE")) {
    add_unique(env_device);
  }
  if (const char* env_device = std::getenv("V4L2_CAMERA_DEVICE")) {
    add_unique(env_device);
  }
  for (int i = 0; i < 8; ++i) {
    add_unique("/dev/video" + std::to_string(i));
  }
  return devices;
}

bool ioctl_retry(int fd, unsigned long request, void* arg) {
  int ret = 0;
  do {
    ret = v4l2_ioctl(fd, request, arg);
  } while (ret < 0 && errno == EINTR);
  return ret == 0;
}

bool has_capture_capability(int fd) {
  v4l2_capability cap{};
  if (!ioctl_retry(fd, VIDIOC_QUERYCAP, &cap)) {
    return false;
  }

  const uint32_t caps =
      (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
}

bool decompress_mjpeg(const uint8_t* data,
                      size_t size,
                      std::vector<uint8_t>& rgb,
                      int& width,
                      int& height) {
  if (!data || size == 0) {
    return false;
  }

  FILE* input = fmemopen(const_cast<uint8_t*>(data), size, "rb");
  if (!input) {
    return false;
  }

  jpeg_decompress_struct dinfo{};
  jpeg_error_mgr jerr{};
  dinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&dinfo);
  jpeg_stdio_src(&dinfo, input);
  if (jpeg_read_header(&dinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&dinfo);
    std::fclose(input);
    return false;
  }

  dinfo.out_color_space = JCS_RGB;
  if (!jpeg_start_decompress(&dinfo)) {
    jpeg_destroy_decompress(&dinfo);
    std::fclose(input);
    return false;
  }

  width  = static_cast<int>(dinfo.output_width);
  height = static_cast<int>(dinfo.output_height);
  if (width <= 0 || height <= 0) {
    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);
    std::fclose(input);
    return false;
  }

  rgb.assign(static_cast<size_t>(width) * height * 3, 0);
  const int stride = width * 3;
  while (dinfo.output_scanline < dinfo.output_height) {
    JSAMPROW row = rgb.data() + static_cast<size_t>(dinfo.output_scanline) * stride;
    jpeg_read_scanlines(&dinfo, &row, 1);
  }

  jpeg_finish_decompress(&dinfo);
  jpeg_destroy_decompress(&dinfo);
  std::fclose(input);
  return true;
}

bool rgb888_to_frame(const std::vector<uint8_t>& rgb, int width, int height, CameraFrame& frame) {
  if (width <= 0 || height <= 0 || rgb.size() < static_cast<size_t>(width * height * 3)) {
    return false;
  }
  frame.width  = width;
  frame.height = height;
  frame.rgb565.assign(static_cast<size_t>(width) * height, 0);
  for (int y = 0; y < height; ++y) {
    const int dst_y = height - 1 - y;
    for (int x = 0; x < width; ++x) {
      const int dst_x       = width - 1 - x;
      const size_t src_idx  = static_cast<size_t>(y * width + x) * 3;
      const int dst_idx     = dst_y * width + dst_x;
      frame.rgb565[dst_idx] = rgb888_to_rgb565(rgb[src_idx], rgb[src_idx + 1], rgb[src_idx + 2]);
    }
  }
  return true;
}
#endif

}  // namespace

struct V4l2Backend::Impl {
#if !USE_DESKTOP
  int fd{-1};
  std::string device;
  std::vector<Buffer> buffers;
  CameraResolution capture_resolution{kDefaultCaptureWidth, kDefaultCaptureHeight};
  CameraZoomState zoom_state{};
  CameraFrame latest_frame;
  bool new_frame{false};
  std::vector<uint8_t> latest_rgb;
  CaptureState capture_state{CaptureState::Idle};
  VideoState video_state{VideoState::Idle};
  std::string last_capture_path_value;
  std::string last_video_path_value;
  std::string last_error_value;
  MjpegAviWriter video_writer;
  int video_quality{80};
  uint32_t pixel_format{V4L2_PIX_FMT_YUYV};
  int width{kPreviewWidth};
  int height{kPreviewHeight};
  int stride{kPreviewWidth * 2};
  bool opened{false};

  bool open() {
    for (const auto& candidate : device_candidates()) {
      last_error_value.clear();
      if (open_device(candidate)) {
        return true;
      }
    }
    if (last_error_value.empty()) {
      last_error_value = "No V4L2 USB camera found";
    }
    return false;
  }

  bool open_device(const std::string& path) {
    fd = v4l2_open(path.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
      last_error_value = path + ": " + std::strerror(errno);
      return false;
    }

    if (!has_capture_capability(fd)) {
      last_error_value = path + ": not a streaming capture device";
      close();
      return false;
    }

    device = path;
    if (!configure_format()) {
      close();
      return false;
    }
    if (!init_mmap()) {
      close();
      return false;
    }
    if (!start_streaming()) {
      close();
      return false;
    }

    opened = true;
    LOG_INFO("V4L2 camera started: {} {}x{} stride={} format={}",
             device,
             width,
             height,
             stride,
             pixel_format == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV");
    return true;
  }

  bool configure_format() {
    constexpr std::array<uint32_t, 2> formats{V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV};
    for (uint32_t fmt : formats) {
      v4l2_format format{};
      format.type          = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      format.fmt.pix.width = static_cast<uint32_t>(
          capture_resolution.width > 0 ? capture_resolution.width : kDefaultCaptureWidth);
      format.fmt.pix.height = static_cast<uint32_t>(
          capture_resolution.height > 0 ? capture_resolution.height : kDefaultCaptureHeight);
      format.fmt.pix.pixelformat = fmt;
      format.fmt.pix.field       = V4L2_FIELD_ANY;

      if (!ioctl_retry(fd, VIDIOC_S_FMT, &format)) {
        continue;
      }

      if (format.fmt.pix.pixelformat != fmt) {
        continue;
      }

      pixel_format = fmt;
      width        = static_cast<int>(format.fmt.pix.width);
      height       = static_cast<int>(format.fmt.pix.height);
      stride       = static_cast<int>(format.fmt.pix.bytesperline);
      if (stride <= 0) {
        stride = pixel_format == V4L2_PIX_FMT_YUYV ? width * 2 : width;
      }
      return true;
    }

    last_error_value = device + ": no supported V4L2 format (MJPEG/YUYV)";
    return false;
  }

  bool init_mmap() {
    v4l2_requestbuffers req{};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (!ioctl_retry(fd, VIDIOC_REQBUFS, &req) || req.count < 2) {
      last_error_value = device + ": failed to request V4L2 buffers";
      return false;
    }

    buffers.assign(req.count, {});
    for (uint32_t i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;
      if (!ioctl_retry(fd, VIDIOC_QUERYBUF, &buf)) {
        last_error_value = device + ": failed to query V4L2 buffer";
        return false;
      }

      buffers[i].length = buf.length;
      buffers[i].start =
          v4l2_mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
      if (buffers[i].start == MAP_FAILED) {
        buffers[i].start = nullptr;
        last_error_value = device + ": failed to mmap V4L2 buffer";
        return false;
      }
    }
    return true;
  }

  bool start_streaming() {
    for (uint32_t i = 0; i < buffers.size(); ++i) {
      v4l2_buffer buf{};
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = i;
      if (!ioctl_retry(fd, VIDIOC_QBUF, &buf)) {
        last_error_value = device + ": failed to queue V4L2 buffer";
        return false;
      }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!ioctl_retry(fd, VIDIOC_STREAMON, &type)) {
      last_error_value = device + ": failed to start V4L2 stream";
      return false;
    }
    return true;
  }

  void close() {
    (void)stop_video_recording();
    if (fd >= 0 && opened) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      (void)v4l2_ioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    opened = false;

    for (auto& buffer : buffers) {
      if (buffer.start) {
        v4l2_munmap(buffer.start, buffer.length);
      }
    }
    buffers.clear();

    if (fd >= 0) {
      v4l2_close(fd);
      fd = -1;
    }
  }

  bool consume_frame(CameraFrame& frame) {
    pollfd pfd{};
    pfd.fd                = fd;
    pfd.events            = POLLIN;
    const int poll_result = poll(&pfd, 1, 0);
    if (poll_result <= 0) {
      if (new_frame) {
        frame     = latest_frame;
        new_frame = false;
        return true;
      }
      return false;
    }

    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (!ioctl_retry(fd, VIDIOC_DQBUF, &buf)) {
      if (errno != EAGAIN) {
        LOG_WARN("Failed to dequeue V4L2 buffer: {}", std::strerror(errno));
      }
      return false;
    }

    const bool converted = convert_buffer(buf);
    (void)ioctl_retry(fd, VIDIOC_QBUF, &buf);

    if (!converted) {
      return false;
    }
    frame     = latest_frame;
    new_frame = false;
    return true;
  }

  bool convert_buffer(const v4l2_buffer& buf) {
    if (buf.index >= buffers.size() || !buffers[buf.index].start || buf.bytesused == 0) {
      return false;
    }

    const auto* data = static_cast<const uint8_t*>(buffers[buf.index].start);
    if (pixel_format == V4L2_PIX_FMT_MJPEG) {
      int jpeg_w = 0;
      int jpeg_h = 0;
      if (!decompress_mjpeg(data, buf.bytesused, latest_rgb, jpeg_w, jpeg_h)) {
        return false;
      }
      width  = jpeg_w;
      height = jpeg_h;
      if (!rgb888_to_frame(latest_rgb, width, height, latest_frame)) {
        return false;
      }
      write_video_frame();
      return true;
    }

    CameraFrame frame;
    frame.width  = width;
    frame.height = height;
    frame.rgb565.assign(static_cast<size_t>(width) * height, 0);
    std::vector<const uint8_t*> planes{data};
    std::vector<size_t> bytes_used{static_cast<size_t>(buf.bytesused)};
    if (!convert_frame_to_outputs(planes,
                                  bytes_used,
                                  width,
                                  height,
                                  stride,
                                  PixelFormat::YUYV,
                                  false,
                                  &frame,
                                  nullptr)) {
      return false;
    }
    latest_frame = std::move(frame);
    latest_rgb.assign(static_cast<size_t>(width) * height * 3, 0);
    for (int i = 0; i < width * height; ++i) {
      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;
      rgb565_to_rgb888(latest_frame.rgb565[i], r, g, b);
      latest_rgb[static_cast<size_t>(i) * 3]     = r;
      latest_rgb[static_cast<size_t>(i) * 3 + 1] = g;
      latest_rgb[static_cast<size_t>(i) * 3 + 2] = b;
    }
    write_video_frame();
    return true;
  }

  void write_video_frame() {
    if (!video_writer.is_open()) {
      return;
    }
    if (!latest_rgb.empty() &&
        !video_writer.write_rgb888_frame(latest_rgb, width, height, video_quality)) {
      video_state = VideoState::Failed;
      (void)video_writer.close();
    }
  }

  bool request_capture() {
    if (latest_rgb.empty() || width <= 0 || height <= 0) {
      CameraFrame frame;
      (void)consume_frame(frame);
    }
    if (latest_rgb.empty() || width <= 0 || height <= 0) {
      capture_state = CaptureState::Failed;
      return false;
    }

    last_capture_path_value = make_photo_path();
    capture_state = save_jpeg_rgb888(last_capture_path_value, latest_rgb, width, height, 92)
                        ? CaptureState::Saved
                        : CaptureState::Failed;
    return capture_state == CaptureState::Saved;
  }

  bool start_video_recording(int fps, int quality) {
    if (!opened || width <= 0 || height <= 0) {
      video_state      = VideoState::Failed;
      last_error_value = "V4L2 stream is not ready for video recording";
      return false;
    }
    if (video_writer.is_open()) {
      return true;
    }

    last_video_path_value = make_video_path();
    video_quality         = clamp_int(quality, 1, 100);
    if (!video_writer.open(last_video_path_value, width, height, std::max(1, fps))) {
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

  void set_capture_resolution(CameraResolution resolution) {
    capture_resolution.width  = clamp_int(resolution.width, 1, kSensorMaxWidth);
    capture_resolution.height = clamp_int(resolution.height, 1, kSensorMaxHeight);
  }

  void set_zoom_state(CameraZoomState state) {
    zoom_state.zoom_percent   = normalize_zoom_percent(state.zoom_percent);
    zoom_state.view_x_percent = clamp_int(state.view_x_percent, 0, 100);
    zoom_state.view_y_percent = clamp_int(state.view_y_percent, 0, 100);
    if (zoom_state.zoom_percent == kMinZoomPercent) {
      zoom_state.view_x_percent = 50;
      zoom_state.view_y_percent = 50;
    }
  }

  CaptureState consume_capture_state(std::string* path) {
    const CaptureState state = capture_state;
    if (path) {
      *path = last_capture_path_value;
    }
    if (capture_state == CaptureState::Saved || capture_state == CaptureState::Failed) {
      capture_state = CaptureState::Idle;
    }
    return state;
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
#endif
};

V4l2Backend::V4l2Backend()
    : impl_(std::make_unique<Impl>()) {}

V4l2Backend::~V4l2Backend() { close(); }

bool V4l2Backend::open() {
#if USE_DESKTOP
  return false;
#else
  return impl_->open();
#endif
}

void V4l2Backend::close() {
#if !USE_DESKTOP
  if (impl_) {
    impl_->close();
  }
#endif
}

bool V4l2Backend::consume_frame(CameraFrame& frame) {
#if USE_DESKTOP
  (void)frame;
  return false;
#else
  return impl_->consume_frame(frame);
#endif
}

bool V4l2Backend::request_capture() {
#if USE_DESKTOP
  return false;
#else
  return impl_->request_capture();
#endif
}

bool V4l2Backend::start_video_recording(int fps, int quality) {
#if USE_DESKTOP
  (void)fps;
  (void)quality;
  return false;
#else
  return impl_->start_video_recording(fps, quality);
#endif
}

bool V4l2Backend::stop_video_recording() {
#if USE_DESKTOP
  return false;
#else
  return impl_->stop_video_recording();
#endif
}

void V4l2Backend::set_capture_resolution(CameraResolution resolution) {
#if !USE_DESKTOP
  impl_->set_capture_resolution(resolution);
#else
  (void)resolution;
#endif
}

void V4l2Backend::set_zoom_state(CameraZoomState state) {
#if !USE_DESKTOP
  impl_->set_zoom_state(state);
#else
  (void)state;
#endif
}

CaptureState V4l2Backend::consume_capture_state(std::string* path) {
#if USE_DESKTOP
  if (path) {
    path->clear();
  }
  return CaptureState::Idle;
#else
  return impl_->consume_capture_state(path);
#endif
}

VideoState V4l2Backend::consume_video_state(std::string* path) {
#if USE_DESKTOP
  if (path) {
    path->clear();
  }
  return VideoState::Idle;
#else
  return impl_->consume_video_state(path);
#endif
}

std::string V4l2Backend::last_capture_path() const {
#if USE_DESKTOP
  return {};
#else
  return impl_->last_capture_path_value;
#endif
}

std::string V4l2Backend::last_video_path() const {
#if USE_DESKTOP
  return {};
#else
  return impl_->last_video_path_value;
#endif
}

std::string V4l2Backend::last_error() const {
#if USE_DESKTOP
  return "V4L2 backend unavailable in desktop build";
#else
  return impl_->last_error_value;
#endif
}

}  // namespace service
