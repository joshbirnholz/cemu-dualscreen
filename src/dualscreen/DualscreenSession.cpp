#include "DualscreenSession.h"

#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include <chrono>

namespace
{
	std::string s_socketPath;

	// Temporary instrumentation: everything here shares m_mutex with Poll()
	// and PublishScreen(), both called on the GPU thread from
	// HandleDualscreenFrame, and SendAudio(), called from Cemu's own audio
	// thread -- so a slow moment in any one of them, while holding that
	// lock, is a stall for whichever of the others wants it next.
	// VulkanRendererDualscreen.cpp already has its own probe showing
	// HandleDualscreenFrame itself costs far more than it should; this one
	// narrows down which locked call is actually spending the time, and
	// whether it is the lock wait or the work under it -- both are inside
	// the timed span, since either explains a caller stalling. Remove once
	// that's answered.
	struct ScopedCallTiming
	{
		const char* label;
		std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point& windowStart;
		double& totalMs;
		int& calls;
		ScopedCallTiming(const char* label_, std::chrono::steady_clock::time_point& windowStart_,
						 double& totalMs_, int& calls_)
			: label(label_), windowStart(windowStart_), totalMs(totalMs_), calls(calls_)
		{
		}
		~ScopedCallTiming()
		{
			const auto now = std::chrono::steady_clock::now();
			totalMs += std::chrono::duration<double, std::milli>(now - started).count();
			++calls;
			const double windowSeconds = std::chrono::duration<double>(now - windowStart).count();
			if (windowSeconds >= 1.0)
			{
				cemuLog_log(LogType::Force,
							"Dualscreen: {} cost {:.1f}ms over {} calls in the last {:.2f}s", label,
							totalMs, calls, windowSeconds);
				windowStart = now;
				totalMs = 0.0;
				calls = 0;
			}
		}
	};
}

DualscreenSession& DualscreenSession::instance()
{
	static DualscreenSession s_instance;
	return s_instance;
}

void DualscreenSession::SetSocketPath(std::string path)
{
	s_socketPath = std::move(path);
}

bool DualscreenSession::IsEnabled()
{
	return !s_socketPath.empty();
}

void DualscreenSession::Start(int tvWidth, int tvHeight, int padWidth, int padHeight,
							  double framesPerSecond)
{
	if (!IsEnabled())
		return;

	std::scoped_lock lock(m_mutex);
	if (m_running)
		return;

	ds::ExternalStreamInfo info;
	info.top_width = tvWidth;
	info.top_height = tvHeight;
	info.bottom_width = padWidth;
	info.bottom_height = padHeight;
	info.audio_sample_rate = 48000;
	info.frames_per_second = framesPerSecond;

	std::string error;
	if (!m_producer.connect(s_socketPath, info, &error))
	{
		if (!m_reportedFailure)
		{
			m_reportedFailure = true;
			cemuLog_log(LogType::Force, "Dualscreen: nothing at {}: {}", s_socketPath, error);
		}
		return;
	}
	m_running = true;
	m_padWidth = padWidth;
	m_padHeight = padHeight;
	m_reportedFailure = false;
	m_reportedPublishFailure[0] = false;
	m_reportedPublishFailure[1] = false;
	m_reportedAudioFailure = false;
	// A fresh connection has said nothing yet -- whatever a previous one last
	// asked for does not carry over to this one.
	m_requestedScreen.reset();
	m_runningAtomic.store(true, std::memory_order_relaxed);
	m_requestedScreenAtomic.store(-1, std::memory_order_relaxed);
	cemuLog_log(LogType::Force, "Dualscreen: handing screens over at {}", s_socketPath);
}

void DualscreenSession::Stop()
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.close();
	m_running = false;
	m_requestedScreen.reset();
	m_runningAtomic.store(false, std::memory_order_relaxed);
	m_requestedScreenAtomic.store(-1, std::memory_order_relaxed);
	cemuLog_log(LogType::Force, "Dualscreen: session ended");
}

bool DualscreenSession::IsRunning()
{
	std::scoped_lock lock(m_mutex);
	return m_running;
}

void DualscreenSession::Poll()
{
	static std::chrono::steady_clock::time_point s_windowStart = std::chrono::steady_clock::now();
	static double s_totalMs = 0.0;
	static int s_calls = 0;
	ScopedCallTiming timing("Poll", s_windowStart, s_totalMs, s_calls);

	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;

	bool resetRequested = false;
	std::string error;
	if (!m_producer.poll(resetRequested, &error))
	{
		// The shell quitting is how a session ends. Cemu keeps running: the
		// game is still playable in this window, which is more use to whoever
		// is sitting in front of it than exiting would be.
		cemuLog_log(LogType::Force, "Dualscreen: the shell went away: {}", error);
		m_producer.close();
		m_running = false;
		m_requestedScreen.reset();
		m_runningAtomic.store(false, std::memory_order_relaxed);
		m_requestedScreenAtomic.store(-1, std::memory_order_relaxed);
		return;
	}
	// Whichever screen the shell asked for -- absent if it has asked for
	// nothing, which is also true of a shell that has not connected past the
	// handshake yet. This is the one thing HandleDualscreenFrame and the
	// audio path gate their own work on (see ShouldCapture).
	m_requestedScreen = m_producer.requested_screen();
	m_requestedScreenAtomic.store(
		m_requestedScreen ? (*m_requestedScreen == ds::ScreenId::Top ? 0 : 1) : -1,
		std::memory_order_relaxed);
	// The only reason this end's own window should ever show anything but
	// its usual default: the shell is watching the television, so something
	// local has to show the other one instead. Nothing requested is left
	// alone entirely -- whatever the player's own Alt+Tab/VPAD toggle or
	// their own display setting already has it at, exactly as if no session
	// existed at all.
	if (m_requestedScreen == ds::ScreenId::Top)
	{
		LatteGPUState.isDRCPrimary = true;
	}
	else if (m_requestedScreen == ds::ScreenId::Bottom)
	{
		LatteGPUState.isDRCPrimary = false;
	}
	if (resetRequested)
	{
		// Deliberately not wired up. Poll() runs on the GPU thread, so acting
		// on this means handing off to the main thread the way
		// CafeSystem::RequestRecreateCanvas() does -- that part is buildable.
		// What it would call is not: the only stop-a-running-title path Cemu
		// has is MainWindow::EndEmulation(), and its own comment calls it
		// unfinished, with memory leaks and crashes after about three uses.
		// Wiring a shell's Reset button through that would make the third
		// press worse than the first two did nothing, which is a worse
		// failure than not having Reset at all. Revisit once Cemu's own
		// restart path is not the one thing standing in the way.
		cemuLog_log(LogType::Force, "Dualscreen: the shell asked for a reset, which is not "
									"implemented yet");
	}
}

bool DualscreenSession::ShouldCapture(ds::ScreenId screen)
{
	// Lock-free, not try_to_lock. This used to fall back to false -- "not
	// wanted" -- whenever m_mutex was merely contended, on the reasoning that
	// a false here just costs one skipped video frame, the same as an
	// already-tolerated drop elsewhere on this link. That reasoning does not
	// hold for audio: AIInitDMA/AIInitDRCDMA read a false as "this machine's
	// own screen, play it locally" rather than "send it to the shell",
	// swapping the request_screen() and the m_mutex contention it is racing
	// GPU-thread Poll()/PublishScreen() calls it happens every 3ms against
	// (see this header's own comment on those two) into audible bleed: the
	// requested screen's audio playing here too, in step with every stall.
	// These two atomics mirror m_running/m_requestedScreen -- both written
	// only under m_mutex, from Poll() -- so this answers without the lock at
	// all rather than merely trying for it.
	if (!m_runningAtomic.load(std::memory_order_relaxed))
		return false;
	const int requested = m_requestedScreenAtomic.load(std::memory_order_relaxed);
	return requested == (screen == ds::ScreenId::Top ? 0 : 1);
}

void DualscreenSession::PublishScreen(ds::ScreenId screen, uint64_t serial, const uint32_t* pixels,
									  int width, int height)
{
	static std::chrono::steady_clock::time_point s_windowStart = std::chrono::steady_clock::now();
	static double s_totalMs = 0.0;
	static int s_calls = 0;
	ScopedCallTiming timing("PublishScreen", s_windowStart, s_totalMs, s_calls);

	// Shared, not exclusive -- see the class comment. This can run alongside
	// Exclusive, like everything else that reaches m_producer: publish() ends
	// with a write to the same control socket SendAudio()/SendRumble() use,
	// and those writes cannot safely interleave -- see the class comment.
	std::scoped_lock lock(m_mutex);
	if (!m_running || pixels == nullptr || width <= 0 || height <= 0)
		return;

	std::string error;
	if (!m_producer.publish(screen, serial, pixels, width, height, width, &error))
	{
		// Not a reason to end the session. The likely cause is a screen larger
		// than the shared memory holds, which is a property of how this Cemu is
		// configured rather than of the link -- and if the socket has actually
		// died, Poll() is what notices that.
		const int index = (screen == ds::ScreenId::Top) ? 0 : 1;
		if (!m_reportedPublishFailure[index])
		{
			m_reportedPublishFailure[index] = true;
			cemuLog_log(LogType::Force, "Dualscreen: not handing over the {} screen: {}",
						(screen == ds::ScreenId::Top) ? "TV" : "GamePad", error);
		}
	}
}

void DualscreenSession::SendAudio(ds::ScreenId screen, const int16_t* interleavedStereo, size_t frames)
{
	static std::chrono::steady_clock::time_point s_windowStart = std::chrono::steady_clock::now();
	static double s_totalMs = 0.0;
	static int s_calls = 0;
	ScopedCallTiming timing("SendAudio", s_windowStart, s_totalMs, s_calls);

	if (interleavedStereo == nullptr || frames == 0)
		return;

	// Shared, not exclusive, and still try_to_lock rather than a blocking
	// wait: this runs on Cemu's own real-time audio thread. A shared lock is
	// what lets this run concurrently with PublishScreen() instead of behind
	// it (see the class comment) -- but Poll() still occasionally needs the
	// exclusive lock to tear the connection down, and waiting on that would
	// be heard as a cutout in whichever screen's audio this machine is still
	// playing locally, or a gap in the stream this call is what feeds the
	// shell. One skipped block is about 12ms of this screen's sound, the
	// same kind of drop Frame and Audio messages already tolerate on the
	// wire for a link that fell behind -- unnoticeable next to what waiting
	// for the lock would cost.
	std::unique_lock lock(m_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !m_running)
		return;

	std::string error;
	if (!m_producer.send_audio(screen, interleavedStereo, frames, &error))
	{
		if (!m_reportedAudioFailure)
		{
			m_reportedAudioFailure = true;
			cemuLog_log(LogType::Force, "Dualscreen: not sending audio: {}", error);
		}
	}
}

void DualscreenSession::SendRumble(bool active)
{
	// try_to_lock, same reasoning as SendAudio(): VPADController::update()
	// calls this from wherever VPADRead's own polling runs, up to sixty times
	// a second, and giving up on the rare contended call is cheaper than
	// blocking whichever thread is asking. Nothing is lost by it: an edge that
	// does not reach the wire is not recorded as sent, so the next call with
	// the same value sends it -- see ExternalFrameProducer::send_rumble().
	std::unique_lock lock(m_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !m_running)
		return;

	m_producer.send_rumble(active);
}

bool DualscreenSession::ReadInput(ds::ExternalInput& out)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running || !m_producer.have_input())
		return false;
	// By value rather than by reference: the next Poll() on another thread
	// would otherwise change it under whoever was reading.
	out = m_producer.input();
	return true;
}

int DualscreenSession::PadWidth()
{
	std::scoped_lock lock(m_mutex);
	return m_padWidth;
}

int DualscreenSession::PadHeight()
{
	std::scoped_lock lock(m_mutex);
	return m_padHeight;
}
