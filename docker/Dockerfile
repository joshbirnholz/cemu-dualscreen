# The build environment, and nothing else -- no source in here, no vcpkg
# cache, no compiled output. Those all live in the repo checkout, which is
# bind-mounted in at build time (see build.sh); this image is only the OS,
# the compiler and the libraries Cemu's own BUILD.md asks for.
#
# SteamOS's own root filesystem is read-only by design, so a full C++ dev
# toolchain (clang-15, cmake, vcpkg's own dependencies, ...) was never going
# to live there directly without `steamos-readonly disable` and the
# fragility that comes with it -- the next OS update either fights it or
# wipes it. This Dockerfile is that writable surface instead, defined in a
# file checked into git rather than hand-installed into a container that
# then quietly drifts (a `distrobox upgrade`, a rolling base image moving
# on) the way the one that broke before did. Rebuilding it from scratch, on
# either machine, is `podman build`, not "remember what you did last time."
#
# Pinned to Ubuntu 22.04 rather than a rolling base or a "latest" tag on top
# of that: 22.04's apt repos are a frozen snapshot (security patches only,
# never a package bump that changes ABI), and its glibc (2.35) is older
# than any realistic target's, which is what makes the binary this produces
# portable to begin with -- glibc only promises forward compatibility, so
# building against a deliberately old, frozen one means the result runs
# anywhere newer without having to check.
FROM docker.io/library/ubuntu:22.04

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
