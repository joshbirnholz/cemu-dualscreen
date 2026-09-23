#include "wxgui/input/settings/DualScreenControllerSettings.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "wxgui/components/wxInputDraw.h"
#include "wxgui/helpers/wxControlObject.h"
#include "wxgui/helpers/wxHelpers.h"

DualScreenControllerSettings::DualScreenControllerSettings(wxWindow* parent, const wxPoint& position, ControllerPtr controller)
	: wxDialog(parent, wxID_ANY, _("DualScreen controller settings"), position, wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
	  m_controller(std::move(controller))
{
	m_settings = m_controller->get_settings();

	auto* sizer = new wxBoxSizer(wxVERTICAL);

	// Options
	{
		auto* box = new wxStaticBox(this, wxID_ANY, _("Options"));
		auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);

		// Only while Dualscreen is feeding this controller: the setting is
		// saved only when the controller says it has motion, so a change made
		// with nothing connected would quietly not be kept.
		m_use_motion = new wxCheckBox(box, wxID_ANY, _("Use motion"));
		m_use_motion->SetValue(m_settings.motion);
		m_use_motion->Enable(m_controller->has_motion());
		if (!m_controller->has_motion())
			m_use_motion->SetToolTip(_("Start Dualscreen to change this"));
		box_sizer->Add(m_use_motion, 0, wxEXPAND | wxALL, 5);

		// On or off, because that is all the link carries. Whichever pads
		// Dualscreen has behind this controller are the ones that buzz.
		m_rumble = new wxCheckBox(box, wxID_ANY, _("Rumble"));
		m_rumble->SetValue(m_settings.rumble > 0.0f);
		m_rumble->SetToolTip(_("Buzzes whichever controllers Dualscreen has assigned to this one"));
		m_rumble->Bind(wxEVT_CHECKBOX, &DualScreenControllerSettings::on_rumble_change, this);
		box_sizer->Add(m_rumble, 0, wxEXPAND | wxALL, 5);

		sizer->Add(box_sizer, 0, wxALL | wxEXPAND, 5);
	}

	// The two sticks, each with the deadzone and range every other pad here
	// has, and a preview of what the game will read.
	auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
	const auto add_stick = [&](const wxString& title, const ControllerBase::AxisSetting& setting,
	                           wxSlider*& deadzone_slider, wxSlider*& range_slider, wxInputDraw*& draw) {
		auto* box = new wxStaticBox(this, wxID_ANY, title);
		auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);
		auto* content_sizer = new wxFlexGridSizer(0, 3, 0, 0);

		const auto deadzone = (int)(setting.deadzone * 100);
		content_sizer->Add(new wxStaticText(box, wxID_ANY, _("Deadzone")), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		deadzone_slider = new wxSlider(box, wxID_ANY, deadzone, 0, 100);
		content_sizer->Add(deadzone_slider, 1, wxALL | wxEXPAND, 5);
		const auto deadzone_text = new wxStaticText(box, wxID_ANY, wxString::Format("%d%%", deadzone));
		content_sizer->Add(deadzone_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		deadzone_slider->Bind(wxEVT_SLIDER, &DualScreenControllerSettings::on_slider_change, this, wxID_ANY, wxID_ANY,
		                      new wxControlObject(deadzone_text));

		const auto range = (int)(setting.range * 100);
		content_sizer->Add(new wxStaticText(box, wxID_ANY, _("Range")), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		range_slider = new wxSlider(box, wxID_ANY, range, 50, 200);
		content_sizer->Add(range_slider, 1, wxALL | wxEXPAND, 5);
		const auto range_text = new wxStaticText(box, wxID_ANY, wxString::Format("%d%%", range));
		content_sizer->Add(range_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		range_slider->Bind(wxEVT_SLIDER, &DualScreenControllerSettings::on_slider_change, this, wxID_ANY, wxID_ANY,
		                   new wxControlObject(range_text));

		content_sizer->AddSpacer(1);
		draw = new wxInputDraw(box, wxID_ANY, wxDefaultPosition, {60, 60});
		content_sizer->Add(draw, 0, wxTOP | wxBOTTOM | wxALIGN_CENTER, 5);

		box_sizer->Add(content_sizer, 1, wxEXPAND, 0);
		row_sizer->Add(box_sizer, 0, wxALL | wxEXPAND, 5);
	};
	add_stick(_("Left stick"), m_settings.axis, m_left_deadzone, m_left_range, m_left_draw);
	add_stick(_("Right stick"), m_settings.rotation, m_right_deadzone, m_right_range, m_right_draw);
	sizer->Add(row_sizer);

	{
		auto* control_sizer = new wxBoxSizer(wxHORIZONTAL);
		auto* ok_button = new wxButton(this, wxID_ANY, _("OK"));
		ok_button->Bind(wxEVT_BUTTON, [this](auto&) { update_settings(); EndModal(wxID_OK); });
		control_sizer->Add(ok_button, 0, wxALL, 5);
		auto* cancel_button = new wxButton(this, wxID_ANY, _("Cancel"));
		cancel_button->Bind(wxEVT_BUTTON, [this](auto&) { EndModal(wxID_CANCEL); });
		control_sizer->Add(cancel_button, 0, wxALL, 5);
		sizer->Add(control_sizer, 0, wxEXPAND, 5);
	}

	this->SetSizer(sizer);
	this->Layout();
	this->Fit();

	this->Bind(wxEVT_CLOSE_WINDOW, &DualScreenControllerSettings::on_close, this);

	m_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &DualScreenControllerSettings::on_timer, this);
	m_timer->Start(100);
}

DualScreenControllerSettings::~DualScreenControllerSettings()
{
	m_controller->stop_rumble();
	m_timer->Stop();
}

void DualScreenControllerSettings::update_settings()
{
	m_controller->set_settings(m_settings);
	if (m_use_motion->IsEnabled())
		m_controller->set_use_motion(m_use_motion->GetValue());
}

void DualScreenControllerSettings::on_timer(wxTimerEvent& event)
{
	if (m_rumble_time.has_value() &&
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_rumble_time.value()).count() > 500)
	{
		m_controller->stop_rumble();
		m_rumble_time = {};
	}

	// Already y-up, unlike SDL's raw state -- DualScreenController turns the
	// link's y-down sticks round as it reads them -- so drawn as it is.
	const auto& default_state = m_controller->get_default_state();
	auto state = m_controller->raw_state();
	m_controller->apply_axis_setting(state.axis, default_state.axis, m_settings.axis);
	m_controller->apply_axis_setting(state.rotation, default_state.rotation, m_settings.rotation);

	m_left_draw->SetDeadzone(m_settings.axis.deadzone);
	m_left_draw->SetAxisValue(state.axis);
	m_right_draw->SetDeadzone(m_settings.rotation.deadzone);
	m_right_draw->SetAxisValue(state.rotation);
}

void DualScreenControllerSettings::on_close(wxCloseEvent& event)
{
	if (this->GetReturnCode() == 0 || this->GetReturnCode() == wxID_OK)
		update_settings();
	event.Skip();
}

void DualScreenControllerSettings::on_slider_change(wxCommandEvent& event)
{
	update_slider_text(event);
	const auto value = (float)event.GetInt() / 100.0f;
	const auto* source = event.GetEventObject();
	if (source == m_left_deadzone)
		m_settings.axis.deadzone = value;
	else if (source == m_left_range)
		m_settings.axis.range = value;
	else if (source == m_right_deadzone)
		m_settings.rotation.deadzone = value;
	else if (source == m_right_range)
		m_settings.rotation.range = value;
}

void DualScreenControllerSettings::on_rumble_change(wxCommandEvent& event)
{
	m_settings.rumble = m_rumble->GetValue() ? 1.0f : 0.0f;
	// A short buzz on switching it on, the same way the other settings
	// windows' rumble slider does it, so the player can feel which pad it is.
	// start_rumble() reads the controller's own setting, so the new one is
	// lent to it for the moment and handed back.
	if (m_rumble->GetValue())
	{
		const float saved = m_controller->get_settings().rumble;
		m_controller->set_rumble(1.0f);
		m_controller->start_rumble();
		m_controller->set_rumble(saved);
		m_rumble_time = std::chrono::steady_clock::now();
	}
	else
	{
		m_controller->stop_rumble();
		m_rumble_time = {};
	}
}
