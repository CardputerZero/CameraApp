#include "services/camera_service.h"

#include "utils/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <utility>

#if !USE_DESKTOP
#include <fcntl.h>
#include <jpeglib.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/control_ids.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/formats.h>
#include <libcamera/property_ids.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include <map>
#include <vector>
#endif

namespace service {
namespace {

constexpr int kSensorMaxWidth = 3280;
constexpr int kSensorMaxHeight = 2464;
constexpr int kDefaultCaptureWidth = 1640;
constexpr int kDefaultCaptureHeight = 1232;
constexpr int kPreviewWidth = 226;
constexpr int kPreviewHeight = 170;
constexpr int kMinZoomPercent = 100;
constexpr int kMidZoomPercent = 250;
constexpr int kMaxZoomPercent = 500;
constexpr int kPanStepPercent = 8;
constexpr unsigned int kHighResolutionBufferCount = 4;
constexpr unsigned int kFallbackBufferCount = 3;

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) |
                                 ((g & 0xFC) << 3) |
                                 (b >> 3));
}

#if !USE_DESKTOP
void rgb565_to_rgb888(uint16_t p, uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = static_cast<uint8_t>(((p >> 11) & 0x1F) << 3);
    g = static_cast<uint8_t>(((p >> 5) & 0x3F) << 2);
    b = static_cast<uint8_t>((p & 0x1F) << 3);
    r |= r >> 5;
    g |= g >> 6;
    b |= b >> 5;
}

std::string lower_string(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void sync_dma_buf(int fd, uint64_t flags)
{
    if (fd < 0) {
        return;
    }

    struct dma_buf_sync sync { flags };
    (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

std::string pictures_dir()
{
    const char* home = std::getenv("HOME");
    std::string base = (home && home[0]) ? home : "/home/pi";
    return base + "/Pictures/DCIM/Camera";
}

bool ensure_dir(const std::string& dir)
{
    std::string current;
    if (!dir.empty() && dir[0] == '/') {
        current = "/";
    }

    size_t start = current == "/" ? 1 : 0;
    while (start <= dir.size()) {
        const size_t slash = dir.find('/', start);
        const std::string part = dir.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (current.size() > 1) {
                current += "/";
            }
            current += part;

            struct stat st {};
            if (::stat(current.c_str(), &st) != 0) {
                if (::mkdir(current.c_str(), 0777) != 0) {
                    return false;
                }
            }
            (void)::chmod(current.c_str(), 0777);
        }

        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    return true;
}

std::string make_photo_path()
{
    const std::string dir = pictures_dir();
    (void)ensure_dir(dir);

    std::time_t now = std::time(nullptr);
    std::tm tm_now {};
    localtime_r(&now, &tm_now);

    char time_buf[64] {};
    std::strftime(time_buf, sizeof(time_buf), "%Y%m%d_%H%M%S", &tm_now);

    char path[512] {};
    std::snprintf(path, sizeof(path), "%s/CAM_%s.jpg", dir.c_str(), time_buf);
    return path;
}

bool save_jpeg_rgb888(const std::string& path,
                      const std::vector<uint8_t>& rgb,
                      int width,
                      int height,
                      int quality = 90)
{
    if (rgb.size() < static_cast<size_t>(width * height * 3)) {
        return false;
    }

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("Failed to open jpeg file: {}", path);
        return false;
    }

    jpeg_compress_struct cinfo {};
    jpeg_error_mgr jerr {};
    cinfo.err = jpeg_std_error(&jerr);

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = static_cast<JDIMENSION>(width);
    cinfo.image_height = static_cast<JDIMENSION>(height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row_pointer[1];
        row_pointer[0] = const_cast<JSAMPROW>(&rgb[cinfo.next_scanline * width * 3]);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::fclose(fp);
    (void)::chmod(path.c_str(), 0666);
    return true;
}

int clamp_int(int value, int min_value, int max_value)
{
    return std::max(min_value, std::min(value, max_value));
}

int normalize_zoom_percent(int zoom_percent)
{
    if (zoom_percent <= kMinZoomPercent) {
        return kMinZoomPercent;
    }
    if (zoom_percent <= kMidZoomPercent) {
        return kMidZoomPercent;
    }
    return kMaxZoomPercent;
}

std::vector<CameraResolution> capture_resolution_candidates(CameraResolution preferred)
{
    std::vector<CameraResolution> candidates;
    auto add_unique = [&candidates](CameraResolution resolution) {
        if (resolution.width <= 0 || resolution.height <= 0) {
            return;
        }
        const auto exists = std::find_if(candidates.begin(), candidates.end(), [resolution](const CameraResolution& item) {
            return item.width == resolution.width && item.height == resolution.height;
        });
        if (exists == candidates.end()) {
            candidates.push_back(resolution);
        }
    };

    add_unique(preferred);
    add_unique({kDefaultCaptureWidth, kDefaultCaptureHeight});
    add_unique({1920, 1080});
    add_unique({1280, 960});
    add_unique({640, 480});
    return candidates;
}
#endif

} // namespace

struct CameraService::Impl {
#if !USE_DESKTOP
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
    std::string last_capture_path;
    std::string last_error;
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

    bool configure_stream(CameraResolution resolution, unsigned int buffer_count)
    {
        config = camera->generateConfiguration({libcamera::StreamRole::Viewfinder,
                                                libcamera::StreamRole::StillCapture});
        if (!config || config->size() < 2) {
            last_error = "Camera configuration generation failed";
            LOG_ERROR("{}", last_error);
            return false;
        }

        libcamera::StreamConfiguration& preview_cfg = config->at(0);
        preview_cfg.size.width = kPreviewWidth;
        preview_cfg.size.height = kPreviewHeight;
        preview_cfg.pixelFormat = libcamera::formats::RGB565;
        preview_cfg.bufferCount = buffer_count;

        libcamera::StreamConfiguration& still_cfg = config->at(1);
        still_cfg.size.width = static_cast<unsigned int>(resolution.width);
        still_cfg.size.height = static_cast<unsigned int>(resolution.height);
        still_cfg.pixelFormat = libcamera::formats::YUV420;
        still_cfg.bufferCount = 1;

        if (config->validate() == libcamera::CameraConfiguration::Invalid) {
            last_error = "Invalid camera configuration";
            LOG_WARN("{}: {}x{} buffers={}", last_error, resolution.width, resolution.height, buffer_count);
            return false;
        }

        if (camera->configure(config.get())) {
            last_error = "Camera configure failed";
            LOG_WARN("{}: {}x{} buffers={}", last_error, resolution.width, resolution.height, buffer_count);
            return false;
        }

        libcamera::StreamConfiguration& active_preview_cfg = config->at(0);
        libcamera::StreamConfiguration& active_still_cfg = config->at(1);
        if (!is_supported(active_preview_cfg.pixelFormat) || !is_supported(active_still_cfg.pixelFormat)) {
            last_error = "Unsupported camera stream format: preview=" +
                         active_preview_cfg.pixelFormat.toString() +
                         " still=" +
                         active_still_cfg.pixelFormat.toString();
            LOG_WARN("{}", last_error);
            return false;
        }

        preview_stream = active_preview_cfg.stream();
        still_stream = active_still_cfg.stream();
        preview_w = static_cast<int>(active_preview_cfg.size.width);
        preview_h = static_cast<int>(active_preview_cfg.size.height);
        preview_stride = static_cast<int>(active_preview_cfg.stride);
        preview_format = active_preview_cfg.pixelFormat;
        still_w = static_cast<int>(active_still_cfg.size.width);
        still_h = static_cast<int>(active_still_cfg.size.height);
        still_stride = static_cast<int>(active_still_cfg.stride);
        still_format = active_still_cfg.pixelFormat;

        const auto crop_max = camera->properties().get(libcamera::properties::ScalerCropMaximum);
        scaler_crop_max = crop_max ? *crop_max : libcamera::Rectangle(0, 0, kSensorMaxWidth, kSensorMaxHeight);

        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_frame.width = kPreviewWidth;
            pending_frame.height = kPreviewHeight;
            pending_frame.rgb565.assign(kPreviewWidth * kPreviewHeight, 0);
            still_rgb.assign(still_w * still_h * 3, 0);
        }

        allocator = std::make_unique<libcamera::FrameBufferAllocator>(camera);
        const int preview_allocated = allocator->allocate(preview_stream);
        const int still_allocated = allocator->allocate(still_stream);
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

    void release_stream_resources()
    {
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
        still_stream = nullptr;
    }

    bool map_buffer(const libcamera::FrameBuffer* buffer)
    {
        const auto planes = buffer->planes();
        if (planes.empty()) {
            return false;
        }

        MappedBuffer mapped;
        for (const auto& plane : planes) {
            const long page_size_value = ::sysconf(_SC_PAGE_SIZE);
            const size_t page_size = page_size_value > 0 ? static_cast<size_t>(page_size_value) : 4096;
            const size_t plane_offset = static_cast<size_t>(plane.offset);
            const size_t map_offset = plane_offset & ~(page_size - 1);
            const size_t data_offset = plane_offset - map_offset;
            const size_t map_length = data_offset + static_cast<size_t>(plane.length);

            void* memory = ::mmap(nullptr, map_length, PROT_READ | PROT_WRITE, MAP_SHARED, plane.fd.get(), map_offset);
            if (memory == MAP_FAILED) {
                LOG_WARN("Camera framebuffer mmap failed");
                break;
            }

            mapped.planes.push_back({memory,
                                     map_length,
                                     plane.fd.get(),
                                     data_offset,
                                     static_cast<size_t>(plane.length)});
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

    bool create_requests()
    {
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

    bool start_stream(CameraResolution resolution, unsigned int buffer_count)
    {
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
        LOG_INFO("Camera streams started: preview={}x{} stride={} format={} still={}x{} stride={} format={}",
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

    bool open()
    {
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

        camera = selected;
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

    void close()
    {
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

    bool consume_frame(CameraFrame& frame)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!new_frame) {
            return false;
        }
        frame = pending_frame;
        new_frame = false;
        return true;
    }

    bool request_capture()
    {
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

    void set_capture_resolution(CameraResolution resolution)
    {
        capture_resolution.width = clamp_int(resolution.width, 1, kSensorMaxWidth);
        capture_resolution.height = clamp_int(resolution.height, 1, kSensorMaxHeight);
    }

    void set_zoom_state(CameraZoomState state)
    {
        std::lock_guard<std::mutex> lock(mutex);
        zoom_state.zoom_percent = normalize_zoom_percent(state.zoom_percent);
        zoom_state.view_x_percent = clamp_int(state.view_x_percent, 0, 100);
        zoom_state.view_y_percent = clamp_int(state.view_y_percent, 0, 100);
        if (zoom_state.zoom_percent == kMinZoomPercent) {
            zoom_state.view_x_percent = 50;
            zoom_state.view_y_percent = 50;
        }
    }

    CameraZoomState current_zoom_state()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return zoom_state;
    }

    libcamera::Rectangle scaler_crop_for_zoom_state(const CameraZoomState& state) const
    {
        const libcamera::Rectangle full = scaler_crop_max.isNull()
            ? libcamera::Rectangle(0, 0, kSensorMaxWidth, kSensorMaxHeight)
            : scaler_crop_max;
        const int zoom = clamp_int(state.zoom_percent, kMinZoomPercent, kMaxZoomPercent);
        unsigned int crop_w = std::max(1u, full.width * 100u / static_cast<unsigned int>(zoom));
        unsigned int crop_h = std::max(1u, full.height * 100u / static_cast<unsigned int>(zoom));

        if (crop_w * 3 > crop_h * 4) {
            crop_w = std::max(1u, crop_h * 4 / 3);
        } else {
            crop_h = std::max(1u, crop_w * 3 / 4);
        }

        crop_w = std::min(crop_w, full.width);
        crop_h = std::min(crop_h, full.height);

        const int max_x = static_cast<int>(full.width - crop_w);
        const int max_y = static_cast<int>(full.height - crop_h);
        const int crop_x = full.x + max_x * clamp_int(state.view_x_percent, 0, 100) / 100;
        const int crop_y = full.y + max_y * clamp_int(state.view_y_percent, 0, 100) / 100;
        return {crop_x, crop_y, crop_w, crop_h};
    }

    void apply_request_controls(libcamera::Request* request)
    {
        if (!request) {
            return;
        }

        const CameraZoomState state = current_zoom_state();
        request->controls().set(libcamera::controls::ScalerCrop, scaler_crop_for_zoom_state(state));
    }

    CaptureState consume_capture_state(std::string* path)
    {
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

    void request_complete(libcamera::Request* request)
    {
        if (!request || request->status() == libcamera::Request::RequestCancelled) {
            return;
        }

        if (!streaming) {
            return;
        }

        const bool has_still = request->buffers().find(still_stream) != request->buffers().end();
        libcamera::FrameBuffer* preview_buffer = request->findBuffer(preview_stream);
        process_completed_stream_buffer(request, preview_stream, preview_w, preview_h, preview_stride, preview_format, false);
        if (has_still) {
            process_completed_stream_buffer(request, still_stream, still_w, still_h, still_stride, still_format, true);
        }

        request->reuse();
        if (preview_buffer && request->addBuffer(preview_stream, preview_buffer) < 0) {
            LOG_WARN("Failed to re-add preview buffer");
            return;
        }

        if (capture_requested.exchange(false) && !free_still_buffers.empty()) {
            libcamera::FrameBuffer* still_buffer = free_still_buffers.back();
            free_still_buffers.pop_back();
            if (request->addBuffer(still_stream, still_buffer) < 0) {
                free_still_buffers.push_back(still_buffer);
                std::lock_guard<std::mutex> lock(mutex);
                capture_state = CaptureState::Failed;
            }
        }

        apply_request_controls(request);

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
                                         bool is_still)
    {
        if (!request || !completed_stream) {
            return;
        }

        auto buffer_it = request->buffers().find(completed_stream);
        if (buffer_it == request->buffers().end()) {
            return;
        }
        libcamera::FrameBuffer* buffer = buffer_it->second;
        auto map_it = mapped_buffers.find(buffer);
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
                    bytes_used[i] = std::min(bytes_used[i], static_cast<size_t>(metadata.planes()[i].bytesused));
                }
            }
        }

        for (const auto& plane : mapped.planes) {
            sync_dma_buf(plane.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        }
        convert_frame(plane_data, bytes_used, width, height, stride, format, is_still);
        for (const auto& plane : mapped.planes) {
            sync_dma_buf(plane.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        }

        if (is_still) {
            free_still_buffers.push_back(buffer);
        }
    }

    static bool is_supported(const libcamera::PixelFormat& format)
    {
        return format == libcamera::formats::YUV420 ||
               format == libcamera::formats::YUYV ||
               format == libcamera::formats::UYVY ||
               format == libcamera::formats::RGB565 ||
               format == libcamera::formats::RGB888 ||
               format == libcamera::formats::BGR888 ||
               format == libcamera::formats::XRGB8888 ||
               format == libcamera::formats::XBGR8888;
    }

    static void store_rgb_pixel(std::vector<uint8_t>& rgb, int idx, uint8_t r, uint8_t g, uint8_t b)
    {
        const int rgb_idx = idx * 3;
        rgb[rgb_idx] = r;
        rgb[rgb_idx + 1] = g;
        rgb[rgb_idx + 2] = b;
    }

    static void rgb565_pixel_to_rgb888(uint16_t pixel, uint8_t& r, uint8_t& g, uint8_t& b)
    {
        rgb565_to_rgb888(pixel, r, g, b);
    }

    static void store_rgb565_preview_pixel(CameraFrame& frame, int width, int height, int idx, uint8_t r, uint8_t g, uint8_t b)
    {
        if (frame.rgb565.size() != static_cast<size_t>(width * height)) {
            return;
        }
        frame.rgb565[idx] = rgb888_to_rgb565(r, g, b);
    }

    static uint8_t clip_u8(int value)
    {
        return static_cast<uint8_t>(std::max(0, std::min(value, 255)));
    }

    void convert_yuv420_frame(const std::vector<const uint8_t*>& planes,
                              const std::vector<size_t>& bytes_used,
                              int width,
                              int height,
                              int stride,
                              bool is_still)
    {
        const int y_stride = stride > 0 ? stride : width;
        const int uv_stride = std::max(1, y_stride / 2);
        const int uv_height = (height + 1) / 2;
        const size_t y_size = static_cast<size_t>(y_stride) * height;
        const size_t uv_size = static_cast<size_t>(uv_stride) * uv_height;
        const size_t required_size = y_size + uv_size * 2;
        if (width <= 0 || height <= 0 || y_stride < width || planes.empty()) {
            return;
        }

        const uint8_t* y_plane = planes[0];
        const uint8_t* u_plane = nullptr;
        const uint8_t* v_plane = nullptr;
        if (planes.size() >= 3) {
            if (bytes_used.size() < 3 ||
                bytes_used[0] < y_size ||
                bytes_used[1] < uv_size ||
                bytes_used[2] < uv_size) {
                LOG_WARN("YUV420 multi-plane frame is too small: y={} u={} v={} required_y={} required_uv={}",
                         bytes_used.size() > 0 ? bytes_used[0] : 0,
                         bytes_used.size() > 1 ? bytes_used[1] : 0,
                         bytes_used.size() > 2 ? bytes_used[2] : 0,
                         y_size,
                         uv_size);
                return;
            }
            u_plane = planes[1];
            v_plane = planes[2];
        } else if (bytes_used[0] >= required_size) {
            u_plane = y_plane + y_size;
            v_plane = u_plane + uv_size;
        } else {
            LOG_WARN("YUV420 frame is too small: used={} required={} size={}x{} stride={}",
                     bytes_used[0],
                     required_size,
                     width,
                     height,
                     y_stride);
            return;
        }

        std::vector<uint8_t>* rgb_out = is_still ? &still_rgb : nullptr;
        CameraFrame* frame_out = is_still ? nullptr : &pending_frame;
        if (is_still && (!rgb_out || rgb_out->size() != static_cast<size_t>(width * height * 3))) {
            still_rgb.assign(width * height * 3, 0);
            rgb_out = &still_rgb;
        }

        for (int y = 0; y < height; ++y) {
            const int dst_y = height - 1 - y;
            for (int x = 0; x < width; ++x) {
                const int dst_x = width - 1 - x;
                const int idx = dst_y * width + dst_x;
                const int uv_idx = (y / 2) * uv_stride + (x / 2);

                const int yy = static_cast<int>(y_plane[y * y_stride + x]);
                const int uu = static_cast<int>(u_plane[uv_idx]) - 128;
                const int vv = static_cast<int>(v_plane[uv_idx]) - 128;

                const uint8_t r = clip_u8(yy + ((1436 * vv) >> 10));
                const uint8_t g = clip_u8(yy - ((352 * uu + 731 * vv) >> 10));
                const uint8_t b = clip_u8(yy + ((1815 * uu) >> 10));
                if (is_still) {
                    store_rgb_pixel(*rgb_out, idx, r, g, b);
                } else {
                    store_rgb565_preview_pixel(*frame_out, width, height, idx, r, g, b);
                }
            }
        }
    }

    void store_yuv422_pair(uint8_t y0,
                           uint8_t u,
                           uint8_t y1,
                           uint8_t v,
                           int dst_y,
                           int dst_x0,
                           int dst_x1,
                           int width,
                           bool is_still)
    {
        auto write_pixel = [&](int dst_x, uint8_t y_value) {
            if (dst_x < 0 || dst_x >= width) {
                return;
            }

            const int yy = static_cast<int>(y_value);
            const int uu = static_cast<int>(u) - 128;
            const int vv = static_cast<int>(v) - 128;
            const uint8_t r = clip_u8(yy + ((1436 * vv) >> 10));
            const uint8_t g = clip_u8(yy - ((352 * uu + 731 * vv) >> 10));
            const uint8_t b = clip_u8(yy + ((1815 * uu) >> 10));
            const int idx = dst_y * width + dst_x;
            if (is_still) {
                store_rgb_pixel(still_rgb, idx, r, g, b);
            } else {
                store_rgb565_preview_pixel(pending_frame, width, pending_frame.height, idx, r, g, b);
            }
        };

        write_pixel(dst_x0, y0);
        write_pixel(dst_x1, y1);
    }

    void convert_yuv422_packed_frame(const std::vector<const uint8_t*>& planes,
                                     const std::vector<size_t>& bytes_used,
                                     int width,
                                     int height,
                                     int stride,
                                     const libcamera::PixelFormat& format,
                                     bool is_still)
    {
        if (planes.empty() || bytes_used.empty() || !planes[0]) {
            return;
        }

        const int row_stride = stride > 0 ? stride : width * 2;
        const int min_stride = width * 2;
        if (row_stride < min_stride) {
            return;
        }

        if (is_still && still_rgb.size() != static_cast<size_t>(width * height * 3)) {
            still_rgb.assign(width * height * 3, 0);
        }

        const uint8_t* src = planes[0];
        const bool is_yuyv = format == libcamera::formats::YUYV;
        for (int y = 0; y < height; ++y) {
            const size_t row_offset = static_cast<size_t>(y) * row_stride;
            if (row_offset + min_stride > bytes_used[0]) {
                break;
            }

            const uint8_t* line = src + row_offset;
            const int dst_y = height - 1 - y;
            for (int x = 0; x < width; x += 2) {
                const uint8_t b0 = line[x * 2];
                const uint8_t b1 = line[x * 2 + 1];
                const uint8_t b2 = line[x * 2 + 2];
                const uint8_t b3 = line[x * 2 + 3];

                const uint8_t y0 = is_yuyv ? b0 : b1;
                const uint8_t u  = is_yuyv ? b1 : b0;
                const uint8_t y1 = is_yuyv ? b2 : b3;
                const uint8_t v  = is_yuyv ? b3 : b2;
                const int dst_x0 = width - 1 - x;
                const int dst_x1 = width - 2 - x;
                store_yuv422_pair(y0, u, y1, v, dst_y, dst_x0, dst_x1, width, is_still);
            }
        }
    }

    void convert_frame(const std::vector<const uint8_t*>& planes,
                       const std::vector<size_t>& bytes_used,
                       int width,
                       int height,
                       int stride,
                       const libcamera::PixelFormat& format,
                       bool is_still)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (planes.empty() || !planes[0] || bytes_used.empty() || width <= 0 || height <= 0) {
            return;
        }

        const bool is_rgb888 = format == libcamera::formats::RGB888;
        const bool is_bgr888 = format == libcamera::formats::BGR888;
        const bool is_xrgb8888 = format == libcamera::formats::XRGB8888;
        const bool is_xbgr8888 = format == libcamera::formats::XBGR8888;
        const bool is_rgb565 = format == libcamera::formats::RGB565;
        const bool is_yuv420 = format == libcamera::formats::YUV420;
        const bool is_yuv422_packed = format == libcamera::formats::YUYV ||
                                      format == libcamera::formats::UYVY;

        if (is_yuv420) {
            convert_yuv420_frame(planes, bytes_used, width, height, stride, is_still);
            if (is_still) {
                const bool saved = save_jpeg_rgb888(last_capture_path, still_rgb, width, height, 92);
                capture_state = saved ? CaptureState::Saved : CaptureState::Failed;
            } else {
                pending_frame.width = width;
                pending_frame.height = height;
                new_frame = true;
            }
            return;
        }

        if (is_yuv422_packed) {
            convert_yuv422_packed_frame(planes, bytes_used, width, height, stride, format, is_still);
            if (is_still) {
                const bool saved = save_jpeg_rgb888(last_capture_path, still_rgb, width, height, 92);
                capture_state = saved ? CaptureState::Saved : CaptureState::Failed;
            } else {
                pending_frame.width = width;
                pending_frame.height = height;
                new_frame = true;
            }
            return;
        }

        const int bytes_per_pixel = (is_rgb888 || is_bgr888) ? 3 : (is_rgb565 ? 2 : 4);
        const int min_stride = width * bytes_per_pixel;
        const int row_stride = stride > 0 ? stride : min_stride;
        if (row_stride < min_stride) {
            return;
        }

        const uint8_t* src = planes[0];
        const size_t src_size = bytes_used[0];
        std::vector<uint8_t>* rgb_out = is_still ? &still_rgb : nullptr;
        if (is_still && (!rgb_out || rgb_out->size() != static_cast<size_t>(width * height * 3))) {
            still_rgb.assign(width * height * 3, 0);
            rgb_out = &still_rgb;
        }

        for (int y = 0; y < height; ++y) {
            const size_t row_offset = static_cast<size_t>(y) * row_stride;
            if (row_offset + min_stride > src_size) {
                break;
            }

            const uint8_t* line = src + row_offset;
            const int dst_y = height - 1 - y;

            for (int x = 0; x < width; ++x) {
                const int dst_x = width - 1 - x;
                const int idx = dst_y * width + dst_x;
                uint8_t r = 0;
                uint8_t g = 0;
                uint8_t b = 0;
                if (is_rgb565) {
                    const uint8_t* p = line + x * 2;
                    rgb565_pixel_to_rgb888(static_cast<uint16_t>(p[0] | (p[1] << 8)), r, g, b);
                } else if (is_rgb888) {
                    const uint8_t* p = line + x * 3;
                    r = p[0];
                    g = p[1];
                    b = p[2];
                } else if (is_bgr888) {
                    const uint8_t* p = line + x * 3;
                    b = p[0];
                    g = p[1];
                    r = p[2];
                } else if (is_xrgb8888) {
                    const uint8_t* p = line + x * 4;
                    b = p[0];
                    g = p[1];
                    r = p[2];
                } else if (is_xbgr8888) {
                    const uint8_t* p = line + x * 4;
                    r = p[0];
                    g = p[1];
                    b = p[2];
                }

                if (is_still) {
                    store_rgb_pixel(*rgb_out, idx, r, g, b);
                } else {
                    store_rgb565_preview_pixel(pending_frame, width, height, idx, r, g, b);
                }
            }
        }

        if (is_still) {
            const bool saved = save_jpeg_rgb888(last_capture_path, still_rgb, width, height, 92);
            capture_state = saved ? CaptureState::Saved : CaptureState::Failed;
        } else {
            pending_frame.width = width;
            pending_frame.height = height;
            new_frame = true;
        }
    }
#endif
};

CameraService::CameraService() = default;

CameraService::~CameraService()
{
    stop();
}

void CameraService::ensure_impl_()
{
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
}

void CameraService::start()
{
    if (state_ == CameraServiceState::Starting || state_ == CameraServiceState::Ready) {
        return;
    }

    LOG_INFO("Camera service start requested");
    elapsed_ms_ = 0;
    new_frame_ = false;
    preview_ready_ = false;
    capture_state_ = CaptureState::Idle;
    last_capture_path_.clear();

#if USE_DESKTOP
    state_ = CameraServiceState::Starting;
    status_message_ = "Preparing camera preview...";
#else
    ensure_impl_();
    impl_->set_capture_resolution(capture_resolution_);
    impl_->set_zoom_state(zoom_state_);
    if (!impl_->open()) {
        state_ = CameraServiceState::Error;
        status_message_ = impl_->last_error.empty() ? "Camera unavailable" : impl_->last_error;
        return;
    }

    state_ = CameraServiceState::Ready;
    status_message_ = "Camera ready";
#endif
}

void CameraService::stop()
{
    LOG_INFO("Camera service stopped");
#if !USE_DESKTOP
    if (impl_) {
        impl_->close();
    }
#endif
    state_ = CameraServiceState::Idle;
    elapsed_ms_ = 0;
    new_frame_ = false;
    preview_ready_ = false;
    status_message_ = "Camera idle";
}

void CameraService::update(uint32_t delta_ms)
{
#if USE_DESKTOP
    if (state_ == CameraServiceState::Starting) {
        elapsed_ms_ += delta_ms;
        if (elapsed_ms_ >= startup_delay_ms_) {
            state_ = CameraServiceState::Ready;
            status_message_ = "Camera preview simulator";
            LOG_INFO("Camera service ready");
        }
    }

    if (state_ == CameraServiceState::Ready) {
        elapsed_ms_ += delta_ms;
        generate_placeholder_frame_();
    }
#else
    (void)delta_ms;
    if (state_ == CameraServiceState::Ready && impl_) {
        CameraFrame frame;
        if (impl_->consume_frame(frame)) {
            latest_frame_ = std::move(frame);
            new_frame_ = true;
            preview_ready_ = true;
        }

        std::string path;
        const CaptureState state = impl_->consume_capture_state(&path);
        if (state == CaptureState::Saved || state == CaptureState::Failed || state == CaptureState::Requested) {
            capture_state_ = state;
            last_capture_path_ = std::move(path);
        }
    }
#endif
}

bool CameraService::consume_frame(CameraFrame& frame)
{
    if (!new_frame_) {
        return false;
    }

    frame = latest_frame_;
    new_frame_ = false;
    return true;
}

bool CameraService::request_capture()
{
    if (state_ != CameraServiceState::Ready) {
        capture_state_ = CaptureState::Failed;
        status_message_ = "Camera not ready";
        return false;
    }

#if USE_DESKTOP
    last_capture_path_ = "desktop-preview-not-saved.jpg";
    capture_state_ = CaptureState::Saved;
    return true;
#else
    ensure_impl_();
    if (!impl_ || !impl_->request_capture()) {
        capture_state_ = CaptureState::Failed;
        status_message_ = "Capture failed";
        return false;
    }

    capture_state_ = CaptureState::Requested;
    last_capture_path_ = impl_->last_capture_path;
    return true;
#endif
}

void CameraService::set_capture_resolution(CameraResolution resolution)
{
    capture_resolution_.width = std::max(1, std::min(resolution.width, kSensorMaxWidth));
    capture_resolution_.height = std::max(1, std::min(resolution.height, kSensorMaxHeight));
#if !USE_DESKTOP
    if (impl_ && state_ == CameraServiceState::Idle) {
        impl_->set_capture_resolution(capture_resolution_);
    }
#endif
}

void CameraService::zoom_in()
{
    if (zoom_state_.zoom_percent < kMidZoomPercent) {
        zoom_state_.zoom_percent = kMidZoomPercent;
    } else {
        zoom_state_.zoom_percent = kMaxZoomPercent;
    }
#if !USE_DESKTOP
    if (impl_) {
        impl_->set_zoom_state(zoom_state_);
    }
#endif
}

void CameraService::zoom_out()
{
    if (zoom_state_.zoom_percent > kMidZoomPercent) {
        zoom_state_.zoom_percent = kMidZoomPercent;
    } else {
        zoom_state_.zoom_percent = kMinZoomPercent;
    }
    if (zoom_state_.zoom_percent == kMinZoomPercent) {
        zoom_state_.view_x_percent = 50;
        zoom_state_.view_y_percent = 50;
    }
#if !USE_DESKTOP
    if (impl_) {
        impl_->set_zoom_state(zoom_state_);
    }
#endif
}

void CameraService::pan(int dx, int dy)
{
    if (zoom_state_.zoom_percent <= kMinZoomPercent) {
        return;
    }

    zoom_state_.view_x_percent = std::max(0, std::min(100, zoom_state_.view_x_percent + dx * kPanStepPercent));
    zoom_state_.view_y_percent = std::max(0, std::min(100, zoom_state_.view_y_percent + dy * kPanStepPercent));
#if !USE_DESKTOP
    if (impl_) {
        impl_->set_zoom_state(zoom_state_);
    }
#endif
}

CaptureState CameraService::consume_capture_state(std::string* path)
{
    if (path) {
        *path = last_capture_path_;
    }

    const CaptureState state = capture_state_;
    if (capture_state_ == CaptureState::Saved || capture_state_ == CaptureState::Failed) {
        capture_state_ = CaptureState::Idle;
    }
    return state;
}

void CameraService::generate_placeholder_frame_()
{
    constexpr int width = kPreviewWidth;
    constexpr int height = kPreviewHeight;
    if (latest_frame_.width != width || latest_frame_.height != height ||
        latest_frame_.rgb565.size() != static_cast<size_t>(width * height)) {
        latest_frame_.width = width;
        latest_frame_.height = height;
        latest_frame_.rgb565.assign(width * height, 0);
    }

    const uint32_t t = elapsed_ms_ / 16;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t r = static_cast<uint8_t>((x + t) & 0xFF);
            const uint8_t g = static_cast<uint8_t>((y * 2 + t) & 0xFF);
            const uint8_t b = static_cast<uint8_t>((x + y + t * 2) & 0xFF);
            latest_frame_.rgb565[y * width + x] = rgb888_to_rgb565(r, g, b);
        }
    }

    preview_ready_ = true;
    new_frame_ = true;

    (void)capture_resolution_;
    (void)zoom_state_;
}

} // namespace service
