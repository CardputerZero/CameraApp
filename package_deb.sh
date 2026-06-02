#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-cm0-local}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
PACKAGE_NAME="${PACKAGE_NAME:-m5cardputerzero-camera}"
PACKAGE_VERSION="${PACKAGE_VERSION:-0.1.0}"
DEB_ARCH="${DEB_ARCH:-arm64}"
MAINTAINER="${MAINTAINER:-kane}"
PARALLEL="${PARALLEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
STAGE_DIR="${STAGE_DIR:-${BUILD_DIR}/deb-root}"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"

if ! command -v dpkg-deb >/dev/null 2>&1; then
    echo "dpkg-deb not found. Install dpkg-dev or run this script on Debian/Raspberry Pi OS." >&2
    exit 1
fi

CMAKE_ARGS=(
    -S "${ROOT_DIR}"
    -B "${BUILD_DIR}"
    -DUSE_DESKTOP=OFF
    -DCMAKE_INSTALL_PREFIX=/usr
    "-DCAMERA_APP_USE_DRM=${CAMERA_APP_USE_DRM:-ON}"
    "-DCAMERA_APP_USE_ALSA=${CAMERA_APP_USE_ALSA:-ON}"
    "-DCAMERA_APP_PACKAGE_VERSION=${PACKAGE_VERSION}"
    "-DCAMERA_APP_DEB_ARCHITECTURE=${DEB_ARCH}"
    "-DLIBCAMERA_MIN_VERSION=${LIBCAMERA_MIN_VERSION:-0.7}"
)

if [[ -n "${CM0_SYSROOT_HINT:-}" ]]; then
    CMAKE_ARGS+=("-DCM0_SYSROOT_HINT=${CM0_SYSROOT_HINT}")
fi
if [[ -n "${CAMERA_APP_DEB_DEPENDS:-}" ]]; then
    CMAKE_ARGS+=("-DCAMERA_APP_DEB_DEPENDS=${CAMERA_APP_DEB_DEPENDS}")
fi

for CACHE_DIR in build-desktop-local build-desktop; do
    if [[ -d "${ROOT_DIR}/${CACHE_DIR}/_deps/fmt-src" ]]; then
        CMAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_FMT=${ROOT_DIR}/${CACHE_DIR}/_deps/fmt-src")
        break
    fi
done
for CACHE_DIR in build-desktop-local build-desktop; do
    if [[ -d "${ROOT_DIR}/${CACHE_DIR}/_deps/cjson-src" ]]; then
        CMAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_CJSON=${ROOT_DIR}/${CACHE_DIR}/_deps/cjson-src")
        break
    fi
done

if [[ "${CLEAN_BUILD}" == "1" ]]; then
    rm -rf "${BUILD_DIR}"
fi

cmake "${CMAKE_ARGS[@]}" "$@"
cmake --build "${BUILD_DIR}" --target camera_app --parallel "${PARALLEL}"

EXECUTABLE=""
for CANDIDATE in "${BUILD_DIR}/camera_app" "${BUILD_DIR}/M5CardputerZero-CameraApp"; do
    if [[ -x "${CANDIDATE}" ]]; then
        EXECUTABLE="${CANDIDATE}"
        break
    fi
done
if [[ -z "${EXECUTABLE}" ]]; then
    echo "Executable not found in ${BUILD_DIR}" >&2
    exit 1
fi

DEB_DEPENDS="${CAMERA_APP_DEB_DEPENDS:-}"
if [[ -z "${DEB_DEPENDS}" && -f "${BUILD_DIR}/CPackConfig.cmake" ]]; then
    DEB_DEPENDS="$(sed -n 's/^set(CPACK_DEBIAN_PACKAGE_DEPENDS "\\(.*\\)")$/\\1/p' "${BUILD_DIR}/CPackConfig.cmake" | head -n 1)"
fi
if [[ -z "${DEB_DEPENDS}" ]]; then
    DEB_DEPENDS="libc6, libstdc++6, libgcc-s1, libfreetype6, libpng16-16, libjpeg62-turbo, zlib1g, libcamera0.7, libcamera-ipa"
fi

rm -rf "${STAGE_DIR}"
cmake --install "${BUILD_DIR}" --prefix "${STAGE_DIR}/usr" --component CameraApp

# Keep this explicit copy so the Debian package cannot miss the launcher binary.
install -Dm755 "${EXECUTABLE}" "${STAGE_DIR}/usr/share/APPLaunch/bin/M5CardputerZero-CameraApp"
install -Dm644 "${ROOT_DIR}/assets/applications/camera.desktop" "${STAGE_DIR}/usr/share/APPLaunch/applications/camera.desktop"
install -Dm644 "${ROOT_DIR}/assets/images/camera_100.png" "${STAGE_DIR}/usr/share/APPLaunch/share/images/camera_100.png"

CONTROL_DIR="${STAGE_DIR}/DEBIAN"
mkdir -p "${CONTROL_DIR}" "${DIST_DIR}"
INSTALLED_SIZE="$(du -sk "${STAGE_DIR}/usr" | awk '{print $1}')"
cat > "${CONTROL_DIR}/control" <<EOF
Package: ${PACKAGE_NAME}
Version: ${PACKAGE_VERSION}
Section: utils
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: ${MAINTAINER}
Depends: ${DEB_DEPENDS}
Installed-Size: ${INSTALLED_SIZE}
Description: Camera application for M5CardputerZero APPLaunch
 Camera application, launcher entry, and runtime assets.
EOF

DEB_PATH="${DIST_DIR}/${PACKAGE_NAME}_${PACKAGE_VERSION}_${DEB_ARCH}.deb"
dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${DEB_PATH}"

echo "Generated Debian package: ${DEB_PATH}"
