#include "input/api/DualScreen/DualScreenControllerProvider.h"

#include "input/api/DualScreen/DualScreenController.h"

std::vector<std::shared_ptr<ControllerBase>> DualScreenControllerProvider::get_controllers()
{
	// Always exactly one, whether or not a shell is attached right now --
	// see the header. A player setting up a game's controls should not have
	// to start a streaming session first just to have something to map.
	std::vector<std::shared_ptr<ControllerBase>> result;
	result.emplace_back(std::make_shared<DualScreenController>());
	return result;
}
