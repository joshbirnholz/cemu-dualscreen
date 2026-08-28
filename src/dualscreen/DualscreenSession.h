#pragma once

#include <memory>
#include <mutex>
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
// Everything is behind one lock. The renderer publishes from the render thread
// and the pad is read from another, both about once a frame, so contention is
// not a concern and the alternative is a lock-free scheme nobody can check.
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
	// safe to call more than once a frame.
	void Poll();

	// One screen, already XRGB8888 and tightly packed. Both screens of one
	// frame must go under the same serial or the two devices can show halves
	// of different moments.
	void PublishScreen(ds::ScreenId screen, uint64_t serial, const uint32_t* pixels, int width,
					   int height);

	// Tagged by screen: a Wii U's TV and GamePad audio are independent, and
	// the shell routes each by whatever the player set that screen's own
	// audio target to.
	void SendAudio(ds::ScreenId screen, const int16_t* interleavedStereo, size_t frames);

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
	// Said once rather than every frame: a session that cannot connect is a
	// normal way to run, and a session that keeps failing is one line of
	// explanation, not a log.
	bool m_reportedFailure = false;
	bool m_reportedPublishFailure = false;
	bool m_reportedAudioFailure = false;
};
