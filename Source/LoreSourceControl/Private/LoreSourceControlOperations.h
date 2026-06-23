// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlState.h"
#include "SourceControlOperations.h"

/** Check in even when another user holds the Lore lock. */
class FLoreCheckInOverLock : public FCheckIn
{
public:
	virtual FName GetName() const override { return "CheckInOverLock"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_CheckInOverLock", "Checking in over an existing lock..."); }
};

class FLoreConnectWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	bool bAvailable = false;
	bool bRepositoryFound = false;
	FString RepositoryRoot;
	FString RemoteUrl;

	/** Commit identity (email) from working tree configuration */
	FString Identity;

	FString BranchName;
	FString RepositoryId;
	FString LoreVersion;
	bool bRemoteAvailable = false;
	bool bRemoteAuthorized = false;
};

class FLoreUpdateStatusWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;

	/** File history keyed by absolute filename, when an update asks for it */
	TMap<FString, TLoreSourceControlHistory> Histories;
};

/** Acquire a Lore lock and clear the read-only attribute. */
class FLoreCheckOutWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

class FLoreMarkForAddWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

/** Delete a file and stage the removal. */
class FLoreDeleteWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

/** Discard local changes, unstage, and release any held lock. */
class FLoreRevertWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

class FLoreSyncWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

/** Commit the submitted files and push the revision. */
class FLoreCheckInWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

/** Check in the submitted files over a foreign lock. */
class FLoreCheckInOverLockWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

class FLoreCopyWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};

class FLoreResolveWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
};
