# The handover to Dualscreen. Vendored, not written here: these files are
# byte-for-byte copies from that project, so re-syncing is a copy and a diff
# rather than a merge. See README.md beside this file.
#
# Its own library because both CemuCafe (the renderer, and the session) and
# CemuInput (the GamePad) need it, and neither owns it.
add_library(CemuDualscreen
	DualscreenSession.cpp
	DualscreenSession.h
	audio/audio_stretch.cpp
	audio/audio_stretch.hpp
	core/external_frame_link.cpp
	core/external_frame_link.hpp
	layout/screen_id.hpp
)

# PUBLIC so dependents can include "core/external_frame_link.hpp" the way the
# vendored .cpp includes it -- which is what keeps the copies unmodified. Cemu
# has no src/core or src/layout of its own for these to shadow.
target_include_directories(CemuDualscreen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(CemuDualscreen PRIVATE
	CemuCommon
)

set_property(TARGET CemuDualscreen PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
