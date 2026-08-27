# The build environment, and nothing else -- no source in here, no vcpkg
# cache, no compiled output. Those all live in the repo checkout, which is
# bind-mounted in at build time (see build.sh); this image is only the OS,
# the compiler and the libraries Cemu's own BUILD.md asks for.
#
# Pinned to Ubuntu 22.04 rather than a rolling base or a "latest" tag, on
# purpose: 22.04's apt repos are a frozen snapshot (security patches only,
# never a package bump that changes ABI), so this Dockerfile produces the
# same toolchain today, next year, and on every machine that builds it --
# which is the whole point of replacing distrobox here. distrobox's own
# container is created by hand on each machine and drifts as each one pulls
# whatever "latest" happened to mean that day; this image is versioned in
# git instead; rebuilding it is `docker build`, not "remember what you did
# last time."
#
# 22.04 also happens to be *older* than SteamOS's own glibc, which is the
# actual fix for the crash that started this: glibc only promises forward
# compatibility (an old-glibc binary runs fine on a newer glibc, never the
# reverse), so building against a deliberately older, frozen base is what
# guarantees the binary this produces runs on the Deck no matter which
# machine built it.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# The exact package list from BUILD.md's "Debian, Ubuntu and derivatives"
# section, plus libusb-1.0-0-dev (BUILD.md's own noted vcpkg/hidapi
# workaround) and the handful vcpkg itself needs to fetch and unpack its own
# dependencies (ca-certificates, curl, zip/unzip/tar, pkg-config, python3).
#
# The last four (libvulkan1, mesa-utils, mesa-vulkan-drivers, vulkan-tools)
# are not needed to build Cemu, only to run it -- so this same image can
# also be the container the Deck actually launches, rather than needing a
# second, separately-maintained one. The container's own userspace
# GL/Vulkan loader still has to be in here even though the kernel driver
# comes from the host through /dev/dri; distrobox does not inject that for
# you.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-15 \
        cmake \
        curl \
        freeglut3-dev \
        git \
        libbluetooth-dev \
        libgcrypt20-dev \
        libglm-dev \
        libgtk-3-dev \
        libpulse-dev \
        libsecret-1-dev \
        libsystemd-dev \
        libtool \
        libusb-1.0-0-dev \
        nasm \
        ninja-build \
        pkg-config \
        python3 \
        tar \
        unzip \
        zip \
        libvulkan1 \
        mesa-utils \
        mesa-vulkan-drivers \
        vulkan-tools \
    && rm -rf /var/lib/apt/lists/*

ENV CC=/usr/bin/clang-15
ENV CXX=/usr/bin/clang++-15

WORKDIR /workspace
