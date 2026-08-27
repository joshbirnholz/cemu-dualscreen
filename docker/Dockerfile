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
# workaround), the handful vcpkg itself needs to fetch and unpack its own
# dependencies (ca-certificates, curl, zip/unzip/tar, pkg-config, python3),
# and libudev-dev + appstream -- the two packages upstream's own CI adds on
# top of BUILD.md's list for the compile and the AppImage packaging steps
# respectively (see .github/workflows/build.yml). No runtime GPU/audio
# packages here: nothing built from this image is meant to run inside it.
# See docker/package-appimage.sh -- the output is a single portable file
# that runs directly against the Deck's own kernel and GPU driver, no
# container involved at that end at all.
RUN apt-get update && apt-get install -y --no-install-recommends \
        appstream \
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
        libudev-dev \
        libusb-1.0-0-dev \
        nasm \
        ninja-build \
        pkg-config \
        python3 \
        tar \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

ENV CC=/usr/bin/clang-15
ENV CXX=/usr/bin/clang++-15

WORKDIR /workspace
