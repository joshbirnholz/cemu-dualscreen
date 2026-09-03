#pragma once

#include "input/api/Controller.h"
#include "input/api/DualScreen/DualScreenControllerProvider.h"
#include "input/motion/MotionHandler.h"

// The one controller the DualScreen provider offers -- see that header for
// why this is a controller at all rather than the override it used to be.
//
// Its raw buttons are the sixteen a Wii U GamePad has, in the order the link
// carries them (ds::ExternalButton), so kButton0 is A and kButton15 is the
// right stick click. They come out under those names in the input settings
// rather than as bare numbers, and VPADController::set_default_mapping wires
// them straight through, which is what keeps a fresh session working with
// nothing configured.
class DualScreenController : public Controller<DualScreenControllerProvider>
{
public:
	DualScreenController();

	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::DualScreen) == "DualScreen");
		return to_string(InputAPI::DualScreen);
	}
	InputAPI::Type api() const override { return InputAPI::DualScreen; }

	bool is_connected() override;

	// Only while the device standing in for the GamePad is actually reporting
	// it. A shell whose device has no sensors -- a plain monitor, a handheld
	// with the gyro switched off -- says so rather than reporting stillness,
	// and this device then simply has no motion for the mapping to use.
	bool has_motion() override;
	MotionSample get_motion_sample() override;

	// The link carries a rumble flag rather than a strength, so any setting
	// above zero means on. Which device actually buzzes is then the ordinary
	// question of which controller is assigned to the GamePad: assign the
	// companion and the companion rumbles, assign a pad plugged in here and
	// that one does.
	bool has_rumble() override { return true; }
	void start_rumble() override;
	void stop_rumble() override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

private:
	// Cemu's own fusion, fed the raw pair this link carries. Deliberately not
	// the shell's: a GamePad's orientation is what the game reads, and two
	// integrations disagreeing about which way is up would be worse than one.
	WiiUMotionHandler m_motion;
	uint32 m_last_motion_us = 0;
	bool m_have_motion = false;
};
