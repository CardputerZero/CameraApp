# CameraApp

CameraApp is an embedded Linux camera application built with LVGL 9 and CMake. It is designed for M5CardputerZero / CM0 and integrates with APPLaunch. The application is installed as a standalone executable and also provides an APPLaunch `.desktop` entry, launcher icon, fonts, and audio assets.

## Directory Layout

Keep this project inside the full workspace when possible:

```text
debian_workspace/
├── SDK/
└── projects/
    └── CameraApp/
```

If the SDK is not in the default location, set it explicitly:

```bash
export SDK_PATH=/path/to/M5CardputerZero-Launcher/SDK
```

## Debian Environment Setup

### 1. Base Tools

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  git \
  pkg-config \
  dpkg-dev
```

`dpkg-dev` provides `dpkg-deb`, which is required to generate Debian packages.

### 2. Desktop Preview Dependencies

Desktop mode is used for local SDL/LVGL preview. It does not enable the CM0 Linux framebuffer/DRM, libcamera, or ALSA cross dependencies.

```bash
sudo apt install -y \
  libsdl2-dev \
  libfreetype-dev \
  libpng-dev \
  zlib1g-dev
```

### 3. CM0 / arm64 Cross-Compile Dependencies

If you are cross-compiling arm64 from Debian x86_64, enable arm64 multiarch first:

```bash
sudo dpkg --add-architecture arm64
sudo apt update
```

Then install the cross compiler and arm64 dependencies:

```bash
sudo apt install -y \
  gcc-aarch64-linux-gnu \
  g++-aarch64-linux-gnu \
  libfreetype-dev:arm64 \
  libpng-dev:arm64 \
  libjpeg-dev:arm64 \
  zlib1g-dev:arm64 \
  libdrm-dev:arm64 \
  libasound2-dev:arm64 \
  libcamera-dev:arm64
```

CameraApp currently targets libcamera `0.7+`. If your Debian repository does not provide a recent enough libcamera package, use a target-board sysroot or the project-local `static_lib/usr` directory, then pass it to CMake or the packaging script:

```bash
-DCM0_SYSROOT_HINT=/path/to/sysroot/usr
```

## CMake Build

The commands below do not build test targets. They are suitable for source packages that do not include the `test/` directory.

### Desktop Build

```bash
cd /path/to/debian_workspace/projects/CameraApp
cmake -S . -B build-desktop-local -DUSE_DESKTOP=ON
cmake --build build-desktop-local --target camera_app --parallel 4
```

Run the desktop build:

```bash
./build-desktop-local/camera_app
```

### CM0 / arm64 Build

```bash
cd /path/to/debian_workspace/projects/CameraApp
cmake -S . -B build-cm0-local -DUSE_DESKTOP=OFF
cmake --build build-cm0-local --target camera_app --parallel 4
```

Specify a sysroot explicitly if needed:

```bash
cmake -S . -B build-cm0-local \
  -DUSE_DESKTOP=OFF \
  -DCM0_SYSROOT_HINT=/path/to/sysroot/usr
cmake --build build-cm0-local --target camera_app --parallel 4
```

If the build directory was generated on another machine or through a different mount path, CMake may report a source/cache path mismatch. Regenerate the build directory in that case:

```bash
rm -rf build-cm0-local
cmake -S . -B build-cm0-local -DUSE_DESKTOP=OFF
cmake --build build-cm0-local --target camera_app --parallel 4
```

## Debian Packaging

Use the repository packaging script. It builds `camera_app`, stages the installed files, and then generates a `.deb` with `dpkg-deb`.

```bash
cd /path/to/debian_workspace/projects/CameraApp
CLEAN_BUILD=1 ./package_deb.sh
```

Default output:

```text
dist/CameraApp_0.1.0_m5stack1_arm64.deb
```

Optional parameters:

```bash
PACKAGE_VERSION=0.1.1 ./package_deb.sh
PACKAGE_SUFFIX=m5stack1 ./package_deb.sh
DEB_ARCH=arm64 ./package_deb.sh
CM0_SYSROOT_HINT=/path/to/sysroot/usr ./package_deb.sh
CAMERA_APP_USE_DRM=OFF CAMERA_APP_USE_ALSA=OFF ./package_deb.sh
```

Inspect the package contents:

```bash
dpkg-deb -c dist/CameraApp_0.1.0_m5stack1_arm64.deb
```

The package should contain at least these paths:

```text
/usr/bin/camera_app
/usr/share/APPLaunch/applications/camera.desktop
/usr/share/APPLaunch/share/images/camera1.png
/usr/share/CameraApp/assets/fonts/...
/usr/share/CameraApp/assets/audio/...
```

Install on the target board:

```bash
sudo apt install ./dist/CameraApp_0.1.0_m5stack1_arm64.deb
```

Or copy the package to the target board and install it there:

```bash
scp dist/CameraApp_0.1.0_m5stack1_arm64.deb pi@pi:~/
ssh pi@pi 'sudo apt install ./CameraApp_0.1.0_m5stack1_arm64.deb'
```

## Installed Asset Paths

After package installation, the application uses these paths:

```text
/usr/bin/camera_app
/usr/share/APPLaunch/applications/camera.desktop
/usr/share/APPLaunch/share/images/camera1.png
/usr/share/CameraApp/assets/fonts
/usr/share/CameraApp/assets/audio
```

You can override the asset root at runtime with an environment variable:

```bash
CAMERA_APP_ASSET_DIR=/custom/assets /usr/bin/camera_app
```
