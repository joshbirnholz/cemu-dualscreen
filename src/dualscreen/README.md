# The Dualscreen frame link

These files are **vendored**, not written here. They are byte-for-byte copies
from the Dualscreen project, which is the other end of this link:

    dual-screen/src/core/external_frame_link.hpp  ->  core/external_frame_link.hpp
    dual-screen/src/core/external_frame_link.cpp  ->  core/external_frame_link.cpp
    dual-screen/src/layout/screen_id.hpp          ->  layout/screen_id.hpp

They are unmodified on purpose. Re-syncing is a copy and a diff rather than a
merge, and `git diff --no-index` against that tree should be empty. The
directory layout above is why: it keeps the `#include` lines inside them
correct without editing them.

Dualscreen has a test (`external_link_stands_alone`) that compiles exactly these
three files with nothing else of that project on the include path, so what is
here cannot quietly grow a dependency on the rest of it.

## What it is

A Wii U has two screens and Dualscreen puts them on two machines. It does not
emulate a Wii U -- it asks whatever is playing one to hand its pictures over.
That is this.

Pixels go through shared memory, because a television frame and a GamePad frame
together are about five megabytes sixty times a second. Everything else -- the
handshake, audio, the pad, the end of the session -- goes through a Unix socket,
which has the one property shared memory does not: when the far end dies, this
end finds out immediately rather than waiting for frames that never come.

Dualscreen is the consumer and goes first: it makes the memory, it listens, and
it hands this side a file descriptor for that memory over the socket. So there
is no path to agree on and nothing left behind when either side crashes.

## What Cemu adds around it

Nothing in these files. The parts that are Cemu's own live where they belong:

- `Cafe/HW/Latte/Renderer/` -- the frame tap and the readback that feeds it
- `input/emulated/VPADController.cpp` -- taking the pad from the link
- `OS/libs/snd_core/ax_out.cpp` -- sending TV and GamePad audio as their own
  streams and silencing Cemu's own output for both while connected
- `Cafe/CafeSystem.cpp` -- starting and ending the session with the title
- `config/LaunchSettings.cpp` -- `--dualscreen-socket`

## Running it

    Cemu --dualscreen-socket /run/user/1000/dualscreen-external.sock -g game.wud

Dualscreen launches Cemu itself with that argument filled in; the flag is here
so the same thing can be done by hand. With no flag Cemu behaves exactly as it
always has, and with a flag nothing is listening on, it says so once in the log
and plays the game normally.

### Controllers

The shell's controllers reach Cemu as ordinary controllers, under the
**DualScreen** API in Cemu's input settings, and are mapped there like any
other pad:

- **DualScreen GamePad** is the Wii U GamePad: buttons, both sticks, motion and
  rumble, from whichever device the shell says is holding the GamePad. The
  touch screen comes from the link directly, since a button mapping has no way
  to describe a point on a screen. A GamePad is created if this Cemu has never
  had its input configured, since the shell needs one to be holding.
- **DualScreen Player 1** to **Player 8** are the game's other controllers.
  Map each to a Pro Controller, Wii Remote or Classic Controller as the game
  wants; each has a default mapping that passes the buttons straight through.
  The GamePad is not one of them, just as it is not on a Wii U.

Which physical pad feeds which of these is decided by the shell, which can see
every controller on every device, not by Cemu. So a mapping made once keeps
working whatever order Steam lists the pads in. A player nothing is feeding
shows as a controller that is not plugged in. Rumble goes back over the link,
and the shell sends it to whichever pads it assigned to that player.

The shell's pads come through Steam Input on a Steam Deck. Cemu should not
also map the same pads through SDL, or every press would arrive twice.

Vulkan only. On OpenGL the frames are simply not handed over -- see
`Renderer::HandleDualscreenFrame`.

## Not done yet

- **Reset.** The shell's Reset button arrives and is logged, deliberately not
  acted on. Handing the request from the GPU thread (where it arrives) to the
  main thread is buildable, the same way `CafeSystem::RequestRecreateCanvas()`
  already does it. What it would call is not: the only stop-a-running-title
  path Cemu has, `MainWindow::EndEmulation()`, is marked by its own author as
  unfinished, with memory leaks and crashes after about three uses. Wiring
  Reset through that would make repeated presses worse than doing nothing.
