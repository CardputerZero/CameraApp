# Used for desktop development and CM0/aarch64 Linux cross builds.
cmake_minimum_required(VERSION 3.20)

set(USE_DESKTOP ON CACHE BOOL "Build the desktop emulator target with LVGL SDL support")

if(NOT USE_DESKTOP AND NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc CACHE FILEPATH "aarch64 C compiler")
    set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++ CACHE FILEPATH "aarch64 C++ compiler")
endif()

project(camera_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(SDK_PATH "$ENV{SDK_PATH}" CACHE PATH "Path to M5CardputerZero SDK")
if(NOT EXISTS "${SDK_PATH}/github_source/lvgl/lvgl_9_5/lvgl/CMakeLists.txt")
    set(SDK_PATH_CANDIDATES
        "${PROJECT_SOURCE_DIR}/../M5CardputerZero-Launcher/SDK"
        "${PROJECT_SOURCE_DIR}/../../SDK"
    )
    foreach(SDK_PATH_CANDIDATE IN LISTS SDK_PATH_CANDIDATES)
        get_filename_component(SDK_PATH_CANDIDATE_ABS "${SDK_PATH_CANDIDATE}" ABSOLUTE)
        if(EXISTS "${SDK_PATH_CANDIDATE_ABS}/github_source/lvgl/lvgl_9_5/lvgl/CMakeLists.txt")
            set(SDK_PATH "${SDK_PATH_CANDIDATE_ABS}" CACHE PATH "Path to M5CardputerZero SDK" FORCE)
            break()
        endif()
    endforeach()
endif()

include(FetchContent)

find_package(fmt CONFIG QUIET)
if(NOT TARGET fmt::fmt)
    set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.1.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(fmt)
endif()

find_package(cJSON CONFIG QUIET)
if(TARGET cjson)
    set(CJSON_TARGET cjson)
elseif(TARGET cJSON::cJSON)
    set(CJSON_TARGET cJSON::cJSON)
else()
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TARGET_EXPORT OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG v1.7.18
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cjson)

    if(TARGET cjson)
        set(CJSON_TARGET cjson)
    elseif(TARGET cJSON::cJSON)
        set(CJSON_TARGET cJSON::cJSON)
    else()
        message(FATAL_ERROR "cJSON target was not created")
    endif()
endif()

set(CJSON_EXTRA_INCLUDE_DIRS)
find_path(CJSON_HEADER_INCLUDE_DIR
    NAMES cJSON.h
    PATH_SUFFIXES cjson
)
if(CJSON_HEADER_INCLUDE_DIR)
    list(APPEND CJSON_EXTRA_INCLUDE_DIRS ${CJSON_HEADER_INCLUDE_DIR})
endif()
if(DEFINED cjson_SOURCE_DIR)
    list(APPEND CJSON_EXTRA_INCLUDE_DIRS ${cjson_SOURCE_DIR})
endif()
if(DEFINED cjson_BINARY_DIR)
    list(APPEND CJSON_EXTRA_INCLUDE_DIRS ${cjson_BINARY_DIR})
endif()
list(REMOVE_DUPLICATES CJSON_EXTRA_INCLUDE_DIRS)

set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "${PROJECT_SOURCE_DIR}/config/lv_conf.h" CACHE PATH "" FORCE)

set(LOCAL_LVGL_DIR "${SDK_PATH}/github_source/lvgl/lvgl_9_5/lvgl" CACHE PATH "Local LVGL source directory")
if(NOT EXISTS "${LOCAL_LVGL_DIR}/CMakeLists.txt")
    set(LOCAL_LVGL_DIR "${SDK_PATH}/github_source/lvgl/lvgl_9_5/lvgl" CACHE PATH "Local LVGL source directory" FORCE)
endif()
if(EXISTS "${LOCAL_LVGL_DIR}/CMakeLists.txt")
    set(LVGL_SOURCE_DIR "${LOCAL_LVGL_DIR}")
    add_subdirectory("${LVGL_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/lvgl-build")
else()
    FetchContent_Declare(
        lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG v9.5.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/lvgl
    )
    FetchContent_MakeAvailable(lvgl)
    set(LVGL_SOURCE_DIR "${lvgl_SOURCE_DIR}")
endif()
get_filename_component(LVGL_INCLUDE_PARENT "${LVGL_SOURCE_DIR}" DIRECTORY)

if(USE_DESKTOP)
    message(STATUS "Configuring DESKTOP build")
    find_package(Freetype REQUIRED)
    find_package(PNG REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(SDL2 CONFIG QUIET)
    if(TARGET SDL2::SDL2)
        set(SDL2_TARGET SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        set(SDL2_TARGET SDL2::SDL2-static)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        set(SDL2_TARGET PkgConfig::SDL2)
    endif()

    target_compile_definitions(lvgl PUBLIC USE_DESKTOP=1)
    target_link_libraries(lvgl PUBLIC ${SDL2_TARGET} Freetype::Freetype PNG::PNG ZLIB::ZLIB)
    if(TARGET lvgl_thorvg)
        target_compile_definitions(lvgl_thorvg PRIVATE USE_DESKTOP=1)
    endif()
else()
    message(STATUS "Configuring CM0/aarch64 Linux build")
    set(CM0_SYSROOT_HINT "" CACHE PATH "Optional aarch64 sysroot/dependency hint")
    if(NOT CM0_SYSROOT_HINT AND EXISTS "${PROJECT_SOURCE_DIR}/static_lib/usr")
        set(CM0_SYSROOT_HINT "${PROJECT_SOURCE_DIR}/static_lib/usr" CACHE PATH "Optional aarch64 sysroot/dependency hint" FORCE)
    endif()

    set(CM0_DEP_HINTS)
    if(CM0_SYSROOT_HINT)
        list(APPEND CM0_DEP_HINTS "${CM0_SYSROOT_HINT}")
    endif()

    set(CM0_INCLUDE_HINTS
        ${CM0_DEP_HINTS}
        ${CM0_DEP_HINTS}/include/aarch64-linux-gnu
        /usr/aarch64-linux-gnu
        /usr/include/aarch64-linux-gnu
        /usr
    )
    set(CM0_LIBRARY_HINTS
        ${CM0_DEP_HINTS}
        /usr/lib/aarch64-linux-gnu
        /usr/aarch64-linux-gnu
        /usr
    )

    set(LIBCAMERA_MIN_VERSION "0.7" CACHE STRING "Minimum libcamera version for CM0 builds")
    set(CM0_ALLOW_MANUAL_LIBCAMERA_LOOKUP OFF CACHE BOOL "Allow manual libcamera lookup if pkg-config lookup fails")
    if(NOT CM0_ALLOW_MANUAL_LIBCAMERA_LOOKUP AND
       CM0_SYSROOT_HINT AND
       EXISTS "${CM0_SYSROOT_HINT}/include/libcamera/camera.h" AND
       EXISTS "${CM0_SYSROOT_HINT}/lib/aarch64-linux-gnu/libcamera.so")
        set(CM0_ALLOW_MANUAL_LIBCAMERA_LOOKUP ON CACHE BOOL "Allow manual libcamera lookup if pkg-config lookup fails" FORCE)
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        set(CM0_PKG_CONFIG_DIRS)
        foreach(CM0_DEP_HINT IN LISTS CM0_DEP_HINTS)
            list(APPEND CM0_PKG_CONFIG_DIRS
                "${CM0_DEP_HINT}/lib/aarch64-linux-gnu/pkgconfig"
                "${CM0_DEP_HINT}/share/pkgconfig"
            )
        endforeach()
        list(APPEND CM0_PKG_CONFIG_DIRS
            "/usr/lib/aarch64-linux-gnu/pkgconfig"
            "/usr/share/pkgconfig"
        )
        list(REMOVE_DUPLICATES CM0_PKG_CONFIG_DIRS)
        list(JOIN CM0_PKG_CONFIG_DIRS ":" CM0_PKG_CONFIG_PATHS)
        set(ENV{PKG_CONFIG_LIBDIR}
            "${CM0_PKG_CONFIG_PATHS}:$ENV{PKG_CONFIG_LIBDIR}")
        set(ENV{PKG_CONFIG_PATH}
            "${CM0_PKG_CONFIG_PATHS}:$ENV{PKG_CONFIG_PATH}")
        if(LIBCAMERA_MIN_VERSION)
            pkg_check_modules(LIBCAMERA_PKG QUIET IMPORTED_TARGET "libcamera>=${LIBCAMERA_MIN_VERSION}")
        else()
            pkg_check_modules(LIBCAMERA_PKG QUIET IMPORTED_TARGET libcamera)
        endif()
    endif()

    set(CAMERA_APP_USE_DRM ON CACHE BOOL "Enable LVGL Linux DRM display driver for CM0 builds when libdrm headers are available")

    find_path(DRM_XF86_INCLUDE_DIR
        NAMES xf86drm.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include include/libdrm
    )
    find_path(DRM_XF86_MODE_INCLUDE_DIR
        NAMES xf86drmMode.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include include/libdrm
    )
    find_path(DRM_MODE_INCLUDE_DIR
        NAMES drm.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include/drm include/libdrm drm libdrm
    )
    find_library(DRM_LIBRARY
        NAMES drm
        HINTS ${CM0_LIBRARY_HINTS}
        PATH_SUFFIXES lib/aarch64-linux-gnu lib
    )
    if(NOT DRM_LIBRARY)
        find_library(DRM_LIBRARY
            NAMES drm.so.2
            HINTS ${CM0_LIBRARY_HINTS}
            PATH_SUFFIXES lib/aarch64-linux-gnu lib
        )
    endif()

    set(CAMERA_APP_DRM_AVAILABLE OFF)
    if(DRM_XF86_INCLUDE_DIR AND DRM_XF86_MODE_INCLUDE_DIR AND DRM_MODE_INCLUDE_DIR AND DRM_LIBRARY)
        set(CAMERA_APP_DRM_AVAILABLE ON)
    endif()
    if(CAMERA_APP_USE_DRM AND CAMERA_APP_DRM_AVAILABLE)
        message(STATUS "CM0 DRM display support enabled")
    elseif(CAMERA_APP_USE_DRM)
        message(WARNING "libdrm-dev headers/library incomplete; building CM0 framebuffer-only display support")
        set(CAMERA_APP_USE_DRM OFF CACHE BOOL "Enable LVGL Linux DRM display driver for CM0 builds when libdrm headers are available" FORCE)
    endif()

    find_path(FREETYPE_INCLUDE_DIR ft2build.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include/freetype2 freetype2
    )
    find_library(FREETYPE_LIBRARY freetype
        HINTS ${CM0_LIBRARY_HINTS}
        PATH_SUFFIXES lib/aarch64-linux-gnu lib
    )

    find_path(PNG_INCLUDE_DIR png.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include include/libpng16 libpng16
    )
    find_library(PNG_LIBRARY png16
        HINTS ${CM0_LIBRARY_HINTS}
        PATH_SUFFIXES lib/aarch64-linux-gnu lib
    )

    find_path(JPEG_INCLUDE_DIR jpeglib.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include
    )
    find_library(JPEG_LIBRARY jpeg
        HINTS ${CM0_LIBRARY_HINTS}
        PATH_SUFFIXES lib/aarch64-linux-gnu lib
    )
    find_path(ALSA_INCLUDE_DIR
        NAMES alsa/asoundlib.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include
    )
    find_library(ALSA_LIBRARY asound
        HINTS ${CM0_LIBRARY_HINTS}
        PATH_SUFFIXES lib/aarch64-linux-gnu lib
    )
    if(NOT ALSA_LIBRARY)
        find_library(ALSA_LIBRARY
            NAMES asound.so.2
            HINTS ${CM0_LIBRARY_HINTS}
            PATH_SUFFIXES lib/aarch64-linux-gnu lib
        )
    endif()
    set(CAMERA_APP_USE_ALSA ON CACHE BOOL "Enable ALSA audio service for CM0 builds when libasound headers are available")
    if(CAMERA_APP_USE_ALSA AND (NOT ALSA_INCLUDE_DIR OR NOT ALSA_LIBRARY))
        message(WARNING "ALSA headers/library not found; building CM0 without ALSA audio support")
        set(CAMERA_APP_USE_ALSA OFF CACHE BOOL "Enable ALSA audio service for CM0 builds when libasound headers are available" FORCE)
    endif()
    if(LIBCAMERA_PKG_FOUND)
        message(STATUS "Using libcamera from pkg-config: version ${LIBCAMERA_PKG_VERSION}")
        set(LIBCAMERA_INCLUDE_DIR ${LIBCAMERA_PKG_INCLUDE_DIRS})
        set(LIBCAMERA_LIBRARY PkgConfig::LIBCAMERA_PKG)
        set(LIBCAMERA_BASE_LIBRARY "")
    else()
        if(NOT CM0_ALLOW_MANUAL_LIBCAMERA_LOOKUP)
            message(FATAL_ERROR "pkg-config could not find libcamera >= ${LIBCAMERA_MIN_VERSION}. Install libcamera-dev:arm64 0.7+, clear the CMake build directory, or set CM0_ALLOW_MANUAL_LIBCAMERA_LOOKUP=ON only for matching BSP libraries.")
        endif()
        message(WARNING "pkg-config libcamera >= ${LIBCAMERA_MIN_VERSION} not found; falling back to manual libcamera lookup")
        find_path(LIBCAMERA_INCLUDE_DIR
            NAMES libcamera/camera.h
            HINTS ${CM0_INCLUDE_HINTS}
            PATH_SUFFIXES include
        )
        find_library(LIBCAMERA_LIBRARY camera
            HINTS ${CM0_LIBRARY_HINTS}
            PATH_SUFFIXES lib/aarch64-linux-gnu lib
        )
        find_library(LIBCAMERA_BASE_LIBRARY camera-base
            HINTS ${CM0_LIBRARY_HINTS}
            PATH_SUFFIXES lib/aarch64-linux-gnu lib
        )
        message(STATUS "Using libcamera library: ${LIBCAMERA_LIBRARY}")
        message(STATUS "Using libcamera-base library: ${LIBCAMERA_BASE_LIBRARY}")
    endif()

    find_path(ZLIB_INCLUDE_DIR zlib.h
        HINTS ${CM0_INCLUDE_HINTS}
        PATH_SUFFIXES include
    )
    find_library(ZLIB_LIBRARY z
        HINTS ${CM0_LIBRARY_HINTS}
        PATH_SUFFIXES lib/aarch64-linux-gnu lib
    )

    if(CAMERA_APP_USE_DRM AND NOT CAMERA_APP_DRM_AVAILABLE)
        message(FATAL_ERROR "libdrm headers/library not found. Install libdrm-dev:arm64, set CM0_SYSROOT_HINT, or configure with -DCAMERA_APP_USE_DRM=OFF.")
    endif()
    if(NOT FREETYPE_INCLUDE_DIR OR NOT FREETYPE_LIBRARY)
        message(FATAL_ERROR "Freetype headers/library not found. Install libfreetype-dev:arm64 or set CM0_SYSROOT_HINT.")
    endif()
    if(NOT PNG_INCLUDE_DIR OR NOT PNG_LIBRARY)
        message(FATAL_ERROR "PNG headers/library not found. Install libpng-dev:arm64 or set CM0_SYSROOT_HINT.")
    endif()
    if(NOT JPEG_INCLUDE_DIR OR NOT JPEG_LIBRARY)
        message(FATAL_ERROR "JPEG headers/library not found. Install libjpeg-dev:arm64 or set CM0_SYSROOT_HINT.")
    endif()
    if(CAMERA_APP_USE_ALSA AND (NOT ALSA_INCLUDE_DIR OR NOT ALSA_LIBRARY))
        message(FATAL_ERROR "ALSA headers/library not found. Install libasound2-dev:arm64, set CM0_SYSROOT_HINT, or configure with -DCAMERA_APP_USE_ALSA=OFF.")
    endif()
    if(NOT LIBCAMERA_INCLUDE_DIR OR NOT LIBCAMERA_LIBRARY)
        message(FATAL_ERROR "libcamera headers/library not found. Install libcamera-dev:arm64 or set CM0_SYSROOT_HINT.")
    endif()
    if(NOT ZLIB_INCLUDE_DIR OR NOT ZLIB_LIBRARY)
        message(FATAL_ERROR "Zlib headers/library not found. Install zlib1g-dev:arm64 or set CM0_SYSROOT_HINT.")
    endif()

    set(CM0_MULTIARCH_INCLUDE_DIRS)
    foreach(CM0_DEP_HINT IN LISTS CM0_DEP_HINTS)
        if(EXISTS "${CM0_DEP_HINT}/include/aarch64-linux-gnu/bits/timesize.h")
            list(APPEND CM0_MULTIARCH_INCLUDE_DIRS "${CM0_DEP_HINT}/include/aarch64-linux-gnu")
        endif()
    endforeach()

    set(CM0_DEP_INCLUDE_DIRS
        ${CM0_MULTIARCH_INCLUDE_DIRS}
        ${FREETYPE_INCLUDE_DIR}
        ${PNG_INCLUDE_DIR}
        ${JPEG_INCLUDE_DIR}
        ${LIBCAMERA_INCLUDE_DIR}
        ${ZLIB_INCLUDE_DIR}
    )
    set(CM0_DEP_LIBRARIES
        ${FREETYPE_LIBRARY}
        ${PNG_LIBRARY}
        ${JPEG_LIBRARY}
        ${LIBCAMERA_LIBRARY}
        ${ZLIB_LIBRARY}
        pthread
        m
    )
    if(LIBCAMERA_BASE_LIBRARY)
        list(APPEND CM0_DEP_LIBRARIES ${LIBCAMERA_BASE_LIBRARY})
    endif()
    if(CAMERA_APP_USE_DRM)
        list(APPEND CM0_DEP_INCLUDE_DIRS
            ${DRM_XF86_INCLUDE_DIR}
            ${DRM_XF86_MODE_INCLUDE_DIR}
            ${DRM_MODE_INCLUDE_DIR}
        )
        list(APPEND CM0_DEP_LIBRARIES ${DRM_LIBRARY})
    endif()
    if(CAMERA_APP_USE_ALSA)
        list(APPEND CM0_DEP_INCLUDE_DIRS ${ALSA_INCLUDE_DIR})
        list(APPEND CM0_DEP_LIBRARIES ${ALSA_LIBRARY})
    endif()

    target_compile_definitions(lvgl
        PUBLIC
            USE_DESKTOP=0
            CAMERA_APP_USE_DRM=$<BOOL:${CAMERA_APP_USE_DRM}>
    )
    if(TARGET lvgl_thorvg)
        target_compile_definitions(lvgl_thorvg
            PRIVATE
                USE_DESKTOP=0
                CAMERA_APP_USE_DRM=$<BOOL:${CAMERA_APP_USE_DRM}>
        )
    endif()

    target_include_directories(lvgl
        PUBLIC
            ${CM0_DEP_INCLUDE_DIRS}
    )

    target_link_libraries(lvgl
        PUBLIC
            ${CM0_DEP_LIBRARIES}
    )
endif()

add_library(camera_utils
    src/utils/logger.cpp
    src/utils/asset_manager.cpp
)

target_include_directories(camera_utils
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CJSON_EXTRA_INCLUDE_DIRS}
)

target_link_libraries(camera_utils
    PUBLIC
        fmt::fmt
        ${CJSON_TARGET}
)

target_compile_definitions(camera_utils
    PUBLIC
        APP_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets"
)

add_library(camera_services
    src/services/audio_service.cpp
    src/services/camera_service.cpp
    src/services/gallery_service.cpp
)

target_include_directories(camera_services
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(camera_services
    PUBLIC
        camera_utils
)

target_compile_definitions(camera_services
    PUBLIC
        USE_DESKTOP=$<BOOL:${USE_DESKTOP}>
        CAMERA_APP_USE_ALSA=$<BOOL:${CAMERA_APP_USE_ALSA}>
)

if(NOT USE_DESKTOP)
    target_include_directories(camera_services
        PUBLIC
            ${CM0_DEP_INCLUDE_DIRS}
    )

    target_link_libraries(camera_services
        PUBLIC
            ${CM0_DEP_LIBRARIES}
    )
endif()

add_library(camera_viewmodels
    src/viewmodels/splash_viewmodel.cpp
    src/viewmodels/camera_viewmodel.cpp
    src/viewmodels/gallery_viewmodel.cpp
)

target_include_directories(camera_viewmodels
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${LVGL_INCLUDE_PARENT}
)

target_link_libraries(camera_viewmodels
    PUBLIC
        camera_services
        lvgl
)

add_library(camera_ui
    src/ui/app_style.cpp
    src/ui/app_font.cpp
    src/views/camera_view.cpp
    src/views/gallery_view.cpp
    src/views/splash_view.cpp
)

target_include_directories(camera_ui
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${LVGL_INCLUDE_PARENT}
)

target_compile_definitions(camera_ui
    PUBLIC
        USE_DESKTOP=$<BOOL:${USE_DESKTOP}>
        APP_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets"
)

target_link_libraries(camera_ui
    PUBLIC
        camera_viewmodels
        lvgl::lvgl
)

add_library(camera_input
    src/input/linux_keypad.cpp
    src/input/sdl_keypad.cpp
)

target_include_directories(camera_input
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${LVGL_INCLUDE_PARENT}
)

target_compile_definitions(camera_input
    PUBLIC
        USE_DESKTOP=$<BOOL:${USE_DESKTOP}>
)

target_link_libraries(camera_input
    PUBLIC
        camera_utils
        lvgl::lvgl
)

if(USE_DESKTOP)
    target_link_libraries(camera_ui PUBLIC ${SDL2_TARGET})
    target_link_libraries(camera_input PUBLIC ${SDL2_TARGET})
endif()

add_executable(camera_app
    src/main.cpp
)

target_include_directories(camera_app
    PRIVATE
        ${LVGL_INCLUDE_PARENT}
)

target_compile_definitions(camera_app
    PRIVATE
        USE_DESKTOP=$<BOOL:${USE_DESKTOP}>
        CAMERA_APP_USE_DRM=$<BOOL:${CAMERA_APP_USE_DRM}>
)

target_link_libraries(camera_app
    PRIVATE
        camera_ui
        camera_input
)

include(GNUInstallDirs)
install(PROGRAMS $<TARGET_FILE:camera_app>
    DESTINATION share/APPLaunch/bin
    RENAME M5CardputerZero-CameraApp
    COMPONENT CameraApp
)

install(DIRECTORY assets/fonts
    DESTINATION share/CameraApp/assets
    COMPONENT CameraApp
    PATTERN ".DS_Store" EXCLUDE
    PATTERN "._*" EXCLUDE
)

install(DIRECTORY assets/audio
    DESTINATION share/CameraApp/assets
    COMPONENT CameraApp
    PATTERN ".DS_Store" EXCLUDE
    PATTERN "._*" EXCLUDE
)

install(FILES assets/applications/camera.desktop
    DESTINATION share/APPLaunch/applications
    COMPONENT CameraApp
)

install(FILES assets/images/camera_100.png
    DESTINATION share/APPLaunch/share/images
    COMPONENT CameraApp
)

enable_testing()

add_executable(test_logger
    test/test_logger.cpp
)
target_link_libraries(test_logger PRIVATE camera_utils)
add_test(NAME logger COMMAND test_logger)

add_executable(test_logger_compile_level
    test/test_logger_compile_level.cpp
)
target_compile_definitions(test_logger_compile_level PRIVATE APP_LOG_LEVEL=APP_LOG_LEVEL_WARN)
target_link_libraries(test_logger_compile_level PRIVATE camera_utils)
add_test(NAME logger_compile_level COMMAND test_logger_compile_level)

add_executable(test_json
    test/test_json.cpp
)
target_link_libraries(test_json PRIVATE camera_utils)
add_test(NAME json COMMAND test_json)

add_executable(test_splash_viewmodel
    test/test_splash_viewmodel.cpp
)
target_link_libraries(test_splash_viewmodel PRIVATE camera_viewmodels)
add_test(NAME splash_viewmodel COMMAND test_splash_viewmodel)

add_executable(test_camera_viewmodel
    test/test_camera_viewmodel.cpp
)
target_link_libraries(test_camera_viewmodel PRIVATE camera_viewmodels)
add_test(NAME camera_viewmodel COMMAND test_camera_viewmodel)

set(CAMERA_APP_PACKAGE_VERSION "0.1.0" CACHE STRING "CameraApp Debian package version")
set(CAMERA_APP_DEB_ARCHITECTURE "" CACHE STRING "Override Debian package architecture")
set(CAMERA_APP_DEB_DEPENDS
    "libc6, libstdc++6, libgcc-s1, libfreetype6, libpng16-16, libjpeg62-turbo, zlib1g, libcamera0.7, libcamera-ipa"
    CACHE STRING
    "CameraApp Debian package dependency list"
)
if(CAMERA_APP_USE_DRM AND NOT CAMERA_APP_DEB_DEPENDS MATCHES "(^|, )libdrm2($|,)")
    string(APPEND CAMERA_APP_DEB_DEPENDS ", libdrm2")
endif()
if(CAMERA_APP_USE_ALSA AND NOT CAMERA_APP_DEB_DEPENDS MATCHES "libasound2")
    string(APPEND CAMERA_APP_DEB_DEPENDS ", libasound2t64 | libasound2")
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "m5cardputerzero-camera")
set(CPACK_PACKAGE_VENDOR "M5CardputerZero")
set(CPACK_PACKAGE_CONTACT "kane")
set(CPACK_PACKAGE_VERSION "${CAMERA_APP_PACKAGE_VERSION}")
if(CAMERA_APP_PACKAGE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")
endif()
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Camera application for M5CardputerZero APPLaunch")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_SOURCE_DIR}/dist")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CAMERA_APP_DEB_DEPENDS}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
if(CAMERA_APP_DEB_ARCHITECTURE)
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CAMERA_APP_DEB_ARCHITECTURE}")
elseif(NOT USE_DESKTOP)
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
endif()

include(CPack)
if(TARGET package)
    add_dependencies(package camera_app)
endif()
