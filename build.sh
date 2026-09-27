#!/usr/bin/env bash
# Build otd (On The Dial) inside a Docker container. Nothing is installed on the host;
# the only outputs are the docker image "otd-build" and ./build/.
#
#   ./build.sh            configure + build + run unit tests
#   ./build.sh --no-test  skip the tests
#   ./build.sh --clean    wipe ./build first
#
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=otd-build:trixie
RUN_TESTS=1
for arg in "$@"; do
  case "$arg" in
    --no-test) RUN_TESTS=0 ;;
    --clean)   rm -rf build ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

docker build -q -t "$IMAGE" docker >/dev/null

# Run as the calling user so ./build is not owned by root.
docker run --rm \
  -u "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$PWD":/src \
  -w /src \
  "$IMAGE" \
  bash -c "
    set -e
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    if [ $RUN_TESTS = 1 ]; then
      cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
    fi
  "

echo
echo "Built: $PWD/build/otd"
