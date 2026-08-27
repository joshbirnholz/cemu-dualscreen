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
# libudev-dev + appstream -- the two packages upstream's own CI adds on top
# of BUILD.md's list for the compile and the AppImage packaging steps
# respectively (see .github/workflows/build.yml) -- and build-essential +
# linux-libc-dev.
#
# That last pair, and autoconf/automake/autoconf-archive/m4/gettext below,
# aren't in BUILD.md or CI's list at all: both assume a machine that already
# has basic build tools on it (GitHub Actions' ubuntu-22.04 runners ship
# build-essential, autoconf and friends preinstalled), which a from-scratch
# ubuntu:22.04 container does not. Individual vcpkg ports build themselves
# with plain `make`/`cc`/autotools, separately from the clang-15 pin below
# that only governs Cemu's own top-level build -- openssl needed make and
# libc headers, libusb needs autoconf/automake/autoconf-archive to
# regenerate its build scripts (vcpkg_run_autoreconf); m4 and gettext ride
# along since they're what autoreconf itself commonly reaches for on other
# ports before it fails on the same class of error one port at a time.
#
# llvm-15 is the same story one level up, for Cemu's own build rather than
# a vcpkg port: apt's clang-15 package is only the compiler frontend, not
# the matching llvm-ar-15/llvm-ranlib-15 CMake wants to archive a static
# library with a Clang toolchain. Without it those come back
# CMAKE_C_COMPILER_AR-NOTFOUND at configure time -- and because that gets
# cached in build/CMakeCache.txt, fixing this needs a fresh configure, not
# just a rerun of cmake --build (delete build/ once after this change).
#
# No runtime GPU/audio packages here: nothing built from this image is
# meant to run inside it. See docker/package-appimage.sh -- the output is a
# single portable file that runs directly against the Deck's own kernel and
# GPU driver, no container involved at that end at all.
RUN apt-get update && apt-get install -y --no-install-recommends \
        appstream \
        autoconf \
        autoconf-archive \
        automake \
        build-essential \
        ca-certificates \
        clang-15 \
        cmake \
        curl \
        freeglut3-dev \
        gettext \
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
        linux-libc-dev \
        llvm-15 \
        m4 \
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
