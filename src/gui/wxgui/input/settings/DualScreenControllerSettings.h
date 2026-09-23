#pragma once

#include <wx/dialog.h>
#include <wx/timer.h>

#include "input/api/Controller.h"

class wxCheckBox;
class wxInputDraw;
class wxSlider;

// Settings for a DualScreen controller -- the GamePad or one of the players a
// Dualscreen shell hands over the link.
//
// Only what actually does something for one. DefaultControllerSettings has
// more, and most of it is not true of this controller: the link carries
// rumble as on or off, so a strength slider would be a switch pretending to
// be a dial; ZL and ZR are buttons on a GamePad, so there is no trigger axis
// to deadzone; and the device on the far end centres its own sticks, so
// there is nothing here to calibrate.
class DualScreenControllerSettings : public wxDialog
{
public:
	DualScreenControllerSettings(wxWindow* parent, const wxPoint& position, ControllerPtr controller);
	~DualScreenControllerSettings();

private:
	void update_settings();

	void on_timer(wxTimerEvent& event);
	void on_close(wxCloseEvent& event);
	void on_slider_change(wxCommandEvent& event);
	void on_rumble_change(wxCommandEvent& event);

	ControllerPtr m_controller;
	ControllerBase::Settings m_settings;

	wxTimer* m_timer = nullptr;
	std::optional<std::chrono::steady_clock::time_point> m_rumble_time{};

	wxCheckBox* m_use_motion = nullptr;
	wxCheckBox* m_rumble = nullptr;
	wxSlider* m_left_deadzone = nullptr;
	wxSlider* m_left_range = nullptr;
	wxSlider* m_right_deadzone = nullptr;
	wxSlider* m_right_range = nullptr;
	wxInputDraw* m_left_draw = nullptr;
	wxInputDraw* m_right_draw = nullptr;
};
