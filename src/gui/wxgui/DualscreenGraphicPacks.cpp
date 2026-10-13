#include "wxgui/DualscreenGraphicPacks.h"

#include "wxgui/wxgui.h"
#include "wxgui/GraphicPacksWindow2.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/CafeSystem.h"

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

	void Answer()
	{
		auto& session = DualscreenSession::instance();
		bool wanted = session.TakeGraphicPacksWanted();
		bool changed = false;
		for (ds::ExternalGraphicPackChange change; session.TakeGraphicPackChange(change);)
		{
			changed = Apply(change) || changed;
			// Answered with the whole list, which a preset can change more
			// of than itself: its conditions show and hide others.
			wanted = true;
		}
		if (changed)
			GraphicPacksWindow2::SaveStateToConfig();
		if (wanted)
			session.SendGraphicPacks(ListForRunningGame());
	}
}

void DualscreenGraphicPacks_Install(wxEvtHandler* owner)
{
	DualscreenSession::instance().SetGraphicPacksHandler([owner]() { owner->CallAfter([]() { Answer(); }); });
}

void DualscreenGraphicPacks_Uninstall()
{
	DualscreenSession::instance().SetGraphicPacksHandler(nullptr);
}
