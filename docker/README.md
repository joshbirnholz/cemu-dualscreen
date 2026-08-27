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

This container is for **building only**. Nothing it produces needs a
container to *run* -- see Packaging, below.

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

Output lands in the usual place: `bin/Cemu_release`. Extra arguments pass
straight through to cmake's configure step, e.g. for a debug build:

```
./docker/build.sh -DCMAKE_BUILD_TYPE=debug
```

## Packaging (the part that gets rid of the Deck-side container)

```
./docker/package-appimage.sh
```

This wraps `bin/Cemu_release` into a single `.AppImage` file, using Cemu
upstream's own packaging script (`dist/linux/appimage.sh`, unmodified --
the same one their real CI runs) inside the same pinned environment. An
AppImage bundles GTK3 and Cemu's other runtime libraries *into the file
itself*, so it needs nothing installed on the machine that runs it -- no
distrobox, no podman, no docker, nothing. It still renders through the
host's own kernel and GPU driver via `/dev/dri`, exactly the way every
native Steam game already does; that was never the part that needed
containing, only the compiler/glibc mismatch was.

**The one file to copy to the Deck is the `.AppImage`** this produces
(under `artifacts/`). On the Deck:

```
chmod +x Cemu-<version>-x86_64.AppImage   # package-appimage.sh already does this once; harmless to repeat
./Cemu-<version>-x86_64.AppImage --dualscreen-socket /run/user/1000/dualscreen-external.sock -g game.wud
```

or add it to Steam as a non-Steam game / launch it from Desktop Mode like
any other portable Linux app. No container, no distrobox, no setup step on
the Deck at all beyond having the file.

## Updating the pin

Bumping the base image (a newer Ubuntu release, a different clang version)
is a deliberate edit to `docker/Dockerfile`, committed like any other
change -- never an automatic "latest" re-resolve. Since building and
packaging both only ever happen inside this one image, there is nothing
else that needs to stay in sync with it.
