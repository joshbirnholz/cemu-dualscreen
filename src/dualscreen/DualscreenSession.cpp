#include "DualscreenSession.h"

#include "Common/precompiled.h"

namespace
{
	std::string s_socketPath;
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
	m_reportedPublishFailure = false;
	m_reportedAudioFailure = false;
	cemuLog_log(LogType::Force, "Dualscreen: handing screens over at {}", s_socketPath);
}

void DualscreenSession::Stop()
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.close();
	m_running = false;
	cemuLog_log(LogType::Force, "Dualscreen: session ended");
}

bool DualscreenSession::IsRunning()
{
	std::scoped_lock lock(m_mutex);
	return m_running;
}

void DualscreenSession::Poll()
{
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
		return;
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

void DualscreenSession::PublishScreen(ds::ScreenId screen, uint64_t serial, const uint32_t* pixels,
									  int width, int height)
{
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
		if (!m_reportedPublishFailure)
		{
			m_reportedPublishFailure = true;
			cemuLog_log(LogType::Force, "Dualscreen: not handing over a screen: {}", error);
		}
	}
}

void DualscreenSession::SendAudio(const int16_t* interleavedStereo, size_t frames)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running || interleavedStereo == nullptr || frames == 0)
		return;

	std::string error;
	if (!m_producer.send_audio(interleavedStereo, frames, &error))
	{
		if (!m_reportedAudioFailure)
		{
			m_reportedAudioFailure = true;
			cemuLog_log(LogType::Force, "Dualscreen: not sending audio: {}", error);
		}
	}
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
