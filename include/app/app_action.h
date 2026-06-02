#pragma once

namespace app {

enum class AppAction {
    None,
    Exit,
    ToggleHint,
    Capture,
    Confirm,
    Delete,
    ZoomOut,
    ZoomIn,
    OpenGallery,
    ToggleCaptureMode,
    PanUp,
    PanDown,
    PanLeft,
    PanRight,
};

} // namespace app
