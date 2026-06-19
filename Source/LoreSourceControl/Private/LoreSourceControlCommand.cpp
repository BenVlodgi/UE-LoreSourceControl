// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlCommand.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"

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
	// The settings identity, when set, overrides the working tree configuration.
	Identity = Module.GetSettings().GetIdentity();
	if (Identity.IsEmpty())
	{
		Identity = Provider.GetIdentity();
	}
	bUsingLocking = Module.GetSettings().IsUsingLocking();
	bPushAfterCommit = Module.GetSettings().IsPushAfterCommit();
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
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);
	return Result;
}
