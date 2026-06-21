// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlMenu.h"

#if WITH_EDITOR && LORE_UE5_1_OR_LATER

#include "LoreSourceControlOperations.h"
#include "ISourceControlModule.h"
#include "ISourceControlState.h"
#include "SourceControlHelpers.h"
#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "ContentBrowserMenuContexts.h"
#include "Framework/Commands/UIAction.h"
#include "Textures/SlateIcon.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "LoreSourceControl"

namespace
{
	const FName LoreMenuOwner("LoreSourceControlAssetMenu");
}

void FLoreSourceControlMenu::Register()
{
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLoreSourceControlMenu::RegisterMenus));
}

void FLoreSourceControlMenu::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(LoreMenuOwner);
	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu"))
	{
		// A dynamic section re-evaluates the lock gate every time the menu opens.
		Menu->AddDynamicSection("LoreSourceControl", FNewToolMenuDelegate::CreateStatic(&FLoreSourceControlMenu::PopulateContextMenu));
	}
}

void FLoreSourceControlMenu::Unregister()
{
	UToolMenus::UnRegisterStartupCallback(this);
	if (UObjectInitialized() && UToolMenus::Get())
	{
		UToolMenus::Get()->UnregisterOwnerByName(LoreMenuOwner);
	}
}

void FLoreSourceControlMenu::PopulateContextMenu(UToolMenu* InMenu)
{
	const UContentBrowserAssetContextMenuContext* Context = InMenu->FindContext<UContentBrowserAssetContextMenuContext>();
	if (!Context || Context->SelectedAssets.Num() == 0)
	{
		return;
	}

	if (!ISourceControlModule::Get().IsEnabled())
	{
		return;
	}

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	// Only Lore understands the over-lock operation, so do not offer it under another provider.
	if (!Provider.IsAvailable() || Provider.GetName() != FName("Lore"))
	{
		return;
	}

	bool bAnyLockedByOther = false;
	for (const FAssetData& Asset : Context->SelectedAssets)
	{
		const FString Filename = SourceControlHelpers::PackageFilename(Asset.PackageName.ToString());
		FSourceControlStatePtr State = Provider.GetState(Filename, EStateCacheUsage::Use);
		if (State.IsValid() && State->IsCheckedOutOther())
		{
			bAnyLockedByOther = true;
			break;
		}
	}
	if (!bAnyLockedByOther)
	{
		return;
	}

	TArray<FAssetData> Assets = Context->SelectedAssets;
	FToolMenuSection& Section = InMenu->AddSection("LoreSourceControl", LOCTEXT("LoreSCCHeading", "Lore Revision Control"));
	Section.AddMenuEntry(
		"LoreCheckInOverLock",
		LOCTEXT("LoreCheckInOverLock", "Check In Over Lock..."),
		LOCTEXT("LoreCheckInOverLockTooltip", "Submit these assets even though another user holds the Lore lock."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FLoreSourceControlMenu::CheckInOverLockClicked, Assets)));
}

void FLoreSourceControlMenu::CheckInOverLockClicked(TArray<FAssetData> SelectedAssets)
{
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TArray<FString> PackageNames;
	for (const FAssetData& Asset : SelectedAssets)
	{
		PackageNames.Add(Asset.PackageName.ToString());
	}
	const TArray<FString> Filenames = SourceControlHelpers::PackageFilenames(PackageNames);

	// Re-read state at click time and list the files held by someone else.
	TArray<FString> LockedLines;
	for (const FString& Filename : Filenames)
	{
		FSourceControlStatePtr State = Provider.GetState(Filename, EStateCacheUsage::Use);
		if (!State.IsValid())
		{
			continue;
		}
		FString Who;
		if (State->IsCheckedOutOther(&Who))
		{
			LockedLines.Add(FString::Printf(TEXT("%s  (locked by %s)"), *FPaths::GetCleanFilename(Filename), Who.IsEmpty() ? TEXT("another user") : *Who));
		}
	}

	const FText Body = FText::Format(
		LOCTEXT("LoreOverLockConfirm", "These assets are locked by another user. This does not take their lock; it pushes a new revision over it. On their next sync they must resolve a conflict, and since a binary asset cannot merge, one side's edits are lost.\n\n{0}\n\nSubmit anyway?"),
		FText::FromString(FString::Join(LockedLines, TEXT("\n"))));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Body) != EAppReturnType::Yes)
	{
		return;
	}

	TSharedRef<FLoreCheckInOverLock, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreCheckInOverLock>();
	Operation->SetDescription(LOCTEXT("LoreOverLockDesc", "Submitted over an existing lock."));
	Provider.Execute(Operation, FSourceControlChangelistPtr(), Filenames, EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateStatic(&FLoreSourceControlMenu::OnOperationComplete));
}

void FLoreSourceControlMenu::OnOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	FNotificationInfo Info(InResult == ECommandResult::Succeeded
		? LOCTEXT("LoreOverLockDone", "Checked in over the lock.")
		: LOCTEXT("LoreOverLockFailed", "Check in over the lock failed. See the message log."));
	Info.ExpireDuration = 4.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

#undef LOCTEXT_NAMESPACE

#endif
