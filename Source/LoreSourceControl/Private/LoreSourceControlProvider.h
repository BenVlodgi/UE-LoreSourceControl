// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#if LORE_UE5_0_OR_LATER
#include "ISourceControlChangelist.h"
#include "ISourceControlChangelistState.h"
#endif
#include "LoreSourceControlState.h"
#include "ILoreSourceControlWorker.h"

class FLoreSourceControlCommand;
class UPackage;
#if LORE_UE5_0_OR_LATER
class FObjectPostSaveContext;
#endif

DECLARE_DELEGATE_RetVal(FLoreSourceControlWorkerRef, FGetLoreSourceControlWorker);

class FLoreSourceControlProvider : public ISourceControlProvider
{
public:
	//~ Begin ISourceControlProvider interface
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual const FName& GetName() const override;
	virtual FText GetStatusText() const override;
#if LORE_UE5_3_OR_LATER
	virtual TMap<EStatus, FString> GetStatus() const override;
#endif
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override;
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRoot) override;
	virtual int32 GetStateBranchIndex(const FString& BranchName) const override;
#if LORE_UE5_7_OR_LATER
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override;
#endif
	virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
#if LORE_UE5_0_OR_LATER
	virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
#endif
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
#if LORE_UE5_0_OR_LATER
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
#else
	virtual ECommandResult::Type Execute(const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
#endif

#if LORE_UE5_3_OR_LATER
	virtual bool CanExecuteOperation(const FSourceControlOperationRef& InOperation) const override;
#endif
	virtual bool CanCancelOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual void CancelOperation(const FSourceControlOperationRef& InOperation) override;
	virtual TArray<TSharedRef<class ISourceControlLabel>> GetLabels(const FString& InMatchingSpec) const override;
#if LORE_UE5_0_OR_LATER
	virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type InStateCacheUsage) override;
#endif
	virtual bool UsesLocalReadOnlyState() const override;
	virtual bool UsesChangelists() const override;
#if LORE_UE5_2_OR_LATER
	virtual bool UsesUncontrolledChangelists() const override;
#endif
	virtual bool UsesCheckout() const override;
#if LORE_UE5_1_OR_LATER
	virtual bool UsesFileRevisions() const override;
#endif

#if LORE_UE5_2_OR_LATER
	virtual bool UsesSnapshots() const override;
	virtual bool AllowsDiffAgainstDepot() const override;
#endif

#if LORE_UE5_8_OR_LATER
	virtual bool UsesSoftRevertOnDelete() const override;
	virtual TOptional<bool> HasChangesToSync() const override;
	virtual TOptional<bool> HasChangesToCheckIn() const override;
#endif

#if LORE_UE_HAS_LATEST_REVISION_QUERY
	virtual TOptional<bool> IsAtLatestRevision() const override;
	virtual TOptional<int> GetNumLocalChanges() const override;
#endif
	virtual void Tick() override;
#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif
	//~ End ISourceControlProvider interface

	/** Register a worker with the provider, so it can map an operation name to the work. */
	void RegisterWorker(const FName& InName, const FGetLoreSourceControlWorker& InDelegate);

	/** Path to Lore binary */
	inline const FString& GetLoreBinaryPath() const
	{
		return PathToLoreBinary;
	}

	/** Repository root (the project directory or any parent) */
	inline const FString& GetPathToRepositoryRoot() const
	{
		return PathToRepositoryRoot;
	}

	/** Commit identity (email) from working tree configuration */
	inline const FString& GetIdentity() const
	{
		return Identity;
	}

	/** Name of current branch */
	inline const FString& GetBranchName() const
	{
		return BranchName;
	}

	/** Remote the working tree pushes to */
	inline const FString& GetRemoteUrl() const
	{
		return RemoteUrl;
	}

	/** Repository identifier from the server */
	inline const FString& GetRepositoryId() const
	{
		return RepositoryId;
	}

	/** Version string of the Lore client */
	inline const FString& GetLoreVersion() const
	{
		return LoreVersion;
	}

	/** Is the Lore binary found and working */
	inline bool IsLoreAvailable() const
	{
		return bLoreAvailable;
	}

	/** Set by the connect worker on the game thread to refresh the discovered repository information. */
	void SetRepositoryInfo(bool bInAvailable, bool bInRepositoryFound, const FString& InRepositoryRoot, const FString& InRemoteUrl, const FString& InIdentity, const FString& InBranchName, const FString& InRepositoryId, const FString& InLoreVersion);

	/** Helper function used to update the state cache */
	TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& InFilename);

private:
	/** The editor refreshes status before a save but not after, so flip a saved tracked file to modified here. */
#if LORE_UE5_0_OR_LATER
	void HandlePackageSaved(const FString& InPackageFilename, UPackage* InPackage, FObjectPostSaveContext InObjectSaveContext);
#else
	void HandlePackageSaved(const FString& InPackageFilename, UObject* InOuter);
#endif

	/** Handle for the package-saved delegate, released on Close. */
	FDelegateHandle OnPackageSavedHandle;

	/** Helper function for Execute() */
	TSharedPtr<ILoreSourceControlWorker, ESPMode::ThreadSafe> CreateWorker(const FName& InOperationName) const;

	/** Helper function for running a command synchronously */
	ECommandResult::Type ExecuteSynchronousCommand(FLoreSourceControlCommand& InCommand, const FText& Task);

	/** Issue a command asynchronously if possible */
	ECommandResult::Type IssueCommand(FLoreSourceControlCommand& InCommand);

	/** Name the editor knows this provider by */
	FName ProviderName = FName("Lore");

	/** Is the Lore binary found and working */
	bool bLoreAvailable = false;

	/** Is a Lore repository found at or above the project */
	bool bLoreRepositoryFound = false;

	/** Path to Lore binary */
	FString PathToLoreBinary;

	/** Repository root (the project directory or any parent) */
	FString PathToRepositoryRoot;

	/** Remote the working tree pushes to */
	FString RemoteUrl;

	/** Commit identity (email) from working tree configuration */
	FString Identity;

	/** Name of current branch */
	FString BranchName;

	/** Repository identifier from the server */
	FString RepositoryId;

	/** Version string of the Lore client */
	FString LoreVersion;

	/** State cache, keyed by absolute filename */
	TMap<FString, TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe>> StateCache;

	/** The currently registered source control operations */
	TMap<FName, FGetLoreSourceControlWorker> WorkersMap;

	/** Queue for commands given by the main thread */
	TArray<FLoreSourceControlCommand*> CommandQueue;

	/** For notifying when the source control states in the cache have changed */
	FSourceControlStateChanged OnSourceControlStateChanged;
};
