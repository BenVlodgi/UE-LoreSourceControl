// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"

#if WITH_EDITOR && LORE_UE5_1_OR_LATER

#include "AssetRegistry/AssetData.h"
#include "ISourceControlProvider.h"

class UToolMenu;

/** Adds a "Check In Over Lock" entry to the Content Browser asset context menu for files another user has locked. */
class FLoreSourceControlMenu
{
public:
	void Register();
	void Unregister();

private:
	void RegisterMenus();
	static void PopulateContextMenu(UToolMenu* InMenu);
	static void CheckInOverLockClicked(TArray<FAssetData> SelectedAssets);
	static void OnOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);
};

#endif
