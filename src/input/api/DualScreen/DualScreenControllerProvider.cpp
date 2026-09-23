#include "input/api/DualScreen/DualScreenControllerProvider.h"

#include "input/api/DualScreen/DualScreenController.h"
#include "core/external_frame_link.hpp"

std::vector<std::shared_ptr<ControllerBase>> DualScreenControllerProvider::get_controllers()
{
	// Always all of them, whether or not a shell is attached right now -- see
	// the header. A player setting up a game's controls should not have to
	// start a streaming session first just to have something to map, nor
	// plug in a fourth pad to find out Player 4 exists.
	std::vector<std::shared_ptr<ControllerBase>> result;
	result.emplace_back(std::make_shared<DualScreenController>());
	for (int player = 0; player < ds::kExternalPlayers; ++player)
		result.emplace_back(std::make_shared<DualScreenController>(player));
	return result;
}
