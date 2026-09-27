#!/usr/bin/env bash
# Build a portable Linux AppImage of otd inside Docker.
# Output: dist/otd-<version>-x86_64.AppImage
set -euo pipefail
cd "$(dirname "$0")"
IMAGE=otd-build-appimage:jammy
VERSION=$(sed -n 's/^project(otd VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)

docker build -q -f docker/Dockerfile.appimage -t "$IMAGE" docker >/dev/null
mkdir -p dist
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/src -w /src "$IMAGE" bash -c "
  set -e
  cmake -S . -B build-appimage -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=\$QT_DIR -DOTD_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build-appimage
  rm -rf build-appimage/AppDir
  DESTDIR=build-appimage/AppDir cmake --install build-appimage
  cd build-appimage
  export VERSION=$VERSION
  export LD_LIBRARY_PATH=\$QT_DIR/lib
  export QML_SOURCES_PATHS=/nonexistent
  export EXTRA_QT_MODULES=
  # the KiwiSDR player needs the multimedia backend plugin (and the ffmpeg
  # libraries that come with Qt's own build)
  export EXTRA_QT_PLUGINS=multimedia
  export EXTRA_PLATFORM_PLUGINS=libqwayland-generic.so\;libqwayland-egl.so
  linuxdeploy-x86_64.AppImage --appdir AppDir --plugin qt --output appimage \
      --desktop-file AppDir/usr/share/applications/otd.desktop \
      --icon-file AppDir/usr/share/icons/hicolor/scalable/apps/otd.svg
  mv *.AppImage ../dist/otd-$VERSION-x86_64.AppImage
"
ls -la dist/otd-$VERSION-x86_64.AppImage
