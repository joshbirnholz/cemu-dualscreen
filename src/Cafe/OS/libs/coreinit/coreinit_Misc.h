#pragma once

namespace coreinit
{
	uint32 OSGetUPID();
	uint32 OSGetPFID();
	uint64 OSGetTitleID();
	uint32 __OSGetProcessSDKVersion();
	uint32 OSLaunchTitleByPathl(const char* path, uint32 pathLength, uint32 argc);
	uint32 OSRestartGame(uint32 argc, MEMPTR<char>* argv);

	void OSReleaseForeground();

	void StartBackgroundForegroundTransition();

	// Dualscreen's quick menu, standing in for the HOME Menu: see
	// coreinit_Misc.cpp. Called from the GPU thread, as Dualscreen asks.
	void DualscreenRequestBackground();
	void DualscreenRequestForeground();
	// ProcUI, once the game has finished letting go of the foreground.
	void DualscreenForegroundReleased();
	// Whether the game's sound is held while Dualscreen's menu has it in the
	// background -- see AXOut_update().
	bool DualscreenSoundHeld();

	struct OSDriverInterface
	{
		MEMPTR<void> getDriverName;
		MEMPTR<void> init;
		MEMPTR<void> onAcquireForeground;
		MEMPTR<void> onReleaseForeground;
		MEMPTR<void> done;
	};
	static_assert(sizeof(OSDriverInterface) == 0x14);

	uint32 OSDriver_Register(uint32 moduleHandle, sint32 priority, OSDriverInterface* driverCallbacks, sint32 driverId, uint32be* outUkn1, uint32be* outUkn2, uint32be* outUkn3);
	uint32 OSDriver_Deregister(uint32 moduleHandle, sint32 driverId);

	enum class COSReportModule
	{
		coreinit = 0,
	};

	enum class COSReportLevel
	{
		Error = 0,
		Warn = 1,
		Info = 2
	};

	void OSFatal(const char* msg);

	sint32 ppc_vprintf(const char* formatStr, char* strOut, sint32 maxLength, ppc_va_list* vargs);

	void miscInit();
};