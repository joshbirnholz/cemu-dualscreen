add_library(CemuCommon 
  betype.h
  cpu_features.cpp
  cpu_features.h
  enumFlags.h
  ExceptionHandler/ExceptionHandler.cpp
  ExceptionHandler/ExceptionHandler.h
  FileStream.h
  GLInclude/glext.h
  GLInclude/glFunctions.h
  GLInclude/GLInclude.h
  GLInclude/glxext.h
  GLInclude/khrplatform.h
  GLInclude/wglext.h
  MemPtr.h
  platform.h
  precompiled.cpp
  precompiled.h
  socket.h
  StackAllocator.h
  SysAllocator.cpp
  SysAllocator.h
  CafeString.h
  version.h
)

set_property(TARGET CemuCommon PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WIN32)
	target_sources(CemuCommon PRIVATE
		windows/platform.cpp
		windows/platform.h
		windows/FileStream_win32.cpp
		ExceptionHandler/ExceptionHandler_win32.cpp
	)
else()
target_sources(CemuCommon
PRIVATE
    unix/date.h
    unix/fast_float.h
    unix/platform.cpp
    unix/platform.h
    unix/FileStream_unix.cpp
    unix/FileStream_unix.h
    ExceptionHandler/ExceptionHandler_posix.cpp
)
endif()

# UNIX AND NOT APPLE rather than the built-in LINUX variable: LINUX only
# exists from CMake 3.25 on, silently reading as false (not an error) below
# that -- which this project's own cmake_minimum_required(3.21.1) allows --
# so ELFSymbolTable.cpp went missing from the target with no warning at
# configure time, only an undefined-reference link error from
# ExceptionHandler_posix.cpp, which calls into it unconditionally.
if(UNIX AND NOT APPLE)
	target_sources(CemuCommon PRIVATE
		ExceptionHandler/ELFSymbolTable.cpp
		ExceptionHandler/ELFSymbolTable.h
	)
endif()

# CemuCommon always builts the precompiled header
target_precompile_headers(CemuCommon PRIVATE precompiled.h)
target_include_directories(CemuCommon PUBLIC "../")

target_link_libraries(CemuCommon PRIVATE
	glm::glm
)

if (UNIX AND NOT APPLE)
	target_link_libraries(CemuCommon PRIVATE X11::X11 X11::Xrender X11::Xutil)
endif()

# PUBLIC because:
# - boost/nowide/convert.hpp is included in precompiled.h
# - boost/predef/os.h is included in platform.h
# - fmt/core.h is included in precompiled.h
target_link_libraries(CemuCommon PUBLIC
	Threads::Threads # necessary because some targets may need -pthreads
	Boost::nowide
	Boost::headers
	fmt::fmt
)
