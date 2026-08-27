#!/usr/bin/env bash
#
# Builds Cemu inside the pinned environment in docker/Dockerfile, so the
# result is the same regardless of which machine ran this -- your PC, the
# Deck, whichever. Replaces the old "distrobox create, then remember what
# you installed into it" workflow: the environment is this Dockerfile,
# checked into git, and `podman build` reproduces it byte-for-byte instead
# of trusting that two hand-built containers stayed in sync.
#
# Uses podman, not docker: SteamOS ships podman already (distrobox is built
# on it), so there is nothing to install and nothing that needs
# steamos-readonly disabled -- this runs exactly as a normal user, no root.
#
# Usage: docker/build.sh [extra cmake args...]
#   docker/build.sh                              # release build (default)
#   docker/build.sh -DCMAKE_BUILD_TYPE=debug      # debug build
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_TAG="cemu-dualscreen-buildenv:22.04-clang15"

cd "$REPO_ROOT"

# 1. The environment. Cached by podman after the first run -- this is a
# no-op unless docker/Dockerfile itself changed.
podman build -t "$IMAGE_TAG" "$SCRIPT_DIR"

# 2. The working tree. Plain git, on the host, so it uses your own SSH key
# rather than needing one handed into the container.
git submodule update --init --recursive

# Extra arguments (e.g. -DCMAKE_BUILD_TYPE=debug) pass straight through to
# cmake's configure step; release is the default when none are given.
CMAKE_EXTRA_ARGS=("-DCMAKE_BUILD_TYPE=release")
if [ "$#" -gt 0 ]; then
    CMAKE_EXTRA_ARGS=("$@")
fi

# 3. The compile, inside the pinned environment. The repo is bind-mounted
# rather than copied in, so build/ and vcpkg's own package cache persist
# across runs exactly like any other incremental build. Runs as the
# invoking user (not root) so the files it writes are yours afterwards, and
# HOME points at a repo-local, gitignored directory so vcpkg's binary cache
# survives between runs without needing anything writable outside the repo.
mkdir -p "$REPO_ROOT/docker/.home"
podman run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/home-cache \
    -v "$REPO_ROOT/docker/.home":/home-cache \
    -v "$REPO_ROOT":/workspace \
    -w /workspace \
    "$IMAGE_TAG" \
    bash -c '
        set -euo pipefail
        cmake -S . -B build \
            -DCMAKE_C_COMPILER=/usr/bin/clang-15 \
            -DCMAKE_CXX_COMPILER=/usr/bin/clang++-15 \
            -G Ninja \
            -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
            "$@"
        cmake --build build
    ' bash "${CMAKE_EXTRA_ARGS[@]}"

BUILD_TYPE="release"
for arg in "${CMAKE_EXTRA_ARGS[@]}"; do
    case "$arg" in
        -DCMAKE_BUILD_TYPE=*) BUILD_TYPE="${arg#-DCMAKE_BUILD_TYPE=}" ;;
    esac
done
BINARY="$REPO_ROOT/bin/Cemu_$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
if [ ! -f "$BINARY" ]; then
    echo "Build finished but $BINARY was not produced -- check the output above." >&2
    exit 1
fi
echo "Built: $BINARY"
