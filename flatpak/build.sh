#!/usr/bin/env bash
# Build, install and lint the Flatpak the way Flathub does, inside a Docker
# container. The Flatpak store (runtimes, SDK, results) lives in ./.flatpak, so
# nothing is installed on the host.
#   flatpak/build.sh          build from the working tree
#   flatpak/build.sh --tag    build exactly what Flathub would (git tag from the manifest)
#   flatpak/build.sh --run    start the installed Flatpak (needs a host flatpak; see below)
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=otd-build-flatpak:trixie
MANIFEST=flatpak/eu.oh2gba.otd.yml
mkdir -p .flatpak build-flatpak

if [ "${1:-}" = "--run" ]; then
  # The host's flatpak can use the same store through FLATPAK_USER_DIR.
  FLATPAK_USER_DIR="$PWD/.flatpak" exec flatpak run eu.oh2gba.otd
fi

if [ "${1:-}" = "--tag" ]; then
  USE=$MANIFEST
else
  # must sit in the project root: sandboxed builds only read below the manifest
  USE=eu.oh2gba.otd.local.yml
  python3 flatpak/local-manifest.py "$MANIFEST" "$USE"
fi

docker build -q -f docker/Dockerfile.flatpak -t "$IMAGE" docker >/dev/null
RUNTIME=$(sed -n "s/^runtime-version: '\(.*\)'/\1/p" "$MANIFEST")

# The container user's default Flatpak location points at ./.flatpak, so both
# the host tools and Flathub's own flatpak-builder (from org.flatpak.Builder)
# see the same store.
mkdir -p build-flatpak/home/.local/share
ln -sfn /src/.flatpak build-flatpak/home/.local/share/flatpak
# D-Bus and fusermount want a passwd entry for the build user.
printf 'root:x:0:0:root:/root:/bin/bash\nbuilder:x:%s:%s:builder:/src/build-flatpak/home:/bin/bash\n' "$(id -u)" "$(id -g)" > build-flatpak/passwd
printf 'root:x:0:\nbuilder:x:%s:\n' "$(id -g)" > build-flatpak/group
mkdir -p -m 700 build-flatpak/run
docker run --rm --privileged -u "$(id -u):$(id -g)" \
  -e HOME=/src/build-flatpak/home \
  -v "$PWD/build-flatpak/passwd":/etc/passwd:ro -v "$PWD/build-flatpak/group":/etc/group:ro \
  -v "$PWD/build-flatpak/run":/run/user/$(id -u) -e XDG_RUNTIME_DIR=/run/user/$(id -u) \
  -v "$PWD":/src -w /src "$IMAGE" dbus-run-session -- bash -c "
  set -e
  flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
  flatpak --user install -y --noninteractive flathub org.kde.Platform//$RUNTIME org.kde.Sdk//$RUNTIME org.flatpak.Builder >/dev/null
  echo '--- lint manifest'
  flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest $MANIFEST
  echo '--- build (flathub-build wrapper, as Flathub runs it)'
  flatpak run --command=flathub-build org.flatpak.Builder --disable-rofiles-fuse --state-dir=.flatpak-builder $USE
  echo '--- lint repo'
  flatpak run --command=flatpak-builder-lint org.flatpak.Builder repo repo
"

# Install the result into the project-local store with the host's flatpak
# (inside the container there is no session bus for the installer).
export FLATPAK_USER_DIR="$PWD/.flatpak"
flatpak --user remote-add --no-gpg-verify --if-not-exists otd-local "$PWD/repo"
flatpak --user install -y --noninteractive --reinstall otd-local eu.oh2gba.otd >/dev/null
echo
echo "Installed into ./.flatpak. Start it with:  flatpak/build.sh --run"
