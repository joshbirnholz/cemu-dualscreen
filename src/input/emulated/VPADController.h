#pragma once

#include "input/emulated/EmulatedController.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "input/motion/MotionHandler.h"


class VPADController : public EmulatedController
{
public:
	enum ButtonId
	{
		kButtonId_None,

		kButtonId_A,
		kButtonId_B,
		kButtonId_X,
		kButtonId_Y,

		kButtonId_L,
		kButtonId_R,
		kButtonId_ZL,
		kButtonId_ZR,

		kButtonId_Plus,
		kButtonId_Minus,

		kButtonId_Up,
		kButtonId_Down,
		kButtonId_Left,
		kButtonId_Right,

		kButtonId_StickL,
		kButtonId_StickR,

		kButtonId_StickL_Up,
		kButtonId_StickL_Down,
		kButtonId_StickL_Left,
		kButtonId_StickL_Right,

		kButtonId_StickR_Up,
		kButtonId_StickR_Down,
		kButtonId_StickR_Left,
		kButtonId_StickR_Right,

		kButtonId_Mic,
		kButtonId_Screen,

		kButtonId_Home,

		kButtonId_Max,
	};

	using EmulatedController::EmulatedController;

	Type type() const override { return VPAD; }

	void VPADRead(VPADStatus_t& status, const BtnRepeat& repeat);

	void update() override;

	uint32 get_emulated_button_flag(uint32 id) const override;

	glm::vec2 get_axis() const override;
	glm::vec2 get_rotation() const override;
	glm::vec2 get_trigger() const override;

	bool is_mic_active() { return m_mic_active; }
	bool is_screen_active() { return m_screen_active; }
	bool is_screen_active_toggle() { return m_screen_active_toggle; }
	void set_screen_toggle(bool toggle) {m_screen_active_toggle = toggle;}

	static std::string_view get_button_name(ButtonId id);

	void clear_rumble();
	bool push_rumble(uint8* pattern, uint8 length);

	size_t get_highest_mapping_id() const override { return kButtonId_Max; }
	bool is_axis_mapping(uint64 mapping) const override { return mapping >= kButtonId_StickL_Up && mapping <= kButtonId_StickR_Right; }

	bool is_start_down() const override { return is_mapping_down(kButtonId_Plus); }
	bool is_left_down() const override { return is_mapping_down(kButtonId_Left); }
	bool is_right_down() const override { return is_mapping_down(kButtonId_Right); }
	bool is_up_down() const override { return is_mapping_down(kButtonId_Up); }
	bool is_down_down() const override { return is_mapping_down(kButtonId_Down); }
	bool is_a_down() const override { return is_mapping_down(kButtonId_A); }
	bool is_b_down() const override { return is_mapping_down(kButtonId_B); }
	bool is_home_down() const override { return is_mapping_down(kButtonId_Home); }

	bool set_default_mapping(const std::shared_ptr<ControllerBase>& controller) override;

	void load(const pugi::xml_node& node) override;
	void save(pugi::xml_node& node) override;

private:
	bool m_mic_active = false;
	bool m_screen_active = false;
	bool m_screen_active_toggle = false;
	uint32be m_last_holdvalue = 0;

	std::chrono::high_resolution_clock::time_point m_last_hold_change{}, m_last_pulse{};

	std::mutex m_rumble_mutex;
	std::chrono::high_resolution_clock::time_point m_last_rumble_check{};
	std::queue<std::vector<bool>> m_rumble_queue;
	uint8 m_parser = 0;

	void update_touch(VPADStatus_t& status);
	void update_motion(VPADStatus_t& status);
	// Fills the motion half of a VPADStatus from one already-fused sample.
	// Shared by update_motion() below and by the Dualscreen path in
	// VPADRead(), so that a GamePad held through the shell and one held
	// through a controller plugged in here report motion through exactly the
	// same arithmetic rather than through two copies of it.
	static void apply_motion_sample(VPADStatus_t& status, MotionSample& sample);
	// Where the shell's touch was last seen, kept for the same reason
	// update_touch keeps its own: a GamePad reports the last point touched
	// rather than nothing once a finger lifts.
	//
	// The only piece of the shell's pad still read directly here. Its buttons,
	// sticks and motion now arrive as an ordinary controller -- see
	// DualScreenControllerProvider -- and go through the player's mapping like
	// anything else; a touch point has no mapping to go through.
	glm::ivec2 m_last_touch_position{};
};
