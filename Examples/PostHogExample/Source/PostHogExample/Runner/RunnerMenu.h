#pragma once

#include "CoreMinimal.h"
#include "RunnerGameMode.h"

enum class ERunnerMenuAction : uint8
{
	StartRun,
	Resume,
	Restart,
	ToggleAnalytics,
	ReloadFlags,
	MainMenu,
	Quit
};

/** The selectable items of each list menu, in display order. */
namespace RunnerMenu
{
	inline TConstArrayView<ERunnerMenuAction> Items(ERunnerMenu Menu)
	{
		static const ERunnerMenuAction Main[] = {
			ERunnerMenuAction::StartRun, ERunnerMenuAction::ToggleAnalytics,
			ERunnerMenuAction::ReloadFlags, ERunnerMenuAction::Quit};
		static const ERunnerMenuAction Pause[] = {
			ERunnerMenuAction::Resume, ERunnerMenuAction::Restart, ERunnerMenuAction::ToggleAnalytics,
			ERunnerMenuAction::MainMenu, ERunnerMenuAction::Quit};
		switch (Menu)
		{
		case ERunnerMenu::Main: return Main;
		case ERunnerMenu::Pause: return Pause;
		default: return {};
		}
	}

	inline int32 Wrap(int32 Index, ERunnerMenu Menu)
	{
		const int32 Count = Items(Menu).Num();
		return Count > 0 ? (Index % Count + Count) % Count : 0;
	}
}
