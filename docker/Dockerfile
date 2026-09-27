# Build environment for otd (otd) on Linux.
# Debian 13 (trixie) ships Qt 6.8, the same version as the host this was
# developed on, so the resulting binary runs natively on such a host.
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite \
        qt6-websockets-dev qt6-multimedia-dev \
        libgl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
