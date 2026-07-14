// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlWindows.h"

#if WITH_EDITOR && LORE_UE5_1_OR_LATER

#include "SLoreSourceControlBranchesWidget.h"
#include "SLoreSourceControlHistoryWidget.h"
#include "ISourceControlModule.h"
#include "Framework/Docking/TabManager.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "Widgets/Docking/SDockTab.h"

#if LORE_UE5_2_OR_LATER
#include "RevisionControlStyle/RevisionControlStyle.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControlWindows"

namespace
{
	const FName LoreWindowsMenuOwner("LoreSourceControlWindows");
	const FName LoreBranchesTabName("LoreSourceControlBranchesWindow");
	const FName LoreHistoryTabName("LoreSourceControlHistoryWindow");
}

void FLoreSourceControlWindows::Register()
{
	// The style manager holds the shared Revision Control icons from 5.2; below that the tabs register iconless, matching the plugin's existing iconless menu entries.
#if LORE_UE5_2_OR_LATER
	const FSlateIcon BranchesIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Branched");
	const FSlateIcon HistoryIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Actions.History");
#else
	const FSlateIcon BranchesIcon;
	const FSlateIcon HistoryIcon;
#endif

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LoreBranchesTabName, FOnSpawnTab::CreateStatic(&FLoreSourceControlWindows::OnSpawnBranchesTab))
		.SetDisplayName(LOCTEXT("BranchesTabTitle", "View Branches"))
		.SetMenuType(ETabSpawnerMenuType::Hidden)
		.SetIcon(BranchesIcon);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LoreHistoryTabName, FOnSpawnTab::CreateStatic(&FLoreSourceControlWindows::OnSpawnHistoryTab))
		.SetDisplayName(LOCTEXT("HistoryTabTitle", "View History"))
		.SetMenuType(ETabSpawnerMenuType::Hidden)
		.SetIcon(HistoryIcon);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLoreSourceControlWindows::RegisterMenus));
}

void FLoreSourceControlWindows::Unregister()
{
	UToolMenus::UnRegisterStartupCallback(this);
	if (UObjectInitialized() && UToolMenus::Get())
	{
		UToolMenus::Get()->UnregisterOwnerByName(LoreWindowsMenuOwner);
	}

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LoreBranchesTabName);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LoreHistoryTabName);
}

void FLoreSourceControlWindows::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(LoreWindowsMenuOwner);

	// The editor's Revision Control status-bar menu, registered by the engine from 5.0; a dynamic section re-evaluates the Lore-active gate every time the menu opens.
	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("StatusBar.ToolBar.SourceControl"))
	{
		Menu->AddDynamicSection("LoreSourceControlWindows", FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
		{
			if (!ISourceControlModule::Get().IsEnabled())
			{
				return;
			}
			ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
			if (!Provider.IsAvailable() || Provider.GetName() != FName("Lore"))
			{
				return;
			}
			FToolMenuSection& Section = InMenu->AddSection("Lore", LOCTEXT("LoreSection", "Lore"), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));
			ExtendSourceControlMenu(Section);
		}));
	}
}

void FLoreSourceControlWindows::ExtendSourceControlMenu(FToolMenuSection& InSection)
{
	InSection.AddMenuEntry(
		"LoreViewBranches",
		LOCTEXT("ViewBranches", "View Branches"),
		LOCTEXT("ViewBranchesTooltip", "Open the Lore branches window."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FLoreSourceControlWindows::OpenBranchesTab)));

	InSection.AddMenuEntry(
		"LoreViewHistory",
		LOCTEXT("ViewHistory", "View History"),
		LOCTEXT("ViewHistoryTooltip", "Open the Lore repository history browser."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FLoreSourceControlWindows::OpenHistoryTab)));
}

TSharedRef<SDockTab> FLoreSourceControlWindows::OnSpawnBranchesTab(const FSpawnTabArgs& InArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SLoreSourceControlBranchesWidget)
		];
}

TSharedRef<SDockTab> FLoreSourceControlWindows::OnSpawnHistoryTab(const FSpawnTabArgs& InArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SLoreSourceControlHistoryWidget)
		];
}

void FLoreSourceControlWindows::OpenBranchesTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(LoreBranchesTabName);
}

void FLoreSourceControlWindows::OpenHistoryTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(LoreHistoryTabName);
}

#undef LOCTEXT_NAMESPACE

#endif
