// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"
#include "Misc/IQueuedWork.h"
#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"
#if LORE_UE5_0_OR_LATER
#include "ISourceControlChangelist.h"
#endif
#include "ILoreSourceControlWorker.h"

/** Used to run Lore commands on a worker thread. */
class FLoreSourceControlCommand : public IQueuedWork
{
public:

	FLoreSourceControlCommand(const FSourceControlOperationRef& InOperation, const FLoreSourceControlWorkerRef& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete());

	/** Run the worker and return whether it succeeded. */
	bool DoWork();

	//~ Begin IQueuedWork interface
	virtual void DoThreadedWork() override;
	virtual void Abandon() override;
	//~ End IQueuedWork interface

	/** Ask a running command to cancel at the next opportunity. */
	void Cancel();
	bool IsCanceled() const;

	/** Save any results and call any registered callback. Game thread only. */
	ECommandResult::Type ReturnResults();

public:
	FString PathToLoreBinary;

	/** Repository root, found at connect */
	FString PathToRepositoryRoot;

	/** Commit identity (email) from working tree configuration */
	FString Identity;

	/** Check out takes a lock; binary assets read-only until checked out */
	bool bUsingLocking = false;

	/** Check in pushes the revision to the server */
	bool bPushAfterCommit = true;

	FSourceControlOperationRef Operation;
	FLoreSourceControlWorkerRef Worker;
	FSourceControlOperationComplete OperationCompleteDelegate;

	/** Set once Execute has run */
	FThreadSafeCounter bExecuteProcessed;

	/** Raised to request cancel */
	FThreadSafeCounter bCancelledCounter;

	bool bCommandSuccessful = false;

	/** Worker thread or inline */
	EConcurrency::Type Concurrency = EConcurrency::Synchronous;

	/** Tick deletes this command when it finishes */
	bool bAutoDelete = false;

	TArray<FString> Files;
#if LORE_UE5_0_OR_LATER
	/** Target changelist, if any */
	FSourceControlChangelistPtr Changelist;
#endif

	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
};
