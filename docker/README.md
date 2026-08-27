# A reproducible build environment

`docker/Dockerfile` is the whole point: it is Cemu's own
[BUILD.md](../BUILD.md) Debian/Ubuntu recipe, pinned to Ubuntu 22.04's
frozen apt snapshot, checked into git. Whichever machine runs
`docker build` against it -- your PC, the Deck, either -- gets the exact
same compiler and libraries, because the image is defined by a file in this
repo rather than by whatever got installed into a distrobox container by
hand on that particular machine at some particular time.

That is also the actual fix for the crash that started this: a binary is
only ever forward-compatible with glibc, never backward, so building
against 22.04's deliberately-older, frozen glibc guarantees the result runs
on SteamOS's newer one -- and does so identically whether the PC or the
Deck was the one that built it.

## Building

```
./docker/build.sh
```

This builds the image (a no-op after the first run, unless the Dockerfile
itself changed), updates submodules, and runs the actual `cmake`/`ninja`
build inside a container with this repo bind-mounted in -- so `build/` and
vcpkg's package cache behave exactly like a normal incremental build across
runs, they just happen inside the pinned environment rather than whatever
the host has installed.

Output lands in the usual place: `bin/Cemu_release`. That is the one file
to copy to the Deck -- see the root [BUILD.md](../BUILD.md) note on that;
nothing else in `bin/` is a build product.

Extra arguments pass straight through to cmake's configure step, e.g. for a
debug build:

```
./docker/build.sh -DCMAKE_BUILD_TYPE=debug
```

## Running

The image has the runtime GL/Vulkan and audio packages installed too, so it
can double as the container that actually launches Cemu, not just the one
that builds it -- one pinned environment for both, rather than a build-time
one and a separately-maintained run-time one that can drift from it the
same way distrobox's did.

**Recommended: point distrobox at this image instead of an upstream one.**
distrobox itself is not the problem -- its device, socket and controller
passthrough for a GUI, GPU-using app is real work that is worth not
reimplementing by hand. The problem was always *which* image it wrapped:
`distrobox create --image archlinux:latest` (or similar) re-resolves
"latest" independently on every machine, which is exactly the drift that
caused the mismatch. Pointing it at the image this Dockerfile builds keeps
all of that convenience while fixing the actual bug:

```
docker/build.sh                 # make sure the image exists locally first
distrobox create --image cemu-dualscreen-buildenv:22.04-clang15 --name cemu-dualscreen
distrobox enter cemu-dualscreen -- ~/cemu-dualscreen/bin/Cemu_release
```

Both machines building from the same `docker/Dockerfile` is what keeps this
in sync from here on: change the Dockerfile, rebuild the image on whichever
machine, `distrobox create` (or `--replace` an existing container) from it
again there.

**Without distrobox**, a bare `podman run`/`docker run` needs the display,
GPU and audio handed in explicitly -- roughly:

```
podman run --rm -it \
    --device /dev/dri \
    --group-add video --group-add render \
    -e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix \
    -e XDG_RUNTIME_DIR -e WAYLAND_DISPLAY \
    -v "$XDG_RUNTIME_DIR":"$XDG_RUNTIME_DIR" \
    -v ~/cemu-dualscreen/bin:/cemu -w /cemu \
    cemu-dualscreen-buildenv:22.04-clang15 \
    ./Cemu_release
```

This is missing controller/USB passthrough (`/dev/input`, `/dev/bus/usb`,
or a udev rule, depending on how the pad is connected) and any Wayland- vs
X11-specific quirks distrobox already papers over, so it is offered as a
reference rather than the recommended path -- expect to add flags for
whatever a first run turns up missing.

## Updating the pin

Bumping the base image (a newer Ubuntu release, a different clang version)
is a deliberate edit to `docker/Dockerfile`, committed like any other
change -- never an automatic "latest" re-resolve. After a change like that,
rebuild the image on every machine that uses it (`docker/build.sh` does
this on its own the next time it runs) so they stay identical rather than
each drifting toward whatever it happened to have cached.
