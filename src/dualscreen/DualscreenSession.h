#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/external_frame_link.hpp"
#include "layout/screen_id.hpp"

// Cemu's side of the handover to Dualscreen. Not vendored -- see README.md;
// everything under core/ and layout/ is a copy from that project and this is
// what Cemu wraps around it.
//
// One of these per process, because a Wii U has one television and one GamePad
// however many things want to talk about them. It lives here rather than in
// CemuCafe because CemuInput needs it too and CemuCafe already depends on
// CemuInput -- putting it the other way round would be a cycle.
//
// Everything is behind one lock, because m_producer is one object with no
// locking of its own and four different threads reach it: Poll() and
// PublishScreen() from the GPU thread and the background readback thread
// (m_dualscreenThread in VulkanRendererDualscreen.cpp) roughly once a frame
// each, ReadInput() from wherever VPADRead ends up (game-controlled, and
// sometimes far faster than once a frame), and SendAudio() from Cemu's own
// audio thread every few milliseconds. That last one is the one of these
// that actually matters: a real-time audio callback stalling on a lock held
// by any of the others is a stutter or a cutout, not just a late frame -- see
// SendAudio()'s own comment for how it avoids that without a second lock.
class DualscreenSession
{
public:
	static DualscreenSession& instance();

	// Where a shell is listening. Empty means this run is not handing its
	// screens anywhere, which is the normal case and costs nothing.
	static void SetSocketPath(std::string path);
	static bool IsEnabled();

	// A title started or stopped. Connecting is allowed to fail -- a Cemu
	// launched with a socket path that nothing is listening on plays the game
	// perfectly well on its own.
	void Start(int tvWidth, int tvHeight, int padWidth, int padHeight, double framesPerSecond);
	void Stop();
	bool IsRunning();

	// Reads whatever the shell has sent and notices it going away. Cheap, and
	// safe to call more than once a frame. Also where the shell's most recent
	// screen request (see ShouldCapture) is picked up and, when it asks for
	// the television, where this end's own window is switched to the GamePad
	// -- the shell showing the television is the only reason this end would
	// ever need to show anything else.
	void Poll();

	// Whether this screen is the one the shell has actually asked to be sent
	// right now -- the one thing HandleDualscreenFrame and the audio path
	// need to know before doing any work at all. False for both screens is
	// the ordinary state of a run nothing has connected to yet, and for
	// whichever screen a connected shell is not currently watching: nothing
	// is captured, converted, or sent for it, the same as if no session
	// existed. See core/external_frame_link.hpp's RequestScreen.
	bool ShouldCapture(ds::ScreenId screen);

	// One screen, already XRGB8888 and tightly packed. Both screens of one
	// frame must go under the same serial or the two devices can show halves
	// of different moments.
	void PublishScreen(ds::ScreenId screen, uint64_t serial, const uint32_t* pixels, int width,
					   int height);

	// Tagged by screen: a Wii U's TV and GamePad audio are independent, and
	// the shell routes each by whatever the player set that screen's own
	// audio target to. Never blocks: called from Cemu's own real-time audio
	// thread, where waiting on m_mutex would be heard as a stutter in
	// whichever screen's sound this machine is still playing locally, or a
	// gap in the buffer this same call is what feeds the shell. If the lock
	// is held by Poll() or PublishScreen() right now, this block is skipped
	// rather than waited for -- the same "drop it, the next one right behind
	// makes it unnoticeable" call this link already makes for Frame and
	// Audio messages on the wire, just made one level up, before there is
	// even a message to drop.
	void SendAudio(ds::ScreenId screen, const int16_t* interleavedStereo, size_t frames);

	// Tells the shell whether the GamePad's own motor is on right now, so it
	// can forward that to whatever the player is actually holding. Cheap to
	// call every time VPADController's own rumble state is read -- see
	// VPADController::update() -- since it is a no-op on the wire whenever
	// the value has not changed; see ExternalFrameProducer::send_rumble().
	// Sent whenever a shell is connected at all, not gated on ShouldCapture
	// the way video and audio are: the shell holds the GamePad's controls
	// regardless of which screen it is currently showing (ReadInput() above
	// is the same way), and a player's hands do not stop being the ones on
	// the GamePad just because their eyes are on the television right now.
	void SendRumble(bool active);

	// The pad the shell is sending, if it is sending one. False means Cemu's
	// own controllers are still in charge, which is how a run with no shell --
	// or one whose shell has not spoken yet -- behaves. Asking and reading are
	// one call so that a Poll() on another thread cannot land between them.
	bool ReadInput(ds::ExternalInput& out);

	// The size of the GamePad screen this session hands over, which is the
	// size the shell reports touches in.
	int PadWidth();
	int PadHeight();

private:
	std::mutex m_mutex;
	ds::ExternalFrameProducer m_producer;
	bool m_running = false;
	int m_padWidth = 0;
	int m_padHeight = 0;
	// The screen the shell most recently asked for, updated from Poll().
	// Absent means either nothing has connected yet or a connected shell
	// explicitly asked for nothing -- both look the same from here: capture
	// and send neither screen.
	std::optional<ds::ScreenId> m_requestedScreen;
	// Said once rather than every frame: a session that cannot connect is a
	// normal way to run, and a session that keeps failing is one line of
	// explanation, not a log.
	bool m_reportedFailure = false;
	bool m_reportedPublishFailure = false;
	bool m_reportedAudioFailure = false;
};
