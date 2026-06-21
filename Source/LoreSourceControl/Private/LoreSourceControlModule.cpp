// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlModule.h"
#include "Features/IModularFeatures.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlLog.h"

DEFINE_LOG_CATEGORY(LogLoreSourceControl);

#define LOCTEXT_NAMESPACE "LoreSourceControl"

template<typename Type>
static FLoreSourceControlWorkerRef CreateWorker()
{
	return MakeShareable(new Type());
}

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
}

void FLoreSourceControlModule::ShutdownModule()
{
#if WITH_EDITOR && LORE_UE5_1_OR_LATER
	AssetMenu.Unregister();
#endif

	Provider.Close();
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &Provider);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLoreSourceControlModule, LoreSourceControl)
