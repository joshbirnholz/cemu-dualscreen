#!/usr/bin/env bash
#
# Packages an already-built bin/Cemu_release (see build.sh) into a single
# portable AppImage -- the actual answer to "run this on the Deck without a
# container." An AppImage bundles GTK3 and Cemu's other runtime libraries
# into the file itself, so the Deck just runs it directly: no distrobox, no
# podman, no docker, nothing. It still uses the host's own kernel and GPU
# driver for Vulkan/OpenGL through /dev/dri, the same as every native Steam
# game already does -- that part was never what needed containing; only the
# build-time compiler/glibc mismatch did.
#
# Reuses Cemu upstream's own packaging script (dist/linux/appimage.sh,
# unmodified) inside the same pinned environment build.sh uses, the same
# way upstream's own CI does it (.github/workflows/build.yml's
# build-appimage job) -- just as one local step instead of two CI jobs
# passing an artifact between them.
#
# Usage: docker/package-appimage.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_TAG="cemu-dualscreen-buildenv:22.04-clang15"

cd "$REPO_ROOT"

if [ ! -f bin/Cemu_release ]; then
    echo "bin/Cemu_release not found -- run docker/build.sh first." >&2
    exit 1
fi

docker build -t "$IMAGE_TAG" "$SCRIPT_DIR"

mkdir -p "$REPO_ROOT/docker/.home"
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/home-cache \
    -e LD_LIBRARY_PATH=/usr/local/lib \
    -e DEPLOY_GTK_VERSION=3 \
    -v "$REPO_ROOT/docker/.home":/home-cache \
    -v "$REPO_ROOT":/workspace \
    -w /workspace \
    "$IMAGE_TAG" \
    bash -c '
        set -euo pipefail
        rm -rf AppDir artifacts
        # appimage.sh (upstream, unmodified) expects the binary named plainly
        # "Cemu" -- see how its own CI renames it the same way before calling
        # this script. A copy, not a rename, so bin/Cemu_release is still
        # there afterwards for anyone iterating with build.sh alone.
        cp bin/Cemu_release bin/Cemu
        dist/linux/appimage.sh X64
        rm -f bin/Cemu
    '

APPIMAGE="$(find "$REPO_ROOT/artifacts" -maxdepth 1 -name "*.AppImage" | head -n1)"
if [ -z "$APPIMAGE" ]; then
    echo "appimage.sh finished but no .AppImage was produced -- check the output above." >&2
    exit 1
fi
chmod +x "$APPIMAGE"
echo "Built: $APPIMAGE"
echo "Copy this ONE file to the Deck and run it there directly -- no container needed."
