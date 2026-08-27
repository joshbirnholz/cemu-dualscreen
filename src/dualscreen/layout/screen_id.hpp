#pragma once

namespace ds {

// Which of a console's two screens. "Top" and "Bottom" are this app's names for
// them rather than a claim about where they sit: on a Wii U one of them is
// across the room.
//
// Its own header, and a small one, because ExternalFrameLink is meant to be
// dropped into a fork of somebody else's emulator and this is the only thing it
// needs from the rest of this app. See docs/cemu-integration.md.
enum class ScreenId { Top, Bottom };

}  // namespace ds
