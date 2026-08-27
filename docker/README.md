# A reproducible build environment

`docker/Dockerfile` is the whole point: it is Cemu's own
[BUILD.md](../BUILD.md) Debian/Ubuntu recipe, pinned to Ubuntu 22.04's
frozen apt snapshot, checked into git. Whichever machine runs
`podman build` against it -- your PC, the Deck, either -- gets the exact
same compiler and libraries, because the image is defined by a file in this
repo rather than by whatever got installed into a distrobox container by
hand on that particular machine at some particular time.

It uses **podman**, not docker: SteamOS ships podman already (distrobox is
built on it), so there's nothing to install on either machine, and nothing
that needs `steamos-readonly disable` -- this all runs as a normal user, no
root, no touching the protected filesystem. That's the actual reason a
pinned container is worth having here at all: SteamOS's root is read-only
by design, so a C++ dev toolchain was never going to live there directly
without exactly the kind of fragility (an OS update fighting it, a
`distrobox upgrade` silently dropping a package) that broke the build that
started this. A Dockerfile checked into git can't drift like that --
rebuilding the environment from scratch is one command, not "remember what
you installed by hand."

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

## Packaging: turning that into one file to copy to the Deck

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
native Steam game already does.

The result lands under `artifacts/Cemu-<version>-x86_64.AppImage`. Copy
that one file to the Deck (scp, a USB stick, a synced folder, whatever) and
run it there directly:

```
./Cemu-<version>-x86_64.AppImage --dualscreen-socket /run/user/1000/dualscreen-external.sock -g game.wud
```

or add it to Steam as a non-Steam game / launch it from Desktop Mode like
any other portable Linux app. No container, no distrobox, no setup step on
the Deck at all beyond having the file.

## The whole thing, start to finish

```
./docker/build.sh              # compile
./docker/package-appimage.sh   # package into one portable file
# copy artifacts/Cemu-*-x86_64.AppImage to the Deck, run it there
```

Every later `git pull` just means running both again -- `podman build`
skips the rebuild if `docker/Dockerfile` hasn't changed, so in practice
this is `git pull && ./docker/build.sh && ./docker/package-appimage.sh`.

## Updating the pin

Bumping the base image (a newer Ubuntu release, a different clang version)
is a deliberate edit to `docker/Dockerfile`, committed like any other
change -- never an automatic "latest" re-resolve. Since building and
packaging both only ever happen inside this one image, there is nothing
else that needs to stay in sync with it.
