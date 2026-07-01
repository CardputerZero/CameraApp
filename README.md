# CameraApp

CameraApp is an embedded Linux camera application built with LVGL 9 and CMake. It targets M5CardputerZero / CM0 APPLaunch integration and installs a standalone executable, launcher entry, icon, fonts, and audio assets.

## Directory Layout

```text
CameraApp/
├── src/
│   ├── app/
│   ├── config/
│   ├── input/
│   ├── services/
│   ├── ui/
│   ├── viewmodels/
│   └── views/
├── cmake/
│   ├── templates/
│   └── toolchains/
├── assets/
└── CMakePresets.json
```

CM0 cross builds use `cmake/toolchains/cp0-aarch64-linux-gnu.cmake`. The default sysroot location is `.cache/sdk_bsp-src`.

## Dependencies

Desktop preview uses SDL/LVGL and does not enable the CM0 framebuffer/DRM, libcamera, or ALSA runtime backends.

```bash
sudo apt install -y \
  build-essential cmake git pkg-config dpkg-dev \
  libsdl2-dev libfreetype-dev libpng-dev libjpeg-dev zlib1g-dev
```

For CM0 / arm64 cross builds, all CameraApp dependencies are required except DRM. fbdev is the default display backend; enable DRM only with `-DAPP_USE_DRM=ON`.

```bash
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install -y \
  gcc-aarch64-linux-gnu \
  g++-aarch64-linux-gnu \
  libfreetype-dev:arm64 \
  libpng-dev:arm64 \
  libjpeg-dev:arm64 \
  zlib1g-dev:arm64 \
  libasound2-dev:arm64 \
  libcamera-dev:arm64 \
  libfmt-dev:arm64 \
  libcjson-dev:arm64
```

Install `libdrm-dev:arm64` only when building with `APP_USE_DRM=ON`.

## Build

Desktop:

```bash
cmake --preset linux-x86-64
cmake --build --preset linux-x86-64-rel
./build/linux-x86-64/Release/camera_app
```

CM0 / arm64:

```bash
cmake --preset cp0-cross
cmake --build --preset cp0-cross-rel
```

With an explicit sysroot:

```bash
cmake -S . -B build/cp0-cross \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/cp0-aarch64-linux-gnu.cmake \
  -DUSE_DESKTOP=OFF \
  -DCM0_SDK_ROOT=/path/to/sysroot \
  -DCMAKE_SYSROOT=/path/to/sysroot
cmake --build build/cp0-cross --config Release --target camera_app --parallel 4
```

Enable DRM/KMS display backend:

```bash
cmake --preset cp0-cross -DAPP_USE_DRM=ON
```

## Debian Package

```bash
./package_deb.sh
```

Default output:

```text
dist/CameraApp_0.2.0_m5stack1_arm64.deb
```

Useful overrides:

```bash
./package_deb.sh -DAPP_USE_DRM=ON
./package_deb.sh -DCM0_SDK_ROOT=/path/to/sysroot -DCMAKE_SYSROOT=/path/to/sysroot
```

The package should contain at least:

```text
/usr/bin/camera_app
/usr/share/APPLaunch/applications/camera_app.desktop
/usr/share/APPLaunch/share/images/camera1.png
/usr/share/camera_app/fonts/...
/usr/share/camera_app/audio/...
/usr/share/CameraApp/assets/fonts/...
/usr/share/CameraApp/assets/audio/...
```

Runtime asset root can still be overridden with:

```bash
CAMERA_APP_ASSET_DIR=/custom/assets /usr/bin/camera_app
```
