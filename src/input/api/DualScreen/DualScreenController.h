#pragma once

#include "input/api/Controller.h"
#include "input/api/DualScreen/DualScreenControllerProvider.h"
#include "input/motion/MotionHandler.h"
#include "core/external_frame_link.hpp"

// A controller the DualScreen provider offers: the GamePad, or one of the
// game's other players -- see that header for why these are controllers at all
// rather than the override the GamePad used to be.
//
// Its raw buttons are the sixteen a Wii U GamePad has, in the order the link
// carries them (ds::ExternalButton), so kButton0 is A and kButton15 is the
// right stick click -- then Home, and a Wii Remote's and a Nunchuk's own.
// They come out under those names in the input settings rather than as bare
// numbers, and VPADController::set_default_mapping wires
// them straight through, which is what keeps a fresh session working with
// nothing configured.
class DualScreenController : public Controller<DualScreenControllerProvider>
{
public:
	// `player` is -1 for the GamePad and 0 to ds::kExternalPlayers - 1 for
	// Player 1 upward. The uuid is "0" for the GamePad, which is what every
	// profile saved before there were players already says, and the player's
	// own number from 1 upward for the rest.
	explicit DualScreenController(int player = -1);

	// The uuid a controller with this `player` is saved under, and the player
	// a saved uuid means -- anything that is not a player's number is the
	// GamePad, which is what an old profile's "0" has always meant.
	static std::string uuid_for(int player);
	static int player_for(std::string_view uuid);

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

	// This controller's pad, from whichever half of the link carries it: a
	// player's buttons already as the game's controller has them -- see
	// ds::ExternalButton -- which a Wii Remote or Pro Controller fed by this
	// alone reads directly. False with nothing feeding it.
	bool read_pad(ds::ExternalInput& out) const;

protected:
	ControllerState raw_state() override;

private:
	// Cemu's own fusion, fed the raw pair this link carries. Deliberately not
	// the shell's: a GamePad's orientation is what the game reads, and two
	// integrations disagreeing about which way is up would be worse than one.
	WiiUMotionHandler m_motion;
	uint32 m_last_motion_us = 0;
	bool m_have_motion = false;

	// Which controller this is: -1 for the GamePad, else the player.
	int m_player = -1;
	bool m_reported = false;
};
