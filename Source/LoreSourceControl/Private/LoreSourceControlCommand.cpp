// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlCommand.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlLog.h"

FLoreSourceControlCommand::FLoreSourceControlCommand(const FSourceControlOperationRef& InOperation, const FLoreSourceControlWorkerRef& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate)
	: Operation(InOperation)
	, Worker(InWorker)
	, OperationCompleteDelegate(InOperationCompleteDelegate)
{
	// Snapshot provider and settings here, so the worker thread never reads them.
	check(IsInGameThread());
	const FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
	const FLoreSourceControlProvider& Provider = Module.GetProvider();
	PathToLoreBinary = Provider.GetLoreBinaryPath();
	PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
	// Settings identity, when set, overrides the working tree configuration.
	Identity = Module.GetSettings().GetIdentity();
	if (Identity.IsEmpty())
	{
		Identity = Provider.GetIdentity();
	}
	bUsingLocking = Module.GetSettings().IsUsingLocking();
	bPushAfterCommit = Module.GetSettings().IsPushAfterCommit();
	BackendMode = Module.GetSettings().GetBackendMode();
	bRemoteAvailable = Provider.IsRemoteAvailable();
}

bool FLoreSourceControlCommand::DoWork()
{
	bCommandSuccessful = Worker->Execute(*this);
	bExecuteProcessed.Increment();
	return bCommandSuccessful;
}

void FLoreSourceControlCommand::DoThreadedWork()
{
	Concurrency = EConcurrency::Asynchronous;
	DoWork();
}

void FLoreSourceControlCommand::Abandon()
{
	bCommandSuccessful = false;
	bExecuteProcessed.Increment();
}

void FLoreSourceControlCommand::Cancel()
{
	bCancelledCounter.Increment();
}

bool FLoreSourceControlCommand::IsCanceled() const
{
	return bCancelledCounter.GetValue() > 0;
}

ECommandResult::Type FLoreSourceControlCommand::ReturnResults()
{
	for (const FString& Message : InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(Message));
	}
	for (const FString& Message : ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(Message));
	}

	ECommandResult::Type Result = bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed;
	if (IsCanceled())
	{
		Result = ECommandResult::Cancelled;
	}

	// One outcome line per operation so the log shows what ran; failures carry their reasons, since the editor only surfaces a generic dialog.
	if (Result == ECommandResult::Failed)
	{
		UE_LOG(LogLoreSourceControl, Warning, TEXT("%s failed: %s"), *Operation->GetName().ToString(), *FString::Join(ErrorMessages, TEXT(" | ")));
	}
	else
	{
		UE_LOG(LogLoreSourceControl, Log, TEXT("%s %s"), *Operation->GetName().ToString(), Result == ECommandResult::Cancelled ? TEXT("cancelled") : TEXT("succeeded"));
	}

	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);
	return Result;
}
