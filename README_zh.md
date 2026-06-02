# CameraApp

CameraApp 是一个基于 LVGL 9 / CMake 的 embedded Linux 相机应用，主要面向 M5CardputerZero / CM0 的 APPLaunch 集成使用。应用本身会安装为独立程序，同时提供 APPLaunch 的 `.desktop` 入口、图标、字体和音效资源。

## 目录约定

当前工程推荐保持在完整 workspace 中：

```text
debian_workspace/
├── SDK/
└── projects/
    └── CameraApp/
```

如果 SDK 不在默认位置，可以手动指定：

```bash
export SDK_PATH=/path/to/M5CardputerZero-Launcher/SDK
```

## Debian 环境准备

### 1. 基础工具

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  git \
  pkg-config \
  dpkg-dev
```

`dpkg-dev` 提供 `dpkg-deb`，用于生成 Debian 包。

### 2. Desktop 本地预览依赖

Desktop 模式用于本机 SDL/LVGL 预览，不会启用 CM0 的 Linux framebuffer/DRM、libcamera 和 ALSA 交叉依赖。

```bash
sudo apt install -y \
  libsdl2-dev \
  libfreetype-dev \
  libpng-dev \
  zlib1g-dev
```

### 3. CM0 / arm64 交叉编译依赖

如果在 Debian x86_64 上交叉编译 arm64，需要先开启 arm64 multiarch：

```bash
sudo dpkg --add-architecture arm64
sudo apt update
```

然后安装交叉编译器和 arm64 依赖：

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

CameraApp 目前按 libcamera `0.7+` 配置。如果当前 Debian 源没有足够新的 libcamera，可以使用目标板 sysroot 或工程内 `static_lib/usr`，并在 CMake 或打包脚本中传入：

```bash
-DCM0_SYSROOT_HINT=/path/to/sysroot/usr
```

## CMake 编译

以下命令不包含 test 目标，适合只上传应用源码、不上传 `test/` 目录的场景。

### Desktop 编译

```bash
cd /path/to/debian_workspace/projects/CameraApp
cmake -S . -B build-desktop-local -DUSE_DESKTOP=ON
cmake --build build-desktop-local --target camera_app --parallel 4
```

运行 desktop 版本：

```bash
./build-desktop-local/camera_app
```

### CM0 / arm64 编译

```bash
cd /path/to/debian_workspace/projects/CameraApp
cmake -S . -B build-cm0-local -DUSE_DESKTOP=OFF
cmake --build build-cm0-local --target camera_app --parallel 4
```

如果需要显式指定 sysroot：

```bash
cmake -S . -B build-cm0-local \
  -DUSE_DESKTOP=OFF \
  -DCM0_SYSROOT_HINT=/path/to/sysroot/usr
cmake --build build-cm0-local --target camera_app --parallel 4
```

如果当前构建目录来自另一台机器或另一条挂载路径，CMake 可能报 source/cache 路径不匹配。此时重新生成构建目录：

```bash
rm -rf build-cm0-local
cmake -S . -B build-cm0-local -DUSE_DESKTOP=OFF
cmake --build build-cm0-local --target camera_app --parallel 4
```

## Debian 打包

推荐使用仓库内脚本打包，它会先构建 `camera_app`，然后 staging 安装文件，最后用 `dpkg-deb` 生成 `.deb`。

```bash
cd /path/to/debian_workspace/projects/CameraApp
CLEAN_BUILD=1 ./package_deb.sh
```

默认输出：

```text
dist/CameraApp_0.1.0_m5stack1_arm64.deb
```

可选参数：

```bash
PACKAGE_VERSION=0.1.1 ./package_deb.sh
PACKAGE_SUFFIX=m5stack1 ./package_deb.sh
DEB_ARCH=arm64 ./package_deb.sh
CM0_SYSROOT_HINT=/path/to/sysroot/usr ./package_deb.sh
CAMERA_APP_USE_DRM=OFF CAMERA_APP_USE_ALSA=OFF ./package_deb.sh
```

检查包内容：

```bash
dpkg-deb -c dist/CameraApp_0.1.0_m5stack1_arm64.deb
```

至少应包含这些路径：

```text
/usr/share/APPLaunch/bin/M5CardputerZero-CameraApp
/usr/share/APPLaunch/applications/camera.desktop
/usr/share/APPLaunch/share/images/camera_100.png
/usr/share/CameraApp/assets/fonts/...
/usr/share/CameraApp/assets/audio/...
```

在目标板安装：

```bash
sudo apt install ./dist/CameraApp_0.1.0_m5stack1_arm64.deb
```

或者复制到目标板后安装：

```bash
scp dist/CameraApp_0.1.0_m5stack1_arm64.deb pi@pi:~/
ssh pi@pi 'sudo apt install ./CameraApp_0.1.0_m5stack1_arm64.deb'
```

## 安装后的资源路径

打包安装后，应用使用以下路径：

```text
/usr/share/APPLaunch/bin/M5CardputerZero-CameraApp
/usr/share/APPLaunch/applications/camera.desktop
/usr/share/APPLaunch/share/images/camera_100.png
/usr/share/CameraApp/assets/fonts
/usr/share/CameraApp/assets/audio
```

运行时也可以通过环境变量覆盖 asset root：

```bash
CAMERA_APP_ASSET_DIR=/custom/assets /usr/share/APPLaunch/bin/M5CardputerZero-CameraApp
```
