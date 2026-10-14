#include "wxgui/DualscreenRequests.h"

#include "wxgui/wxgui.h"
#include "wxgui/GraphicPacksWindow2.h"
#include "wxgui/MainWindow.h"
#include "wxgui/wxCemuConfig.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "config/CemuConfig.h"

#include "DualscreenSession.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "input/emulated/WiimoteController.h"
#include "input/api/DualScreen/DualScreenController.h"

namespace
{
	// The running game's packs, as Cemu's window shows them with that game
	// selected: universal ones too, and only the presets their conditions
	// leave showing.
	std::vector<ds::ExternalGraphicPack> ListForRunningGame()
	{
		std::vector<ds::ExternalGraphicPack> packs;
		if (!CafeSystem::IsTitleRunning())
			return packs;
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		for (const auto& gp : GraphicPack2::GetGraphicPacks())
		{
			if (!gp->ContainsTitleId(titleId))
				continue;
			ds::ExternalGraphicPack pack;
			pack.id = gp->GetNormalizedPathString();
			pack.path = gp->GetVirtualPath();
			pack.name = gp->GetName();
			pack.description = gp->GetDescription();
			pack.enabled = gp->IsEnabled();
			pack.restart_needed = gp->RequiresRestart(true, true);
			std::vector<std::string> order;
			auto categorized = gp->GetCategorizedPresets(order);
			for (const auto& category : order)
			{
				ds::ExternalGraphicPack::Choice choice;
				choice.category = category;
				for (const auto& preset : categorized[category])
				{
					if (!preset->visible)
						continue;
					if (preset->active)
						choice.active = (int)choice.presets.size();
					choice.presets.push_back(preset->name);
				}
				if (!choice.presets.empty())
					pack.choices.push_back(std::move(choice));
			}
			packs.push_back(std::move(pack));
		}
		return packs;
	}

	// What OnTreeChoiceChanged() and OnActivePresetChanged() do in Cemu's
	// window, but for the pack Dualscreen named. False when there is no such
	// pack, or nothing changed.
	bool Apply(const ds::ExternalGraphicPackChange& change)
	{
		GraphicPackPtr pack;
		for (const auto& gp : GraphicPack2::GetGraphicPacks())
		{
			if (gp->GetNormalizedPathString() == change.id)
			{
				pack = gp;
				break;
			}
		}
		if (!pack)
		{
			cemuLog_log(LogType::Force, "Dualscreen: no graphics pack {} to change", change.id);
			return false;
		}
		const bool running = CafeSystem::IsTitleRunning() &&
							 pack->ContainsTitleId(CafeSystem::GetForegroundTitleId());
		if (change.kind == ds::ExternalGraphicPackChange::Kind::Enabled)
		{
			if (pack->IsEnabled() == change.enabled)
				return false;
			pack->SetEnabled(change.enabled);
			const bool restart = pack->RequiresRestart(true, false);
			if (running)
			{
				if (change.enabled)
				{
					GraphicPack2::ActivateGraphicPack(pack);
					if (!restart)
						GraphicPacksWindow2::ReloadPack(pack);
				}
				else
				{
					if (!restart)
						GraphicPacksWindow2::DeleteShadersFromRuntimeCache(pack);
					GraphicPack2::DeactivateGraphicPack(pack);
				}
			}
			cemuLog_log(LogType::Force, "Dualscreen: graphics pack {} turned {}{}", pack->GetName(),
						change.enabled ? "on" : "off",
						running && restart ? ", for when the game starts again" : "");
			return true;
		}
		pack->SetActivePreset(change.category, change.preset);
		// Only a pack in use has anything to load again; Reload() would
		// otherwise put one in use that is turned off.
		const bool restart = pack->RequiresRestart(false, true);
		if (running && pack->IsActivated() && !restart)
			GraphicPacksWindow2::ReloadPack(pack);
		cemuLog_log(LogType::Force, "Dualscreen: graphics pack {}: {}{}{}", pack->GetName(),
					change.category.empty() ? "" : change.category + " ", change.preset,
					running && restart ? ", for when the game starts again" : "");
		return true;
	}

	// Cemu's own settings that a Wii U game's Options in Dualscreen's quick
	// menu show: each with the words Dualscreen's menus use, the values it
	// takes, and how to read and set it.
	//
	// The first few Cemu keeps for every game alike; Dualscreen keeps each
	// game's choice and sets it as the game starts, falling back on
	// `defaultChoice`, so they behave per game too. They take effect at once,
	// as from Cemu's General settings window, which only ever sets the value.
	// CPU mode is Cemu's own per game, in the game's profile, and is read
	// when the game starts.
	struct Option
	{
		const char* id;
		const char* label;
		std::vector<std::string> choices;
		int (*get)();
		void (*set)(int);
		int defaultChoice = -1;
		bool restartNeeded = false;
		// A default worked out rather than fixed -- see PlayerTypeDefault().
		int (*defaultOf)() = nullptr;
	};

	// What each of Dualscreen's players is to the game, in the order of
	// kPlayerTypeChoices: a Pro Controller, a Classic Controller Pro, or a Wii
	// Remote alone or with an attachment -- or nothing at all.
	const std::vector<std::string> kPlayerTypeChoices = {
		"Pro Controller", "Classic Controller", "Wii Remote", "Wii Remote + Nunchuk",
		"Wii Remote + Classic Controller", "None"};
	constexpr int kPlayerTypeNone = 5;

	// The DualScreen controller for `player` (0 for Player 1), which is the
	// one the shell feeds that player's pad through.
	std::shared_ptr<ControllerBase> DualscreenPlayerController(int player)
	{
		const auto provider = InputManager::instance().get_api_provider(InputAPI::DualScreen);
		if (!provider)
			return {};
		const std::string uuid = DualScreenController::uuid_for(player);
		for (const auto& controller : provider->get_controllers())
		{
			if (controller->uuid() == uuid)
				return controller;
		}
		return {};
	}

	// Which of Cemu's controller slots `player` is: the one its DualScreen
	// controller is assigned to, or the slot after the GamePad's by number.
	size_t SlotForPlayer(int player)
	{
		auto& input = InputManager::instance();
		const std::string uuid = DualScreenController::uuid_for(player);
		for (size_t slot = 0; slot < InputManager::kMaxController; ++slot)
		{
			const auto emulated = input.get_controller(slot);
			if (!emulated)
				continue;
			for (const auto& controller : emulated->get_controllers())
			{
				if (controller->api() == InputAPI::DualScreen && controller->uuid() == uuid)
					return slot;
			}
		}
		return std::min<size_t>(player + 1, InputManager::kMaxController - 1);
	}

	int PlayerType(int player)
	{
		const auto emulated = InputManager::instance().get_controller(SlotForPlayer(player));
		if (!emulated)
			return kPlayerTypeNone;
		switch (emulated->type())
		{
		case EmulatedController::Type::Pro: return 0;
		case EmulatedController::Type::Classic: return 1;
		case EmulatedController::Type::Wiimote:
			switch (std::static_pointer_cast<WiimoteController>(emulated)->get_device_type())
			{
			case kWAPDevFreestyle:
			case kWAPDevMPLSFreeStyle: return 3;
			case kWAPDevClassic:
			case kWAPDevMPLSClassic: return 4;
			default: return 2;
			}
		default: return -1;  // the GamePad's slot: not a player's to say
		}
	}

	// Made what the game's controls want for the session -- never saved over
	// the controller profiles, so Cemu on its own is as it was. A slot that is
	// already that keeps its own mapping; a new one gets the default mapping
	// for whatever feeds it, as a fresh Cemu does.
	void SetPlayerType(int player, int choice)
	{
		auto& input = InputManager::instance();
		const size_t slot = SlotForPlayer(player);
		if (choice == kPlayerTypeNone)
		{
			input.delete_controller(slot);
			return;
		}
		const auto type = choice == 0   ? EmulatedController::Type::Pro
						  : choice == 1 ? EmulatedController::Type::Classic
										: EmulatedController::Type::Wiimote;
		const auto previous = input.get_controller(slot);
		std::vector<std::shared_ptr<ControllerBase>> feeding;
		if (previous)
			feeding = previous->get_controllers();
		if (feeding.empty())
		{
			if (auto own = DualscreenPlayerController(player))
				feeding.push_back(own);
		}
		auto emulated = ControllerFactory::CreateEmulatedController(slot, type);
		for (const auto& controller : feeding)
		{
			emulated->add_controller(controller);
			emulated->set_default_mapping(controller);
		}
		if (type == EmulatedController::Type::Wiimote)
		{
			std::static_pointer_cast<WiimoteController>(emulated)->set_device_type(
				choice == 3 ? kWAPDevFreestyle : choice == 4 ? kWAPDevClassic : kWAPDevCore);
		}
		input.set_controller(emulated);
	}

	// What each player was when Cemu started -- its controller profiles, or
	// the game profile's -- which a game Dualscreen has kept no choice for
	// goes back to, so one game's never carries into the next.
	int PlayerTypeDefault(int player)
	{
		static std::array<int, 4> s_started{-2, -2, -2, -2};
		if (s_started[player] == -2)
			s_started[player] = PlayerType(player);
		return s_started[player];
	}

	template<int Player> int GetPlayerType() { return PlayerType(Player); }
	template<int Player> void SetPlayerTypeOf(int choice) { SetPlayerType(Player, choice); }
	template<int Player> int PlayerTypeDefaultOf() { return PlayerTypeDefault(Player); }

	// This game's profile, read afresh: the Game Profile window may have
	// changed it since.
	std::optional<GameProfile> RunningGameProfile()
	{
		if (!CafeSystem::IsTitleRunning())
			return std::nullopt;
		GameProfile profile;
		profile.Load(CafeSystem::GetForegroundTitleId());
		return profile;
	}

	const std::vector<Option>& Options()
	{
		static const std::vector<Option> s_options = {
			// The order of Cemu's UpscalingFilter.
			{"upscale_filter", "Upscaling", {"Bilinear", "Bicubic", "Hermite", "Nearest neighbor"},
			 []() { return (int)GetConfig().upscale_filter.GetValue(); },
			 [](int choice) { GetConfig().upscale_filter = choice; }, kBicubicFilter},
			// The order of Cemu's FullscreenScaling.
			{"fullscreen_scaling", "Aspect Ratio", {"Original", "Stretched"},
			 []() { return (int)GetConfig().fullscreen_scaling.GetValue(); },
			 [](int choice) { GetConfig().fullscreen_scaling = choice; }, kKeepAspectRatio},
			// Cemu's overlay is a position and a tick for each line; these
			// are the few combinations worth stepping through, in the corner
			// it was last put in, or the top left.
			{"performance_overlay", "Performance Overlay",
			 {"Off", "Frame rate", "Frame rate and CPU", "Everything"},
			 []() {
				 const auto& overlay = GetConfig().overlay;
				 if (overlay.position == ScreenPosition::kDisabled)
					 return 0;
				 if (overlay.ram_usage || overlay.vram_usage)
					 return 3;
				 return overlay.cpu_usage ? 2 : 1;
			 },
			 [](int choice) {
				 auto& overlay = GetConfig().overlay;
				 if (choice == 0)
				 {
					 overlay.position = ScreenPosition::kDisabled;
					 return;
				 }
				 if (overlay.position == ScreenPosition::kDisabled)
					 overlay.position = ScreenPosition::kTopLeft;
				 overlay.fps = true;
				 overlay.cpu_usage = choice >= 2;
				 overlay.ram_usage = choice >= 3;
				 overlay.vram_usage = choice >= 3;
			 }, 0},
			// The TV's sound, as Cemu's Audio settings offer it: Dualscreen
			// keeps it in its own Settings, for every game alike, and tells it
			// here. Read as a game starts, which opens the TV's device.
			{"tv_channels", "TV audio", {"Mono", "Stereo", "Surround"},
			 []() { return (int)GetConfig().tv_channels; },
			 [](int choice) { GetConfig().tv_channels = (AudioChannels)choice; }, kStereo},
			// As the Game Profile window offers it, less the interpreter,
			// which is for debugging.
			{"cpu_mode", "CPU Mode", {"Auto", "Multi-core", "Single-core"},
			 []() {
				 const auto profile = RunningGameProfile();
				 if (!profile)
					 return -1;
				 const auto& mode = profile->GetCPUMode();
				 if (!mode || *mode == CPUMode::Auto)
					 return 0;
				 if (*mode == CPUMode::MulticoreRecompiler || *mode == CPUMode::DualcoreRecompiler)
					 return 1;
				 return *mode == CPUMode::SinglecoreRecompiler ? 2 : -1;
			 },
			 [](int choice) {
				 auto profile = RunningGameProfile();
				 if (!profile)
					 return;
				 profile->SetCPUMode(choice == 1   ? CPUMode::MulticoreRecompiler
									 : choice == 2 ? CPUMode::SinglecoreRecompiler
												   : CPUMode::Auto);
				 profile->Save(CafeSystem::GetForegroundTitleId());
			 },
			 -1, true},
			// The game's other controllers, each one's type as this game wants
			// it, kept for the game by Dualscreen.
			{"player1_type", "Player 1 Controller", kPlayerTypeChoices, &GetPlayerType<0>, &SetPlayerTypeOf<0>,
			 -1, false, &PlayerTypeDefaultOf<0>},
			{"player2_type", "Player 2 Controller", kPlayerTypeChoices, &GetPlayerType<1>, &SetPlayerTypeOf<1>,
			 -1, false, &PlayerTypeDefaultOf<1>},
			{"player3_type", "Player 3 Controller", kPlayerTypeChoices, &GetPlayerType<2>, &SetPlayerTypeOf<2>,
			 -1, false, &PlayerTypeDefaultOf<2>},
			{"player4_type", "Player 4 Controller", kPlayerTypeChoices, &GetPlayerType<3>, &SetPlayerTypeOf<3>,
			 -1, false, &PlayerTypeDefaultOf<3>},
		};
		return s_options;
	}

	std::vector<ds::ExternalOption> ListOptions()
	{
		std::vector<ds::ExternalOption> list;
		for (const Option& option : Options())
		{
			ds::ExternalOption entry;
			entry.id = option.id;
			entry.label = option.label;
			entry.choices = option.choices;
			// One about the running game has nothing to say without one.
			if (option.restartNeeded && !CafeSystem::IsTitleRunning())
				continue;
			const int at = option.get();
			entry.active = at >= 0 && at < (int)option.choices.size() ? at : -1;
			entry.default_choice = option.defaultOf ? option.defaultOf() : option.defaultChoice;
			entry.restart_needed = option.restartNeeded;
			list.push_back(std::move(entry));
		}
		return list;
	}

	bool ApplyOption(const std::string& id, int choice)
	{
		for (const Option& option : Options())
		{
			if (id != option.id)
				continue;
			if (choice < 0 || choice >= (int)option.choices.size() || choice == option.get())
				return false;
			option.set(choice);
			cemuLog_log(LogType::Force, "Dualscreen: {}: {}", option.label, option.choices[choice]);
			return true;
		}
		cemuLog_log(LogType::Force, "Dualscreen: no option {} to change", id);
		return false;
	}

	// What dropping an amiibo's file on the window does: held to the
	// GamePad for a moment, and added to Cemu's recent ones.
	ds::ExternalAmiiboResult ScanAmiibo(MainWindow* window, const std::string& path)
	{
		if (!CafeSystem::IsTitleRunning())
			return ds::ExternalAmiiboResult::NoGame;
		uint32 error = NFC_TOUCH_TAG_ERROR_NONE;
		if (!nfc::TouchTagFromFile(_utf8ToPath(path), &error))
		{
			cemuLog_log(LogType::Force, "Dualscreen: could not scan {} as an amiibo", path);
			return error == NFC_TOUCH_TAG_ERROR_NO_ACCESS ? ds::ExternalAmiiboResult::CannotOpen
														  : ds::ExternalAmiiboResult::NotAnAmiibo;
		}
		cemuLog_log(LogType::Force, "Dualscreen: scanned the amiibo {}", path);
		GetWxGUIConfig().AddRecentNfcFile(path);
		window->UpdateNFCMenu();
		return ds::ExternalAmiiboResult::Scanned;
	}

	void Answer(MainWindow* window)
	{
		auto& session = DualscreenSession::instance();

		bool packsWanted = session.TakeGraphicPacksWanted();
		bool packsChanged = false;
		for (ds::ExternalGraphicPackChange change; session.TakeGraphicPackChange(change);)
		{
			packsChanged = Apply(change) || packsChanged;
			// Answered with the whole list, which a preset can change more
			// of than itself: its conditions show and hide others.
			packsWanted = true;
		}
		if (packsChanged)
			GraphicPacksWindow2::SaveStateToConfig();
		if (packsWanted)
			session.SendGraphicPacks(ListForRunningGame());

		bool optionsWanted = session.TakeOptionsWanted();
		bool optionsChanged = false;
		std::string id;
		for (int choice = 0; session.TakeOptionChange(id, choice);)
		{
			optionsChanged = ApplyOption(id, choice) || optionsChanged;
			optionsWanted = true;
		}
		if (optionsChanged)
			GetConfigHandle().Save();
		if (optionsWanted)
			session.SendOptions(ListOptions());

		for (std::string path; session.TakeAmiiboScan(path);)
			session.SendAmiiboResult(ScanAmiibo(window, path));
	}
}

void DualscreenRequests_Install(MainWindow* window)
{
	DualscreenSession::instance().SetRequestHandler(
		[window]() { window->CallAfter([window]() { Answer(window); }); });
}

void DualscreenRequests_Uninstall()
{
	DualscreenSession::instance().SetRequestHandler(nullptr);
}
