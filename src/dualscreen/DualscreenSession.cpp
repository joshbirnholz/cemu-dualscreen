#include <algorithm>
#include "DualscreenSession.h"

#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include <chrono>

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
	// Dualscreen's menu can be drawn over this window -- see TakeOverlay() --
	// can pause the game while it is up -- see TakePauseRequest() -- and can
	// change the game's graphics packs and Cemu's options, and scan an
	// amiibo -- see SetRequestHandler().
	// And the pointer over that menu is streamed, so its lists drag and turn
	// under a finger and the wheel -- see SendOverlayPointer().
	// And the game's own keyboard and error messages are Dualscreen's to show
	// -- see ShowGameDialog().
	info.features = ds::kExternalFeatureOverlay | ds::kExternalFeaturePause |
					ds::kExternalFeatureOverlayPointer | ds::kExternalFeatureGameDialogs |
					ds::kExternalFeatureOverlayAnimation;
	{
		std::scoped_lock requests(m_requestsMutex);
		if (m_requestHandler)
			info.features |= ds::kExternalFeatureGraphicPacks | ds::kExternalFeatureOptions |
							 ds::kExternalFeatureAmiibo | ds::kExternalFeatureAmiiboReading;
	}

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
	m_requestedMaxWidthAtomic.store(0, std::memory_order_relaxed);
	m_requestedMaxHeightAtomic.store(0, std::memory_order_relaxed);
	// Nothing a previous session was still holding belongs to this one. Taken
	// in the same order as everywhere else -- m_mutex is already held here.
	{
		std::scoped_lock pending(m_pendingAudioMutex);
		m_pendingAudio[0].clear();
		m_pendingAudio[1].clear();
	}
	cemuLog_log(LogType::Force, "Dualscreen: handing screens over at {}", s_socketPath);
}

void DualscreenSession::Stop()
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.close();
	m_overlayVisibleAtomic.store(false, std::memory_order_relaxed);
	m_overlayTakesInputAtomic.store(false, std::memory_order_relaxed);
	m_pointerShownAtomic.store(true, std::memory_order_relaxed);
	m_running = false;
	m_requestedScreen.reset();
	m_runningAtomic.store(false, std::memory_order_relaxed);
	m_requestedScreenAtomic.store(-1, std::memory_order_relaxed);
	m_requestedMaxWidthAtomic.store(0, std::memory_order_relaxed);
	m_requestedMaxHeightAtomic.store(0, std::memory_order_relaxed);
	// Nothing a previous session was still holding belongs to this one. Taken
	// in the same order as everywhere else -- m_mutex is already held here.
	{
		std::scoped_lock pending(m_pendingAudioMutex);
		m_pendingAudio[0].clear();
		m_pendingAudio[1].clear();
	}
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
		m_requestedScreen.reset();
		m_runningAtomic.store(false, std::memory_order_relaxed);
		m_requestedScreenAtomic.store(-1, std::memory_order_relaxed);
	m_requestedMaxWidthAtomic.store(0, std::memory_order_relaxed);
	m_requestedMaxHeightAtomic.store(0, std::memory_order_relaxed);
		m_overlayVisibleAtomic.store(false, std::memory_order_relaxed);
		m_overlayTakesInputAtomic.store(false, std::memory_order_relaxed);
		m_pointerShownAtomic.store(true, std::memory_order_relaxed);
		// Cemu draws the game's dialogs again.
		m_gameDialogsWantedAtomic.store(false, std::memory_order_relaxed);
		// A game Dualscreen paused, or is pausing, would otherwise wait for it
		// for ever. coreinit ignores this when nothing is paused.
		m_pauseRequests.assign(1, false);
		// The game's controllers as Cemu had them before the shell unplugged
		// any, now nothing is left to feed them.
		{
			std::scoped_lock requests(m_requestsMutex);
			if (m_playersAttached)
			{
				m_playersAttached = false;
				m_playersFed.fill(false);
				m_playersFedChanged = true;
				if (m_requestHandler)
					m_requestHandler();
			}
		}
		return;
	}
	for (bool pause = false; m_producer.take_pause_request(pause);)
		m_pauseRequests.push_back(pause);
	{
		bool asked = false;
		std::scoped_lock requests(m_requestsMutex);
		if (m_producer.take_graphic_packs_wanted())
		{
			m_graphicPacksWanted = true;
			asked = true;
		}
		for (ds::ExternalGraphicPackChange change; m_producer.take_graphic_pack_change(change);)
		{
			m_graphicPackChanges.push_back(std::move(change));
			asked = true;
		}
		if (m_producer.take_options_wanted())
		{
			m_optionsWanted = true;
			asked = true;
		}
		std::string id;
		for (int choice = 0; m_producer.take_option_change(id, choice);)
		{
			m_optionChanges.emplace_back(std::move(id), choice);
			asked = true;
		}
		for (std::string path; m_producer.take_amiibo_scan(path);)
		{
			m_amiiboScans.push_back(std::move(path));
			asked = true;
		}
		// Which players anything is feeding: a change is the UI thread's to
		// act on, unplugging or plugging the game's controllers to match.
		for (int player = 0; player < ds::kExternalPlayers; ++player)
		{
			const bool fed = m_producer.player_connected(player);
			if (fed != m_playersFed[player] || !m_playersAttached)
			{
				m_playersFed[player] = fed;
				m_playersFedChanged = true;
				asked = true;
			}
		}
		m_playersAttached = true;
		// Not the UI thread's: the render thread takes these as it draws.
		for (ds::ExternalGameDialogAnswer answer; m_producer.take_game_dialog_answer(answer);)
		{
			m_gameDialogAnswers.push_back(std::move(answer));
			if (m_gameDialogAnswers.size() > 8)
				m_gameDialogAnswers.erase(m_gameDialogAnswers.begin());
		}
		if (asked && m_requestHandler)
			m_requestHandler();
	}
	m_pointerShownAtomic.store(m_producer.pointer_shown(), std::memory_order_relaxed);
	m_gameDialogsWantedAtomic.store(m_producer.game_dialogs_wanted(), std::memory_order_relaxed);
	// Dualscreen's menu, when it is up: copied out of shared memory here,
	// under m_mutex like everything else that reaches m_producer, and kept
	// for the renderer to pick up.
	{
		std::vector<uint8_t> pixels;
		int width = 0;
		int height = 0;
		if (m_producer.take_overlay(pixels, width, height))
		{
			std::scoped_lock overlay(m_overlayMutex);
			m_overlayPixels.swap(pixels);
			m_overlayWidth = width;
			m_overlayHeight = height;
			m_overlayFresh = true;
		}
		ds::ExternalOverlayAnimationFrames animation;
		if (m_producer.take_overlay_animation(animation))
		{
			std::scoped_lock overlay(m_overlayMutex);
			m_overlayAnimation = std::move(animation);
			m_overlayAnimationFresh = true;
			// The picture before it is the old menu's, or nothing.
			m_overlayFresh = false;
		}
		m_overlayVisibleAtomic.store(m_producer.overlay_visible(), std::memory_order_relaxed);
		m_overlayTakesInputAtomic.store(m_producer.overlay_takes_input(), std::memory_order_relaxed);
	}
	// Whichever screen the shell asked for -- absent if it has asked for
	// nothing, which is also true of a shell that has not connected past the
	// handshake yet. This is the one thing HandleDualscreenFrame and the
	// audio path gate their own work on (see ShouldCapture).
	m_requestedScreen = m_producer.requested_screen();
	m_requestedScreenAtomic.store(
		m_requestedScreen ? (*m_requestedScreen == ds::ScreenId::Top ? 0 : 1) : -1,
		std::memory_order_relaxed);
	// Published after the screen itself, and read the other way round in
	// CaptureSize(): a frame that briefly sees the old cap alongside the new
	// screen is one frame at the wrong size, which the shell caps again on
	// its own side anyway.
	m_requestedMaxWidthAtomic.store(m_producer.requested_max_width(), std::memory_order_relaxed);
	m_requestedMaxHeightAtomic.store(m_producer.requested_max_height(), std::memory_order_relaxed);
	const std::optional<ds::ScreenId> shownScreen = m_producer.shown_screen();
	m_shownScreenAtomic.store(
		shownScreen ? (*shownScreen == ds::ScreenId::Top ? 0 : 1) : -1,
		std::memory_order_relaxed);
	// Which screen this window should show, which the shell now says outright
	// rather than leaving to be inferred.
	//
	// It used to be inferred, as "whichever screen the shell did not ask to
	// capture", and that is right whenever it has asked for one. It has no
	// answer at all when it has asked for nothing, which is not an idle case:
	// it is a shell playing on a single machine, with no second device
	// anywhere. A handheld in that state is being held as a GamePad and wants
	// this window showing the GamePad; a desktop is a television and wants the
	// television. Only the shell knows which it is being, so it says.
	//
	// An older shell says nothing here and the inference stands, unchanged.
	//
	// Either way this runs every frame, so while a session is up the shell's
	// answer is the one that holds: the player's own Ctrl+Tab toggle sets the
	// same flag and is overwritten on the next poll. Holding Tab to peek at
	// the other screen still works, because that is a separate flag XORed with
	// this one. Both were already true before this said anything explicitly.
	if (const std::optional<ds::ScreenId> shown = m_producer.shown_screen())
	{
		LatteGPUState.isDRCPrimary = (*shown == ds::ScreenId::Bottom);
	}
	else if (m_requestedScreen == ds::ScreenId::Top)
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

bool DualscreenSession::ShouldPlayLocally(ds::ScreenId screen)
{
	// Whether a screen's sound belongs in this machine's own speakers.
	//
	// Three cases. A screen a shell asked for goes over the link and is never
	// played here -- that is ShouldCapture()'s half, and this is only ever
	// consulted for screens it said no to. A screen this machine is showing
	// plays here, which is where the sound of a picture belongs. And a screen
	// that is neither sent nor shown is a screen nobody is watching, so its
	// sound is nobody's either.
	//
	// That third case is new because it only exists on one machine. A Wii U
	// has two screens and a single device shows one of them, so the other was
	// playing to an empty room -- both soundtracks at once, out of the same
	// speakers. Two devices never reach it: between them they hold both
	// screens, so every screen is either sent or shown.
	//
	// Lock-free for the same reason ShouldCapture() is, and read on the same
	// real-time audio thread -- see its comment.
	if (!m_runningAtomic.load(std::memory_order_relaxed))
		return true;  // no session: this machine is the only place sound can go
	const int shown = m_shownScreenAtomic.load(std::memory_order_relaxed);
	if (shown < 0)
		return true;  // a shell with no opinion; leave its sound where it was
	return shown == (screen == ds::ScreenId::Top ? 0 : 1);
}

void DualscreenSession::CaptureSize(int sourceWidth, int sourceHeight, int& outWidth,
									int& outHeight)
{
	// The arithmetic lives in the link's own header so that both ends land on
	// the same number to the pixel -- see ds::fit_within(). Reducing here to
	// something a pixel off what the shell asked for would just make it scale
	// again, which is the whole cost this exists to remove.
	ds::fit_within(sourceWidth, sourceHeight,
				   m_requestedMaxWidthAtomic.load(std::memory_order_relaxed),
				   m_requestedMaxHeightAtomic.load(std::memory_order_relaxed), outWidth,
				   outHeight);
}

void DualscreenSession::PublishScreen(ds::ScreenId screen, uint64_t serial, const uint32_t* pixels,
									  int width, int height, uint64_t capturedUs)
{
	// Shared, not exclusive -- see the class comment. This can run alongside
	// Exclusive, like everything else that reaches m_producer: publish() ends
	// with a write to the same control socket SendAudio()/SendRumble() use,
	// and those writes cannot safely interleave -- see the class comment.
	std::scoped_lock lock(m_mutex);
	if (!m_running || pixels == nullptr || width <= 0 || height <= 0)
		return;

	std::string error;
	if (!m_producer.publish(screen, serial, pixels, width, height, width, &error, capturedUs))
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
	if (interleavedStereo == nullptr || frames == 0)
		return;

	// try_to_lock rather than a blocking wait, always: this runs on Cemu's
	// own real-time audio thread, and waiting on
	// m_mutex would be heard as a cutout in whichever screen's audio this
	// machine is still playing locally, or a gap in the stream this call is
	// what feeds the shell.
	//
	// So the samples go into m_pendingAudio first, under nothing but their own
	// small mutex, and only then is the link's lock tried. Missing it costs
	// this block a few milliseconds of extra delay and nothing else; the next
	// block that does get the lock carries both. That matters because the
	// alternative -- returning here and dropping the block -- takes about 12ms
	// of sound out of the middle of a waveform, and PublishScreen() holds
	// m_mutex often enough to make that happen roughly once a second, which is
	// audible as exactly the popping this whole path has been chased for.
	const size_t index = (screen == ds::ScreenId::Top) ? 0 : 1;
	{
		std::scoped_lock pending(m_pendingAudioMutex);
		std::vector<int16_t>& held = m_pendingAudio[index];
		held.insert(held.end(), interleavedStereo, interleavedStereo + frames * 2);
		// A link that has genuinely stopped taking audio must not make this
		// grow without bound. Well past anything ordinary contention causes:
		// by a quarter second the sound is too old to be worth playing anyway.
		constexpr size_t kMaxPendingSamples = 48000 * 2 / 4;
		if (held.size() > kMaxPendingSamples)
			held.erase(held.begin(), held.end() - static_cast<std::ptrdiff_t>(kMaxPendingSamples));
	}

	// m_mutex first and m_pendingAudioMutex second, always, and never the
	// other way round: send_audio() below writes to the control socket, which
	// can wait for the socket to drain, and holding the pending mutex across
	// that would make the very stall this exists to avoid -- with the audio
	// thread waiting on it next time round.
	std::unique_lock lock(m_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !m_running)
		return;  // kept, not lost: the next block that gets the lock takes it too

	std::vector<int16_t> outgoing;
	{
		std::scoped_lock pending(m_pendingAudioMutex);
		outgoing.swap(m_pendingAudio[index]);
	}
	if (outgoing.empty())
		return;

	std::string error;
	if (!m_producer.send_audio(screen, outgoing.data(), outgoing.size() / 2, &error))
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
	// Starting is the call that comes often -- once per poll while a rumble
	// pattern plays, up to sixty times a second -- and giving up on the rare
	// contended one is cheaper than blocking whichever thread is asking.
	// Nothing is lost: an edge that does not reach the wire is not recorded as
	// sent, so the next call with the same value sends it.
	//
	// Stopping is not like that. It happens once, on the falling edge --
	// EmulatedController::stop_rumble() returns early once it has stopped --
	// so there is no next call to make good a dropped one, and dropping it
	// leaves the companion buzzing until something else happens to rumble.
	// It is rare enough that waiting for the lock costs nothing.
	std::unique_lock lock(m_mutex, std::defer_lock);
	if (active)
	{
		lock.try_lock();
		if (!lock.owns_lock())
			return;
	}
	else
	{
		lock.lock();
	}
	if (!m_running)
		return;

	m_producer.send_rumble(active);
}

bool DualscreenSession::ReadPlayerInput(int player, ds::ExternalInput& out)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running || !m_producer.player_connected(player))
		return false;
	out = m_producer.player_input(player);
	return true;
}

void DualscreenSession::SendPlayerRumble(int player, bool active)
{
	// The same locking as SendRumble(), for the same reasons: a start that
	// cannot get the lock is tried again on the next call anyway, and a stop
	// is sent once and must not be lost.
	std::unique_lock lock(m_mutex, std::defer_lock);
	if (active)
	{
		lock.try_lock();
		if (!lock.owns_lock())
			return;
	}
	else
	{
		lock.lock();
	}
	if (!m_running)
		return;

	m_producer.send_player_rumble(player, active);
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

bool DualscreenSession::TakeOverlayAnimation(ds::ExternalOverlayAnimationFrames& out)
{
	std::scoped_lock overlay(m_overlayMutex);
	if (!m_overlayAnimationFresh)
		return false;
	out = std::move(m_overlayAnimation);
	m_overlayAnimation = ds::ExternalOverlayAnimationFrames();
	m_overlayAnimationFresh = false;
	return true;
}

bool DualscreenSession::TakeOverlay(std::vector<uint8_t>& rgba, int& width, int& height)
{
	std::scoped_lock overlay(m_overlayMutex);
	if (!m_overlayFresh)
		return false;
	rgba.swap(m_overlayPixels);
	width = m_overlayWidth;
	height = m_overlayHeight;
	m_overlayFresh = false;
	return true;
}

bool DualscreenSession::TakePauseRequest(bool& outPause)
{
	std::scoped_lock lock(m_mutex);
	if (m_pauseRequests.empty())
		return false;
	outPause = m_pauseRequests.front();
	m_pauseRequests.erase(m_pauseRequests.begin());
	return true;
}

void DualscreenSession::ReportPauseState(ds::ExternalPauseState state)
{
	m_gamePausedAtomic.store(state == ds::ExternalPauseState::Paused, std::memory_order_relaxed);
	m_stillWantedAtomic.store(state == ds::ExternalPauseState::Paused, std::memory_order_relaxed);
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_pause_state(state);
}

void DualscreenSession::SendMenuKey()
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_menu_key();
	cemuLog_log(LogType::Force, "Dualscreen: F1 pressed; asking for the menu");
}

void DualscreenSession::SetOverlayRect(float left, float top, float right, float bottom)
{
	std::scoped_lock rect(m_overlayRectMutex);
	m_overlayRect[0] = left;
	m_overlayRect[1] = top;
	m_overlayRect[2] = right;
	m_overlayRect[3] = bottom;
}

bool DualscreenSession::MapToOverlay(float x, float y, float& outX, float& outY)
{
	if (!OverlayTakesInput())
		return false;
	std::scoped_lock rect(m_overlayRectMutex);
	const float width = m_overlayRect[2] - m_overlayRect[0];
	const float height = m_overlayRect[3] - m_overlayRect[1];
	if (width <= 0.0f || height <= 0.0f)
		return false;
	outX = std::clamp((x - m_overlayRect[0]) / width, 0.0f, 1.0f);
	outY = std::clamp((y - m_overlayRect[1]) / height, 0.0f, 1.0f);
	return true;
}

void DualscreenSession::SendMenuKeyboard(ds::ExternalMenuKey key, bool down)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_menu_keyboard(key, down);
}

void DualscreenSession::SendOverlayTap(float pressX, float pressY, float x, float y)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_overlay_tap(pressX, pressY, x, y);
}

void DualscreenSession::SendOverlayPointer(ds::ExternalOverlayPointerKind kind, float x, float y,
										   float wheel)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_overlay_pointer(kind, x, y, wheel);
}

void DualscreenSession::SetRequestHandler(std::function<void()> handler)
{
	std::scoped_lock requests(m_requestsMutex);
	m_requestHandler = std::move(handler);
}

bool DualscreenSession::TakeGraphicPacksWanted()
{
	std::scoped_lock requests(m_requestsMutex);
	const bool wanted = m_graphicPacksWanted;
	m_graphicPacksWanted = false;
	return wanted;
}

bool DualscreenSession::TakeGraphicPackChange(ds::ExternalGraphicPackChange& out)
{
	std::scoped_lock requests(m_requestsMutex);
	if (m_graphicPackChanges.empty())
		return false;
	out = std::move(m_graphicPackChanges.front());
	m_graphicPackChanges.erase(m_graphicPackChanges.begin());
	return true;
}

void DualscreenSession::SendGraphicPacks(const std::vector<ds::ExternalGraphicPack>& packs)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_graphic_packs(packs);
}

bool DualscreenSession::TakeOptionsWanted()
{
	std::scoped_lock requests(m_requestsMutex);
	const bool wanted = m_optionsWanted;
	m_optionsWanted = false;
	return wanted;
}

bool DualscreenSession::TakeOptionChange(std::string& id, int& choice)
{
	std::scoped_lock requests(m_requestsMutex);
	if (m_optionChanges.empty())
		return false;
	id = std::move(m_optionChanges.front().first);
	choice = m_optionChanges.front().second;
	m_optionChanges.erase(m_optionChanges.begin());
	return true;
}

void DualscreenSession::SendOptions(const std::vector<ds::ExternalOption>& options)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_options(options);
}

bool DualscreenSession::TakePlayersFed(std::array<bool, ds::kExternalPlayers>& fed, bool& attached)
{
	std::scoped_lock requests(m_requestsMutex);
	if (!m_playersFedChanged)
		return false;
	m_playersFedChanged = false;
	fed = m_playersFed;
	attached = m_playersAttached;
	return true;
}

bool DualscreenSession::TakeAmiiboScan(std::string& path)
{
	std::scoped_lock requests(m_requestsMutex);
	if (m_amiiboScans.empty())
		return false;
	path = std::move(m_amiiboScans.front());
	m_amiiboScans.erase(m_amiiboScans.begin());
	return true;
}

void DualscreenSession::SendAmiiboResult(ds::ExternalAmiiboResult result)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_amiibo_result(result);
}

void DualscreenSession::SetAmiiboReading(ds::ExternalAmiiboReading reading)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_amiibo_reading(reading);
}

void DualscreenSession::ShowGameDialog(const ds::ExternalGameDialog& dialog)
{
	std::scoped_lock lock(m_mutex);
	if (!m_running)
		return;
	m_producer.send_game_dialog(dialog);
}

bool DualscreenSession::TakeGameDialogAnswer(uint32_t serial, ds::ExternalGameDialogAnswer& out)
{
	std::scoped_lock requests(m_requestsMutex);
	for (auto it = m_gameDialogAnswers.begin(); it != m_gameDialogAnswers.end(); ++it)
	{
		if (it->serial != serial)
			continue;
		out = std::move(*it);
		m_gameDialogAnswers.erase(it);
		return true;
	}
	return false;
}
