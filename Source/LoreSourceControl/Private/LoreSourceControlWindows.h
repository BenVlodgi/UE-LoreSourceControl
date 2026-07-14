// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"

#if WITH_EDITOR && LORE_UE5_1_OR_LATER

class SDockTab;
class SWidget;
class FSpawnTabArgs;
class UToolMenu;
struct FToolMenuSection;

/** Registers the branches window and the history browser as hidden nomad tabs and adds their entries to the Revision Control status-bar menu. The whole surface is gated to 5.1, the plugin's single ToolMenus floor. */
class FLoreSourceControlWindows
{
public:
	void Register();
	void Unregister();

private:
	void RegisterMenus();
	static void ExtendSourceControlMenu(FToolMenuSection& InSection);

	static TSharedRef<SDockTab> OnSpawnBranchesTab(const FSpawnTabArgs& InArgs);
	static TSharedRef<SDockTab> OnSpawnHistoryTab(const FSpawnTabArgs& InArgs);

	static void OpenBranchesTab();
	static void OpenHistoryTab();
};

#endif
