#pragma once

#include "audio_service.h"
#include "camera_service.h"
#include "gallery_service.h"

#include <memory>

namespace service {

struct AppServices {
    std::shared_ptr<AudioService> audio;
    std::shared_ptr<CameraService> camera;
    std::shared_ptr<GalleryService> gallery;

    static std::shared_ptr<AppServices> create() {
        auto services = std::make_shared<AppServices>();
        services->audio = std::make_shared<AudioService>();
        services->camera = std::make_shared<CameraService>();
        services->gallery = std::make_shared<GalleryService>();
        return services;
    }
};

} // namespace service
