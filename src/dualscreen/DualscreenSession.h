#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
// locking of its own and four different threads reach it: Poll() from the GPU
// thread, PublishScreen() from the background readback thread
// (m_dualscreenThread in VulkanRendererDualscreen.cpp), ReadInput() from
// wherever VPADRead ends up (game-controlled, and sometimes far faster than
// once a frame), and SendAudio() from Cemu's own audio thread every few
// milliseconds.
//
// A plain mutex, deliberately, and not a shared_mutex. It is tempting to say
// that PublishScreen(), SendAudio(), SendRumble() and ReadInput() touch
// disjoint state -- screen slots against audio against rumble -- and so could
// each hold a shared (reader) lock and run concurrently, excluding only Poll()
// tearing the connection down. That was tried, and it is wrong: all of them
// end up writing to the same control socket, and those writes are not atomic.
// write_best_effort() can commit part of a message and then wait for the
// socket to drain, so a second thread writing in that window puts its bytes in
// the middle of the first message and desynchronises the length-prefixed
// framing from then on. It surfaced as the GamePad's rumble silently never
// reaching the shell again.
//
// So everything stays behind this one lock, and the calls that must not wait
// for it -- SendAudio() and SendRumble(), both on threads that cannot stall --
// take it with try_to_lock. Missing it is then handled by keeping what they
// were carrying rather than discarding it: see m_pendingAudio for sound, and
// ExternalFrameProducer::send_rumble() for the motor's own state. Neither
// waits, and neither loses anything by not waiting.
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
	// Whether a screen this machine is not sending anywhere should be heard
	// out of its own speakers. Only meaningful for a screen ShouldCapture()
	// said no to; see the definition.
	bool ShouldPlayLocally(ds::ScreenId screen);

	// The size a `sourceWidth` x `sourceHeight` screen is actually worth
	// handing over at: the shell's own display size, fitted to preserve the
	// picture's shape, or the source size unchanged when the shell has not
	// said (or is already smaller than what it asked for).
	//
	// This is what makes a graphics pack's resolution free rather than
	// ruinous. A 4K preset against a 1280x800 handheld is sixteen times the
	// pixels; sent whole, every one of them is copied out of video memory,
	// through shared memory, and then scaled back down on the shell's CPU --
	// measured at 14.6ms per frame of software scaling alone, on top of a
	// 33MB copy here. Reducing it on the GPU, where the frame already is and
	// where a blit does it for nothing, removes both.
	//
	// Lock-free for the same reason ShouldCapture() is, and called from the
	// same place: this runs on the GPU thread, per frame, alongside a Poll()
	// that holds m_mutex.
	void CaptureSize(int sourceWidth, int sourceHeight, int& outWidth, int& outHeight);

	// One screen, already XRGB8888 and tightly packed. Both screens of one
	// frame must go under the same serial or the two devices can show halves
	// of different moments. `capturedUs` is when the copy was recorded, by
	// ds::external_frame_clock_us(), so the shell can say how old frames are
	// by the time it has them; 0 means now.
	void PublishScreen(ds::ScreenId screen, uint64_t serial, const uint32_t* pixels, int width,
					   int height, uint64_t capturedUs = 0);

	// Tagged by screen: a Wii U's TV and GamePad audio are independent, and
	// the shell routes each by whatever the player set that screen's own
	// audio target to. Never blocks: called from Cemu's own real-time audio
	// thread, where waiting on m_mutex would be heard as a stutter in
	// whichever screen's sound this machine is still playing locally, or a
	// gap in the buffer this same call is what feeds the shell. So it takes
	// m_mutex with try_to_lock and never waits -- but a block that misses the
	// lock is kept and sent with the next one rather than thrown away, since
	// throwing it away is itself a pop. See m_pendingAudio.
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

	// The same for one of the game's other players, 0 for Player 1 -- see
	// ds::kExternalPlayers. False when no shell is attached, or when the shell
	// says nothing is feeding that player: a controller that is not plugged in.
	bool ReadPlayerInput(int player, ds::ExternalInput& out);

	// That player's motor, on the same terms as SendRumble() above -- the
	// shell decides which pad, on which device, actually buzzes.
	void SendPlayerRumble(int player, bool active);

	// The size of the GamePad screen this session hands over, which is the
	// size the shell reports touches in.
	int PadWidth();
	int PadHeight();

private:
	// Every method that reaches m_producer takes this, exclusively -- see the
	// class comment for why a shared_mutex is not safe here.
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
	// Lock-free mirrors of m_running/m_requestedScreen, written alongside them
	// (under m_mutex, so still only ever from one thread at a time) and read
	// by ShouldCapture() without the lock at all -- see that function's own
	// comment for why. -1/0/1 rather than an atomic<optional<ScreenId>>,
	// which is not itself lock-free on every platform this builds for.
	std::atomic<bool> m_runningAtomic{false};
	std::atomic<int> m_requestedScreenAtomic{-1};
	// And the size cap alongside it, written and read on exactly the same
	// terms. Zero means the shell expressed no preference, which is what a
	// shell that has not yet learned its own companion's display size sends
	// -- and what every shell built before this existed sends always.
	std::atomic<int> m_requestedMaxWidthAtomic{0};
	std::atomic<int> m_requestedMaxHeightAtomic{0};
	// And which screen the shell says this machine's own window is showing,
	// on the same terms again. -1 means the shell has no opinion, which is
	// what one built before this existed always sends -- and which
	// ShouldPlayLocally() reads as "carry on as before".
	std::atomic<int> m_shownScreenAtomic{-1};
	// Sound that arrived while m_mutex was busy, per screen, waiting to go out
	// with the next block that gets through.
	//
	// SendAudio() cannot wait for the lock -- it runs on the real-time audio
	// thread -- but the block it is holding must not simply be dropped for
	// that either: dropping it takes about 12ms of sound out of the middle of
	// a waveform, and a step discontinuity like that is exactly what a pop is.
	// PublishScreen() holds m_mutex for around a millisecond per frame, which
	// against 87 audio blocks a second is a collision roughly once a second --
	// once a second is not "rare", it is audible. Kept here instead and sent
	// with the next block, which costs that sound a few milliseconds of extra
	// delay and loses none of it.
	//
	// Guarded by its own small mutex rather than by m_mutex, which would
	// defeat the point: it is only ever held for a copy between two buffers,
	// never across a socket write, so nothing that takes it can be made to
	// wait on the link.
	std::mutex m_pendingAudioMutex;
	std::vector<int16_t> m_pendingAudio[2];

	// Said once rather than every frame: a session that cannot connect is a
	// normal way to run, and a session that keeps failing is one line of
	// explanation, not a log.
	bool m_reportedFailure = false;
	// Indexed by screen (Top: 0, Bottom: 1), not one shared flag -- a publish
	// failure on one screen (the television, say, first to try a too-large
	// frame right after a graphics pack loads) must not silence the other
	// screen's own first failure later on (off-TV play switching the GamePad
	// to mirror that same too-large picture, minutes into a session): that
	// second failure is a different fact, on a different screen, and a shared
	// flag made it invisible -- exactly what turned a one-line "cannot be
	// carried: 3840x2160" into a mysteriously silent stuck picture the one
	// time this mattered in practice.
	bool m_reportedPublishFailure[2] = {false, false};
	bool m_reportedAudioFailure = false;
};
