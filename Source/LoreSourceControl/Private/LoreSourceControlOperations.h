// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlState.h"

/** Confirm the Lore client and find the working tree, then refresh the provider's repository information. */
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
};

/** Get the source control status of files, and their history when asked. */
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

/** Stage a new file for the next commit. */
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

/** Pull the latest revision from the server. */
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

/** Stage a copied or moved asset. */
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

/** Mark a conflicted file resolved. */
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
