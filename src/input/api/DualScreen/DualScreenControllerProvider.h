#pragma once

#include "input/api/ControllerProvider.h"

#ifndef HAS_DUALSCREEN_INPUT
#define HAS_DUALSCREEN_INPUT 1
#endif

// The device holding this Cafe's GamePad screen, offered to Cemu as an
// ordinary controller.
//
// It used to be an override buried inside VPADController::VPADRead: whenever a
// shell was attached it replaced the emulated GamePad's buttons, sticks and
// touch wholesale and never consulted the user's own mapping. That worked, and
// it answered every question about control schemes with "you cannot". Wii U
// games want wildly different ones -- and Cemu already has somewhere to put
// that, in per-game profiles against a named device.
//
// So this is that named device. Everything a mapping can express -- buttons,
// both sticks, motion -- comes through here and is mapped exactly like a pad
// plugged into this machine, with a sensible default applied on the way in so
// that nothing needs configuring to start with (see
// VPADController::set_default_mapping). Touch stays outside it, since an
// absolute point on a screen is not something a button mapping has any way to
// describe.
//
// One controller, always offered even with no shell attached, so a player can
// set the mapping up before starting a session rather than only during one.
class DualScreenControllerProvider : public ControllerProviderBase
{
	friend class DualScreenController;
public:
	inline static InputAPI::Type kAPIType = InputAPI::DualScreen;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;
};
