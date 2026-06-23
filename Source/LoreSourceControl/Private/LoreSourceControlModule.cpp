// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlModule.h"
#include "Features/IModularFeatures.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlLog.h"

#if LORE_WITH_LIBLORE
#include "LoreClient.h"
#endif

#if WITH_EDITOR && LORE_UE5_8_OR_LATER
#include "ISourceControlModule.h"
#include "ISourceControlWindowsModule.h"
#endif

DEFINE_LOG_CATEGORY(LogLoreSourceControl);

#define LOCTEXT_NAMESPACE "LoreSourceControl"

template<typename Type>
static FLoreSourceControlWorkerRef CreateWorker()
{
	return MakeShareable(new Type());
}

#if WITH_EDITOR && LORE_UE5_8_OR_LATER
// The submit dialog title override is global and single-cast across providers, so defer to the engine default unless Lore is active, then name the branch the commit lands on.
static FText LoreSubmitDialogTitle(const FSubmitTitleOverrideParameters&)
{
	if (ISourceControlModule::Get().GetProvider().GetName() != FName("Lore"))
	{
		return NSLOCTEXT("SourceControl.ConfirmSubmit", "Title", "Confirm Submit");
	}

	const FString Branch = FLoreSourceControlModule::Get().GetProvider().GetBranchName();
	if (Branch.IsEmpty())
	{
		return NSLOCTEXT("LoreSourceControl", "SubmitTitle", "Submit to Lore");
	}

	return FText::Format(NSLOCTEXT("LoreSourceControl", "SubmitTitleBranch", "Submit to Lore (branch: {0})"), FText::FromString(Branch));
}
#endif

void FLoreSourceControlModule::StartupModule()
{
	Settings.LoadSettings();

	Provider.RegisterWorker("Connect", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreConnectWorker>));
	Provider.RegisterWorker("UpdateStatus", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreUpdateStatusWorker>));
	// Lore has real locks, so CheckOut acquires an advisory lock.
	Provider.RegisterWorker("CheckOut", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreCheckOutWorker>));
	Provider.RegisterWorker("MarkForAdd", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreMarkForAddWorker>));
	Provider.RegisterWorker("Delete", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreDeleteWorker>));
	Provider.RegisterWorker("Revert", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreRevertWorker>));
	Provider.RegisterWorker("Sync", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreSyncWorker>));
	Provider.RegisterWorker("CheckIn", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreCheckInWorker>));
	Provider.RegisterWorker("CheckInOverLock", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreCheckInOverLockWorker>));
	Provider.RegisterWorker("Copy", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreCopyWorker>));
	Provider.RegisterWorker("Resolve", FGetLoreSourceControlWorker::CreateStatic(&CreateWorker<FLoreResolveWorker>));

	IModularFeatures::Get().RegisterModularFeature("SourceControl", &Provider);

#if WITH_EDITOR && LORE_UE5_1_OR_LATER
	AssetMenu.Register();
#endif

#if WITH_EDITOR && LORE_UE5_8_OR_LATER
	ISourceControlWindowsModule::Get().SubmitTitleOverrideDelegate.BindStatic(&LoreSubmitDialogTitle);
#endif
}

void FLoreSourceControlModule::ShutdownModule()
{
#if WITH_EDITOR && LORE_UE5_1_OR_LATER
	AssetMenu.Unregister();
#endif

#if WITH_EDITOR && LORE_UE5_8_OR_LATER
	if (ISourceControlWindowsModule* WindowsModule = ISourceControlWindowsModule::TryGet())
	{
		WindowsModule->SubmitTitleOverrideDelegate.Unbind();
	}
#endif

	Provider.Close();
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &Provider);

#if LORE_WITH_LIBLORE
	// Stop liblore's worker-thread pool now that the command queue has drained and no synchronous call is in flight.
	LoreClient::Shutdown();
#endif
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLoreSourceControlModule, LoreSourceControl)
