#pragma once

#include "base_service.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace service {

enum class CameraServiceState {
    Idle,
    Starting,
    Ready,
    Error
};

struct CameraFrame {
    int width{0};
    int height{0};
    std::vector<uint16_t> rgb565;
};

struct CameraResolution {
    int width{0};
    int height{0};
};

struct CameraZoomState {
    int zoom_percent{100};
    int view_x_percent{50};
    int view_y_percent{50};
};

enum class CaptureState {
    Idle,
    Requested,
    Saved,
    Failed
};

class CameraService : public BaseService {
public:
    CameraService();
    ~CameraService() override;

    void start() override;
    void stop() override;
    void update(uint32_t delta_ms) override;

    bool is_ready() const override { return state_ == CameraServiceState::Ready; }
    CameraServiceState state() const { return state_; }
    std::string status_message() const override { return status_message_; }

    void set_startup_delay(uint32_t delay_ms) { startup_delay_ms_ = delay_ms; }
    bool consume_frame(CameraFrame& frame);
    bool request_capture();
    void set_capture_resolution(CameraResolution resolution);
    CameraResolution capture_resolution() const { return capture_resolution_; }
    void zoom_in();
    void zoom_out();
    void pan(int dx, int dy);
    CameraZoomState zoom_state() const { return zoom_state_; }
    CaptureState consume_capture_state(std::string* path = nullptr);
    bool has_preview() const { return preview_ready_; }

private:
    struct Impl;

    void ensure_impl_();
    void generate_placeholder_frame_();

    CameraServiceState state_{CameraServiceState::Idle};
    uint32_t elapsed_ms_{0};
    uint32_t startup_delay_ms_{1000};
    std::string status_message_{"Camera idle"};
    std::unique_ptr<Impl> impl_;
    CameraFrame latest_frame_;
    bool new_frame_{false};
    bool preview_ready_{false};
    CaptureState capture_state_{CaptureState::Idle};
    std::string last_capture_path_;
    CameraResolution capture_resolution_{3280, 2464};
    CameraZoomState zoom_state_{};
};

} // namespace service
