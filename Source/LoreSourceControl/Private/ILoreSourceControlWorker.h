// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

class FLoreSourceControlCommand;

/** A unit of source control work. One worker per ISourceControlOperation. */
class ILoreSourceControlWorker
{
public:
	virtual ~ILoreSourceControlWorker() = default;

	/** Name of operation this worker handles, matching ISourceControlOperation::GetName. */
	virtual FName GetName() const = 0;

	/** Runs on a source control worker thread. */
	virtual bool Execute(FLoreSourceControlCommand& InCommand) = 0;

	/** Runs on the game thread to push results into the provider's state cache. */
	virtual bool UpdateStates() const = 0;
};

typedef TSharedRef<ILoreSourceControlWorker, ESPMode::ThreadSafe> FLoreSourceControlWorkerRef;
