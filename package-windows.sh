#!/usr/bin/env bash
# Cross-build the Windows 64-bit packages of otd inside Docker.
# Output: dist/otd-<version>-windows-x64.zip (portable folder) and
# dist/otd-<version>-setup.exe (NSIS installer), both containing otd.exe, the
# Qt runtime and a copy of Hamlib's rigctld (LGPL) so nothing else is needed.
set -euo pipefail
cd "$(dirname "$0")"
IMAGE=otd-build-windows:trixie
VERSION=$(sed -n 's/^project(otd VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
HAMLIB_VERSION=${HAMLIB_VERSION:-4.7.2}
HAMLIB_URL="https://github.com/Hamlib/Hamlib/releases/download/${HAMLIB_VERSION}/hamlib-w64-${HAMLIB_VERSION}.zip"

docker build -q -f docker/Dockerfile.windows -t "$IMAGE" docker >/dev/null
mkdir -p dist build-windows/cache
if [ ! -f "build-windows/cache/hamlib-w64-${HAMLIB_VERSION}.zip" ]; then
  echo "Fetching Hamlib ${HAMLIB_VERSION} for Windows ..."
  curl -sL -o "build-windows/cache/hamlib-w64-${HAMLIB_VERSION}.zip" "$HAMLIB_URL"
fi

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/src -w /src "$IMAGE" bash -c "
  set -e
  cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/mingw-toolchain.cmake \
        -DCMAKE_PREFIX_PATH=\$QT_TARGET -DQT_HOST_PATH=\$QT_HOST \
        -DOTD_BUILD_TESTS=OFF
  cmake --build build-windows

  PKG=build-windows/pkg/otd
  rm -rf build-windows/pkg && mkdir -p \$PKG/platforms \$PKG/sqldrivers \$PKG/styles \$PKG/tls \$PKG/multimedia \$PKG/hamlib
  cp build-windows/otd.exe \$PKG/
  for lib in Qt6Core Qt6Gui Qt6Widgets Qt6Network Qt6Sql Qt6WebSockets Qt6Multimedia; do cp \$QT_TARGET/bin/\$lib.dll \$PKG/; done
  # audio output for the KiwiSDR player: the Windows Media Foundation backend
  # (no ffmpeg DLLs needed)
  cp \$QT_TARGET/plugins/multimedia/windowsmediaplugin.dll \$PKG/multimedia/
  cp \$QT_TARGET/plugins/platforms/qwindows.dll \$PKG/platforms/
  cp \$QT_TARGET/plugins/sqldrivers/qsqlite.dll \$PKG/sqldrivers/
  cp \$QT_TARGET/plugins/styles/qmodernwindowsstyle.dll \$PKG/styles/
  cp \$QT_TARGET/plugins/tls/qschannelbackend.dll \$PKG/tls/
  # MinGW runtime DLLs from the compiler that built otd.exe (newer than the
  # ones Qt was built with, and backward compatible with them)
  for lib in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    src=\$(x86_64-w64-mingw32-g++-posix -print-file-name=\$lib)
    test -f \"\$src\" || { echo \"missing runtime \$lib\" >&2; exit 1; }
    cp \"\$src\" \$PKG/
  done
  x86_64-w64-mingw32-strip \$PKG/otd.exe \$PKG/libstdc++-6.dll \$PKG/libgcc_s_seh-1.dll \$PKG/libwinpthread-1.dll

  # Hamlib: rigctld plus the libraries it needs, and its licence
  cd build-windows/cache && rm -rf hamlib && mkdir hamlib && cd hamlib && 7z x -y ../hamlib-w64-${HAMLIB_VERSION}.zip >/dev/null && cd ../../..
  H=\$(ls -d build-windows/cache/hamlib/hamlib-w64-*)
  for f in rigctld.exe rigctl.exe libhamlib-4.dll libusb-1.0.dll libwinpthread-1.dll libgcc_s_seh-1.dll; do cp \$H/bin/\$f \$PKG/hamlib/; done
  cp \$H/COPYING.LIB.txt \$PKG/hamlib/LICENSE-hamlib.txt
  cp LICENSE \$PKG/LICENSE.txt
  cp third_party/miniz/LICENSE \$PKG/LICENSE-miniz.txt
  cat > \$PKG/README.txt <<'TXT'
otd (On The Dial) for Windows
=============================

This is the portable folder: unzip anywhere and start otd.exe. Everything the
program needs is here, including Hamlib's rigctld (in hamlib\\) for talking to
your radio. There is also an installer, otd-setup.exe, on the download page.

First start: File > Settings > Radio, tick \"Start Hamlib's rigctld when the
program starts\", pick your radio model, the COM port and the baud rate.

Settings and the station database are kept in
  %LOCALAPPDATA%\\onthedial\\otd\\
or, if you prefer a portable setup, start it as:  otd.exe --data-dir .\\data

Documentation: https://otd.oh2gba.eu/
otd is free software (GPL-3.0-or-later). Hamlib is LGPL, see
hamlib\\LICENSE-hamlib.txt; its source is at https://github.com/Hamlib/Hamlib
TXT
  cd build-windows/pkg && rm -f ../../dist/otd-$VERSION-windows-x64.zip && zip -qr ../../dist/otd-$VERSION-windows-x64.zip otd
  cd ../..

  # The installer: File and Delete lines generated from the folder, so
  # nothing is missed and the uninstaller removes exactly what was put there.
  python3 windows/nsis-lists.py build-windows/pkg/otd build-windows/pkg/files.nsh build-windows/pkg/delete.nsh
  makensis -V2 -DVERSION=$VERSION -DPKGDIR=/src/build-windows/pkg/otd -DICON=/src/data/otd.ico \
           -DOUTFILE=/src/dist/otd-$VERSION-setup.exe \
           -DFILELIST=/src/build-windows/pkg/files.nsh -DDELLIST=/src/build-windows/pkg/delete.nsh \
           windows/otd.nsi
"
ls -la dist/otd-$VERSION-windows-x64.zip dist/otd-$VERSION-setup.exe
