#include "wxgui/DualscreenRequests.h"

#include "wxgui/wxgui.h"
#include "wxgui/GraphicPacksWindow2.h"
#include "wxgui/MainWindow.h"
#include "wxgui/wxCemuConfig.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "config/CemuConfig.h"

#include "DualscreenSession.h"

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

	// Cemu's own settings that Dualscreen's Options page shows: each with
	// the words Dualscreen's menus use, the values it takes, and how to read
	// and set it. All of them take effect at once, as they do from Cemu's
	// General settings window, which only ever sets the value too.
	struct Option
	{
		const char* id;
		const char* label;
		std::vector<std::string> choices;
		int (*get)();
		void (*set)(int);
	};

	const std::vector<Option>& Options()
	{
		static const std::vector<Option> s_options = {
			// The order of Cemu's UpscalingFilter.
			{"upscale_filter", "Upscaling", {"Bilinear", "Bicubic", "Hermite", "Nearest neighbor"},
			 []() { return (int)GetConfig().upscale_filter.GetValue(); },
			 [](int choice) { GetConfig().upscale_filter = choice; }},
			// The order of Cemu's FullscreenScaling.
			{"fullscreen_scaling", "Aspect ratio", {"Original", "Stretched"},
			 []() { return (int)GetConfig().fullscreen_scaling.GetValue(); },
			 [](int choice) { GetConfig().fullscreen_scaling = choice; }},
			// Cemu's overlay is a position and a tick for each line; these
			// are the few combinations worth stepping through, in the corner
			// it was last put in, or the top left.
			{"performance_overlay", "Performance overlay",
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
			 }},
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
			const int at = option.get();
			entry.active = at >= 0 && at < (int)option.choices.size() ? at : -1;
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
