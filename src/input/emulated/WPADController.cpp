#include <api/Controller.h>
#include "input/emulated/WPADController.h"

#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/WiimoteController.h"
#include "input/api/DualScreen/DualScreenControllerProvider.h"
#if HAS_DUALSCREEN_INPUT
#include "input/api/DualScreen/DualScreenController.h"
#endif

WPADController::WPADController(size_t player_index, WPADDataFormat data_format)
	: EmulatedController(player_index), m_data_format(data_format)
{
}

#if HAS_DUALSCREEN_INPUT
namespace
{
	using ds::ExternalButton;

	template<size_t N>
	uint32 flags_of(const ds::ExternalInput& pad, const std::pair<ExternalButton, uint32> (&table)[N])
	{
		uint32 flags = 0;
		for (const auto& [button, flag] : table)
		{
			if (pad.pressed(button))
				flags |= flag;
		}
		return flags;
	}

	// The remote's own, and a Nunchuk's, which a remote reports with them.
	uint32 remote_buttons(const ds::ExternalInput& pad)
	{
		static constexpr std::pair<ExternalButton, uint32> kRemote[] = {
			{ExternalButton::RemoteA, kWPADButton_A},       {ExternalButton::RemoteB, kWPADButton_B},
			{ExternalButton::One, kWPADButton_1},           {ExternalButton::Two, kWPADButton_2},
			{ExternalButton::RemotePlus, kWPADButton_Plus}, {ExternalButton::RemoteMinus, kWPADButton_Minus},
			{ExternalButton::RemoteHome, kWPADButton_Home}, {ExternalButton::RemoteUp, kWPADButton_Up},
			{ExternalButton::RemoteDown, kWPADButton_Down}, {ExternalButton::RemoteLeft, kWPADButton_Left},
			{ExternalButton::RemoteRight, kWPADButton_Right},
			{ExternalButton::NunchukC, kWPADButton_C},      {ExternalButton::NunchukZ, kWPADButton_Z},
		};
		return flags_of(pad, kRemote);
	}

	uint32 classic_buttons(const ds::ExternalInput& pad)
	{
		static constexpr std::pair<ExternalButton, uint32> kClassic[] = {
			{ExternalButton::A, kCLButton_A},       {ExternalButton::B, kCLButton_B},
			{ExternalButton::X, kCLButton_X},       {ExternalButton::Y, kCLButton_Y},
			{ExternalButton::L, kCLButton_L},       {ExternalButton::R, kCLButton_R},
			{ExternalButton::Zl, kCLButton_ZL},     {ExternalButton::Zr, kCLButton_ZR},
			{ExternalButton::Plus, kCLButton_Plus}, {ExternalButton::Minus, kCLButton_Minus},
			{ExternalButton::Home, kCLButton_Home}, {ExternalButton::Up, kCLButton_Up},
			{ExternalButton::Down, kCLButton_Down}, {ExternalButton::Left, kCLButton_Left},
			{ExternalButton::Right, kCLButton_Right},
		};
		return flags_of(pad, kClassic);
	}

	uint32 pro_buttons(const ds::ExternalInput& pad)
	{
		static constexpr std::pair<ExternalButton, uint32> kPro[] = {
			{ExternalButton::A, kProButton_A},       {ExternalButton::B, kProButton_B},
			{ExternalButton::X, kProButton_X},       {ExternalButton::Y, kProButton_Y},
			{ExternalButton::L, kProButton_L},       {ExternalButton::R, kProButton_R},
			{ExternalButton::Zl, kProButton_ZL},     {ExternalButton::Zr, kProButton_ZR},
			{ExternalButton::Plus, kProButton_Plus}, {ExternalButton::Minus, kProButton_Minus},
			{ExternalButton::Home, kProButton_Home}, {ExternalButton::Up, kProButton_Up},
			{ExternalButton::Down, kProButton_Down}, {ExternalButton::Left, kProButton_Left},
			{ExternalButton::Right, kProButton_Right},
			{ExternalButton::LeftStick, kProButton_StickL}, {ExternalButton::RightStick, kProButton_StickR},
		};
		return flags_of(pad, kPro);
	}

	bool is_classic_format(WPADDataFormat format)
	{
		return format == kDataFormat_CLASSIC || format == kDataFormat_CLASSIC_ACC ||
			   format == kDataFormat_CLASSIC_ACC_DPD;
	}
}

bool WPADController::read_dualscreen(ds::ExternalInput& out, glm::vec2& axis, glm::vec2& rotation) const
{
	const auto& feeding = get_controllers();
	if (feeding.size() != 1 || feeding.front()->api() != InputAPI::DualScreen)
		return false;
	const auto controller = std::static_pointer_cast<DualScreenController>(feeding.front());
	if (!controller->read_pad(out))
		return false;
	axis = controller->get_state().axis;
	rotation = controller->get_state().rotation;
	return true;
}
#else
bool WPADController::read_dualscreen(ds::ExternalInput&, glm::vec2&, glm::vec2&) const
{
	return false;
}
#endif

WPADDataFormat WPADController::get_default_data_format() const
{
	switch (get_device_type())
	{
	case kWAPDevCore:
		return kDataFormat_CORE_ACC_DPD;
	case kWAPDevFreestyle:
		return kDataFormat_FREESTYLE_ACC;
	case kWAPDevClassic:
		return kDataFormat_CLASSIC;
	case kWAPDevMPLS:
		return kDataFormat_MPLS;
	case kWAPDevMPLSFreeStyle:
		return kDataFormat_FREESTYLE_ACC_DPD;
	case kWAPDevMPLSClassic:
		return kDataFormat_CLASSIC_ACC_DPD;
	case kWAPDevURCC:
		return kDataFormat_URCC;
	default:
		return kDataFormat_CORE;
	}
}

uint32 WPADController::get_emulated_button_flag(WPADDataFormat format, uint32 id) const
{
	switch(format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	case kDataFormat_MPLS:
		return WiimoteController::s_get_emulated_button_flag(id);
	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
		return ClassicController::s_get_emulated_button_flag(id);
	
	case kDataFormat_TRAIN: break;
	case kDataFormat_GUITAR: break;
	case kDataFormat_BALANCE_CHECKER: break;
	case kDataFormat_DRUM: break;
	
	case kDataFormat_TAIKO: break;
	case kDataFormat_URCC:
		return ProController::s_get_emulated_button_flag(id);

	}

	return 0;
}

void WPADController::WPADRead(WPADStatus_t* status)
{
	controllers_update_states();
	uint32 button = 0;
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			button |= value;
		}
	}

	m_homebutton_down |= is_home_down();

	// A Dualscreen player's buttons are this controller's already.
	ds::ExternalInput fed;
	glm::vec2 fed_axis{}, fed_rotation{};
	const bool dualscreen = read_dualscreen(fed, fed_axis, fed_rotation);
#if HAS_DUALSCREEN_INPUT
	if (dualscreen)
	{
		button = get_device_type() == kWAPDevURCC ? pro_buttons(fed)
				 : is_classic_format(m_data_format) ? classic_buttons(fed)
													: remote_buttons(fed);
		m_homebutton_down |= fed.pressed(ds::ExternalButton::Home) || fed.pressed(ds::ExternalButton::RemoteHome);
	}
#endif

	// todo fill position api from wiimote

	switch (m_data_format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	{
		memset(status, 0x00, sizeof(*status));
		status->button = button;
		break;
	}

	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	{
		WPADFSStatus_t* ex_status = (WPADFSStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->button = button;

		auto axis = get_axis();
		axis *= 127.0f;
		ex_status->fsStickX = (sint8)axis.x;
		ex_status->fsStickY = (sint8)axis.y;
		break;
	}

	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
	case kDataFormat_GUITAR:
	case kDataFormat_DRUM:
	case kDataFormat_TAIKO:
	{
		WPADCLStatus_t* ex_status = (WPADCLStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->clButton = button;
#if HAS_DUALSCREEN_INPUT
		// The remote it is plugged into has buttons of its own.
		if (dualscreen)
			ex_status->button = remote_buttons(fed);
#endif
		
		auto axis = dualscreen ? fed_axis : get_axis();
		axis *= 2048.0f;
		ex_status->clLStickX = (uint16)axis.x;
		ex_status->clLStickY = (uint16)axis.y;

		auto rotation = dualscreen ? fed_rotation : get_rotation();
		rotation *= 2048.0f;
		ex_status->clRStickX = (uint16)rotation.x;
		ex_status->clRStickY = (uint16)rotation.y;
		break;
	}
	case kDataFormat_TRAIN:
	{
		WPADTRStatus_t* ex_status = (WPADTRStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_BALANCE_CHECKER:
	{
		WPADBLStatus_t* ex_status = (WPADBLStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_MPLS:
	{
		WPADMPStatus_t* ex_status = (WPADMPStatus_t*)status;
		ex_status->stat = 1; // attached
		// TODO
		break;
	}
	case kDataFormat_URCC:
	{
		WPADUCStatus_t* ex_status = (WPADUCStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->ucButton = button;

		ex_status->cable = TRUE;
		ex_status->charge = TRUE;

		auto axis = get_axis();
		axis *= 2048.0f;
		ex_status->ucLStickX = (uint16)axis.x;
		ex_status->ucLStickY = (uint16)axis.y;

		auto rotation = get_rotation();
		rotation *= 2048.0f;
		ex_status->ucRStickX = (uint16)rotation.x;
		ex_status->ucRStickY = (uint16)rotation.y;

		break;
	}
	default:
		cemu_assert(false);
	}

	status->dev = get_device_type();
	status->err = WPAD_ERR_NONE;
}

void WPADController::KPADRead(KPADStatus_t& status, const BtnRepeat& repeat)
{
	uint32be* hold, *release, *trigger;
	switch (type())
	{
	case Pro:
		hold = &status.ex_status.uc.hold;
		release = &status.ex_status.uc.release;
		trigger = &status.ex_status.uc.trig;
		break;
	case Classic:
		hold = &status.ex_status.cl.hold;
		release = &status.ex_status.cl.release;
		trigger = &status.ex_status.cl.trig;
		break;
	default:
		hold = &status.hold;
		release = &status.release;
		trigger = &status.trig;
	}

	controllers_update_states();
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			*hold |= value;
		}
	}

	m_homebutton_down |= is_home_down();

	// A Dualscreen player's buttons are this controller's already: a Wii
	// Remote's in the remote's place, and a Classic Controller plugged into
	// it in its own, below.
	ds::ExternalInput fed;
	glm::vec2 fed_axis{}, fed_rotation{};
	const bool dualscreen = read_dualscreen(fed, fed_axis, fed_rotation);
#if HAS_DUALSCREEN_INPUT
	if (dualscreen)
	{
		*hold = type() == Pro       ? pro_buttons(fed)
				: type() == Classic ? classic_buttons(fed)
									: remote_buttons(fed);
		m_homebutton_down |= fed.pressed(ds::ExternalButton::Home) || fed.pressed(ds::ExternalButton::RemoteHome);
	}
#endif
	
	// button repeat
	const auto now = std::chrono::steady_clock::now();
	if (*hold != m_last_holdvalue)
	{
		m_last_hold_change = m_last_pulse = now;
	}

	if (repeat.pulse > 0)
	{
		if (m_last_hold_change + std::chrono::milliseconds(repeat.delay) >= now)
		{
			if ((m_last_pulse + std::chrono::milliseconds(repeat.pulse)) < now)
			{
				m_last_pulse = now;
				*hold |= kWPADButtonRepeat;
			}
		}
	}

	// axis
	const auto axis = get_axis();
	const auto rotation = get_rotation();

	*release = m_last_holdvalue & ~*hold;
	//status.release = m_last_holdvalue & ~*hold;
	*trigger = ~m_last_holdvalue & *hold;
	//status.trig = ~m_last_holdvalue & *hold;
	m_last_holdvalue = *hold;

	if (is_mpls_attached())
	{
		status.mpls.dir.X.x = 1;
		status.mpls.dir.X.y = 0;
		status.mpls.dir.X.z = 0;

		status.mpls.dir.Y.x = 0;
		status.mpls.dir.Y.y = 1;
		status.mpls.dir.Y.z = 0;

		status.mpls.dir.Z.x = 0;
		status.mpls.dir.Z.y = 0;
		status.mpls.dir.Z.z = 1;
	}

	if (has_motion())
	{
		auto motion_sample = get_motion_data();

		glm::vec3 acc;
		motion_sample.getAccelerometer(&acc[0]);
		status.acc.x = acc.x;
		status.acc.y = acc.y;
		status.acc.z = acc.z;

		status.acc_value = motion_sample.getVPADAccMagnitude();
		status.acc_speed = motion_sample.getVPADAccAcceleration();

		//glm::vec2 acc_vert;
		//motion_sample.getVPADAccXY(&acc_vert[0]);
		//status.acc_vertical.x = acc_vert.x;
		//status.acc_vertical.y = acc_vert.y;

		status.accVertical.x = std::min(1.0f, std::abs(acc.x + acc.y));
		status.accVertical.y = std::min(std::max(-1.0f, -acc.z), 1.0f);

		if (is_mpls_attached())
		{
			// todo
			glm::vec3 gyroChange;
			motion_sample.getVPADGyroChange(&gyroChange[0]);
			//const auto& gyroChange = motionSample.getVPADGyroChange();
			status.mpls.mpls.x = gyroChange.x;
			status.mpls.mpls.y = gyroChange.y;
			status.mpls.mpls.z = gyroChange.z;

			//debug_printf("GyroChange %7.2lf %7.2lf %7.2lf\n", (float)status.gyroChange.x, (float)status.gyroChange.y, (float)status.gyroChange.z);

			glm::vec3 gyroOrientation;
			motion_sample.getVPADOrientation(&gyroOrientation[0]);
			//const auto& gyroOrientation = motionSample.getVPADOrientation();
			status.mpls.angle.x = gyroOrientation.x;
			status.mpls.angle.y = gyroOrientation.y;
			status.mpls.angle.z = gyroOrientation.z;

			float attitude[9];
			motion_sample.getVPADAttitudeMatrix(attitude);
			status.mpls.dir.X.x = attitude[0];
			status.mpls.dir.X.y = attitude[1];
			status.mpls.dir.X.z = attitude[2];
			status.mpls.dir.Y.x = attitude[3];
			status.mpls.dir.Y.y = attitude[4];
			status.mpls.dir.Y.z = attitude[5];
			status.mpls.dir.Z.x = attitude[6];
			status.mpls.dir.Z.y = attitude[7];
			status.mpls.dir.Z.z = attitude[8];
		}
	}
	auto visibility = GetPositionVisibility();
	if (has_position() && visibility != PositionVisibility::NONE)
	{
		if (visibility == PositionVisibility::FULL)
			status.dpd_valid_fg = 2;
		else
			status.dpd_valid_fg = -1;

		const auto position = get_position();

		const auto pos = (position * 2.0f) - 1.0f;
		status.pos.x = pos.x;
		status.pos.y = pos.y;

		const auto delta = position - get_prev_position();
		status.vec.x = delta.x;
		status.vec.y = delta.y;
		status.speed = glm::length(delta);
	}
	else
		status.dpd_valid_fg = 0;

	switch (type())
	{
	case Wiimote:
		status.ex_status.fs.stick.x = axis.x;
		status.ex_status.fs.stick.y = axis.y;

		if(has_second_motion())
		{
			auto motion_sample = get_second_motion_data();

			glm::vec3 acc;
			motion_sample.getAccelerometer(&acc[0]);
			status.ex_status.fs.acc.x = acc.x;
			status.ex_status.fs.acc.y = acc.y;
			status.ex_status.fs.acc.z = acc.z;

			status.ex_status.fs.accValue = motion_sample.getVPADAccMagnitude();
			status.ex_status.fs.accSpeed = motion_sample.getVPADAccAcceleration();
		}

		break;
	case Pro:
		status.ex_status.uc.lstick.x = axis.x;
		status.ex_status.uc.lstick.y = axis.y;

		status.ex_status.uc.rstick.x = rotation.x;
		status.ex_status.uc.rstick.y = rotation.y;

		status.ex_status.uc.charge = FALSE;
		status.ex_status.uc.cable = TRUE;

		break;
	case Classic:
		status.ex_status.cl.lstick.x = axis.x;
		status.ex_status.cl.lstick.y = axis.y;

		status.ex_status.cl.rstick.x = rotation.x;
		status.ex_status.cl.rstick.y = rotation.y;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZL))
			status.ex_status.cl.ltrigger = 1.0f;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZR))
			status.ex_status.cl.rtrigger = 1.0f;
		break;
	default:
		cemu_assert(false);
	}

#if HAS_DUALSCREEN_INPUT
	// A Classic Controller in a Wii Remote: its own buttons and sticks, over
	// the Nunchuk's that share their place.
	if (dualscreen && type() == Wiimote && is_classic_format(m_data_format))
	{
		const uint32 classic = classic_buttons(fed);
		const uint32 last = m_last_classic_holdvalue;
		status.ex_status.cl.hold = classic;
		status.ex_status.cl.trig = ~last & classic;
		status.ex_status.cl.release = last & ~classic;
		m_last_classic_holdvalue = classic;
		status.ex_status.cl.lstick.x = fed_axis.x;
		status.ex_status.cl.lstick.y = fed_axis.y;
		status.ex_status.cl.rstick.x = fed_rotation.x;
		status.ex_status.cl.rstick.y = fed_rotation.y;
		status.ex_status.cl.ltrigger = HAS_FLAG(classic, kCLButton_ZL) ? 1.0f : 0.0f;
		status.ex_status.cl.rtrigger = HAS_FLAG(classic, kCLButton_ZR) ? 1.0f : 0.0f;
	}
#endif
}
