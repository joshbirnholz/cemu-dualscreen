#include "input/api/DualScreen/DualScreenController.h"

#include "DualscreenSession.h"
#include "Cemu/Logging/CemuLogging.h"

#include <algorithm>
#include <iterator>

namespace
{
	// Wii U GamePad button order, which is ds::ExternalButton's order, which is
	// the order these appear as kButton0 upward. Written out so the names in
	// the input settings and the default mapping cannot drift apart from what
	// the link actually sends.
	constexpr const char* kButtonNames[] = {
		"A", "B", "X", "Y",
		"L", "R", "ZL", "ZR",
		"+", "-",
		"Up", "Down", "Left", "Right",
		"Stick L", "Stick R",
	};
	constexpr size_t kButtonCount = std::size(kButtonNames);

	// The link carries sticks in libretro's range, y positive downwards;
	// Cemu wants [-1, 1] with y positive up, which is what the default
	// mapping's kAxisYP for "up" then assumes.
	//
	// Raw and undeadzoned on purpose. The shell leaves deadzoning to whoever
	// consumes the stick, and now that this arrives as an ordinary controller
	// the answer is ControllerBase::apply_axis_setting -- the same scaled
	// radial deadzone every other pad here gets, at whatever value the player
	// set in the input settings, rather than one hardcoded out of their reach.
	constexpr float kStickRange = 32767.0f;

	// A GamePad's acceleration is in g on Cemu's side of the fusion, and the
	// link carries metres per second squared.
	constexpr float kGravity = 9.81f;

	float stick(sint16 value)
	{
		return std::clamp(static_cast<float>(value) / kStickRange, -1.0f, 1.0f);
	}
} // namespace

DualScreenController::DualScreenController()
	// "GamePad" rather than "Companion", which was only ever true of half the
	// ways this gets used: the shell hands over one pad, and where that pad
	// comes from is its business, not this end's. On two devices it is the
	// companion's; on one it is whatever is plugged into or built into the
	// machine running the shell. What it always is, is the GamePad.
	//
	// A display name only, and safe to change: profiles are keyed by the API
	// and the uuid below, and ControllerFactory ignores the name a saved
	// profile carries for this API entirely.
	: base_type("0", "DualScreen GamePad")
{
	// Motion and rumble on by default, the way NativeWiimoteController does it
	// and for the same reason: these are not incidental capabilities of the
	// device, they are most of what it is for.
	//
	// Setting them in set_default_mapping instead, which is where this started,
	// did not survive a restart. InputManager::save only writes the motion
	// setting when has_motion() says so, and has_motion() was false whenever no
	// companion happened to be streaming -- which is exactly the situation
	// someone is in while adding the controller in the input settings. So the
	// value was never written, and every launch loaded the built-in default of
	// off. A constructor default is loaded first and then overridden only by a
	// setting the profile actually carries, which is the behaviour wanted:
	// on unless the player turned it off.
	m_settings.motion = true;
	m_settings.rumble = 1.0f;
}

bool DualScreenController::is_connected()
{
	// Connected means a shell is attached and has sent at least one pad, not
	// merely that this build can speak the protocol -- otherwise the input
	// settings would show it as live on a machine with no shell running.
	ds::ExternalInput pad;
	return DualscreenSession::instance().ReadInput(pad);
}

bool DualScreenController::has_motion()
{
	// Whether this device can report motion, not whether a reading happens to
	// be in flight this instant. Cemu asks this both to decide what to poll and
	// to decide whether the motion setting is worth saving, and answering "not
	// right now" to the second question is what silently dropped the setting
	// from the profile.
	//
	// Still false with no shell attached at all, so a session that is not
	// running leaves any other pad's gyro to be used instead.
	ds::ExternalInput pad;
	return DualscreenSession::instance().ReadInput(pad);
}

MotionSample DualScreenController::get_motion_sample()
{
	ds::ExternalInput pad;
	if (!DualscreenSession::instance().ReadInput(pad) || !pad.motion.present)
	{
		// Whatever it last said stops being true once it stops saying it, so
		// the next sample after a gap starts a fresh interval rather than
		// integrating across the whole silence.
		m_have_motion = false;
		return m_motion.getMotionSample();
	}

	// The interval comes from the sending device's own clock rather than from
	// this one: the samples were taken there, and the time between them is
	// not the time between two reads here. Unsigned subtraction gives the
	// right answer across the wrap that clock has every seventy-one minutes.
	float seconds = 1.0f / 60.0f;
	if (m_have_motion)
	{
		const uint32 elapsed = pad.motion.sampled_us - m_last_motion_us;
		// The same sample read twice -- nothing here controls how often this
		// is asked for, and the shell sends one per frame while a caller may
		// ask more than once within it. Integrating it again would turn one
		// movement into two.
		if (elapsed == 0)
			return m_motion.getMotionSample();
		// A gap rather than a measurement: the link stopped, or the game was
		// paused. An unbounded step there would spin the fused orientation
		// somewhere arbitrary in a single frame.
		if (elapsed <= 1000000)
			seconds = static_cast<float>(elapsed) / 1000000.0f;
	}
	m_last_motion_us = pad.motion.sampled_us;
	m_have_motion = true;

	// The link carries SDL's convention; this is the transform Cemu's own SDL
	// provider applies, so a device read through either path lands in the
	// same place.
	m_motion.processMotionSample(seconds,
	                             pad.motion.gyro_x,
	                             -pad.motion.gyro_y,
	                             -pad.motion.gyro_z,
	                             -pad.motion.accel_x / kGravity,
	                             pad.motion.accel_y / kGravity,
	                             pad.motion.accel_z / kGravity);
	return m_motion.getMotionSample();
}

void DualScreenController::start_rumble()
{
	// Honouring the strength the way every other controller here does, so
	// dragging the slider to zero turns a companion's rumble off.
	if (m_settings.rumble <= 0)
		return;
	DualscreenSession::instance().SendRumble(true);
}

void DualScreenController::stop_rumble()
{
	// Not gated, deliberately -- the same as SDLController. Turning the
	// strength down mid-rumble must still stop the one already running rather
	// than leave it on forever.
	DualscreenSession::instance().SendRumble(false);
}

std::string DualScreenController::get_button_name(uint64 button) const
{
	if (button < kButtonCount)
		return kButtonNames[button];
	return base_type::get_button_name(button);
}

ControllerState DualScreenController::raw_state()
{
	ControllerState result{};

	ds::ExternalInput pad;
	if (!DualscreenSession::instance().ReadInput(pad))
		return result;

	// Said once, the first time this device is actually polled -- which only
	// happens when it is mapped to an emulated controller. Whether motion is
	// switched on for it is otherwise invisible from the outside and is the
	// one thing that decides whether a gyro works at all, so it should not
	// take a round of guessing to find out.
	static bool reported = false;
	if (!reported)
	{
		reported = true;
		cemuLog_log(LogType::Force,
		            "Dualscreen: companion mapped to the GamePad; motion is {}, rumble is {}",
		            m_settings.motion ? "on" : "off",
		            m_settings.rumble > 0 ? "on" : "off");
	}

	std::vector<uint32> pressed;
	pressed.reserve(kButtonCount);
	for (uint32 index = 0; index < kButtonCount; ++index)
	{
		if ((pad.buttons & (1u << index)) != 0)
			pressed.emplace_back(static_cast<uint32>(kButton0) + index);
	}
	if (!pressed.empty())
		result.buttons.SetPressedButtons(pressed);

	result.axis.x = stick(pad.left_x);
	result.axis.y = -stick(pad.left_y);
	result.rotation.x = stick(pad.right_x);
	result.rotation.y = -stick(pad.right_y);
	// No analog triggers: a Wii U GamePad's ZL and ZR are switches, and they
	// arrive above as ordinary buttons.

	return result;
}
