#pragma once

namespace app {

enum class AppState {
    None,
    Splash,
    Camera,
    Gallery
};

inline const char* screen_id(AppState state)
{
    switch (state) {
        case AppState::Splash:  return "splash_screen";
        case AppState::Camera:  return "camera_screen";
        case AppState::Gallery: return "gallery_screen";
        case AppState::None:
        default:                return "";
    }
}

} // namespace app
