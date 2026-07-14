// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ILoreSourceControlWorker.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlVersion.h"
#include "SourceControlOperations.h"

/** Check in even when another user holds the Lore lock. */
class FLoreCheckInOverLock : public FCheckIn
{
public:
	virtual FName GetName() const override { return "CheckInOverLock"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_CheckInOverLock", "Checking in over an existing lock..."); }
};

// The read operations that feed the branches window and the history browser. Each is a custom FSourceControlOperationBase carrying parameters in and result rows out, the pattern the Unity Version Control provider uses for its branch and changeset operations; the worker fills the operation object and leaves the state cache untouched. Ungated: FSourceControlOperationBase exists at 4.27, and an operation nobody reaches from UI is simply never executed outside tests.

/** List the repository's branches as merged rows for the branches window. */
class FLoreGetBranches : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "GetBranches"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_GetBranches", "Listing branches..."); }

	/** Include archived local branches, rendered local-only with a blank latest. */
	bool bIncludeArchived = false;

	/** Result rows, one per branch, filled by the worker and read by the widget on completion. */
	TArray<FLoreBranchItem> Branches;
};

/** List one page of a branch's revisions for the history browser. */
class FLoreGetRevisions : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "GetRevisions"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_GetRevisions", "Reading revisions..."); }

	/** Branch to read; empty reads the current branch. */
	FString Branch;

	/** Start revision of this page, inclusive; empty starts at the branch tip. The next page starts at the oldest loaded entry's parent hash. */
	FString StartRevision;

	/** Page length; zero returns the whole history. */
	int32 PageLength = 0;

	/** Result rows, newest first, each carrying its inline metadata. */
	TArray<FLoreRevisionListEntry> Revisions;
};

/** Read the per-file delta of one revision for the history browser's files pane. */
class FLoreGetRevisionFiles : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "GetRevisionFiles"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_GetRevisionFiles", "Reading revision files..."); }

	/** Revision to read; empty reads the current revision. */
	FString Revision;

	/** The revision's metadata and full delta; the files pane filters Delta on bIsFile. */
	FLoreRevisionDetails Details;
};

// The guarded branch write operations that back the branches window's context menu. Each carries the branch it acts on and, for the file-rewriting ones, the delta paths the game-thread reload uses. Ungated: FSourceControlOperationBase exists at 4.27, so the workers, their preconditions, and their recovery arm compile and test headlessly on every engine; only the widget entries that dispatch them stay behind the 5.1 UI gate.

/** Switch the working tree to another branch, bracketed by the crash-protection guard. */
class FLoreSwitchBranch : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "SwitchBranch"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_SwitchBranch", "Switching the working tree to a branch..."); }

	/** Branch to switch to. */
	FString BranchName;

	/** Delta paths the switch rewrote, repository-relative; the widget reloads these packages after the switch. */
	TArray<FString> UpdatedFiles;
};

/** Create a branch at the current revision and switch to it, bracketed by the guard only when a delta exists. */
class FLoreCreateBranch : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "CreateBranch"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_CreateBranch", "Creating a branch..."); }

	/** Name of the branch to create. */
	FString BranchName;

	/** Delta paths, empty when creating at the current revision (the usual case, which rewrites no files). */
	TArray<FString> UpdatedFiles;
};

/** Archive a branch. No unarchive in 0.8.3. */
class FLoreArchiveBranch : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "ArchiveBranch"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_ArchiveBranch", "Archiving a branch..."); }

	/** Branch to archive. */
	FString BranchName;
};

/** Set a branch's protect flag; enforcement is server-side, so this is advisory. */
class FLoreProtectBranch : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "ProtectBranch"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_ProtectBranch", "Protecting a branch..."); }

	/** Branch to protect. */
	FString BranchName;
};

/** Clear a branch's protect flag; enforcement is server-side, so this is advisory. */
class FLoreUnprotectBranch : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "UnprotectBranch"; }
	virtual FText GetInProgressString() const override { return NSLOCTEXT("LoreSourceControl", "Lore_UnprotectBranch", "Unprotecting a branch..."); }

	/** Branch to unprotect. */
	FString BranchName;
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

	/** A guarded rewrite that failed left its marker, so the game-thread completion surfaces the recovery offer without waiting for a restart */
	bool bRewriteFailed = false;
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

// The read workers fill their operation object and leave the state cache untouched, so UpdateStates returns false. They read local revision and branch data, so they work offline.

/** Enumerate merged branch rows with the protect key onto the FLoreGetBranches operation. */
class FLoreGetBranchesWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface
};

/** Read one page of revisions onto the FLoreGetRevisions operation. */
class FLoreGetRevisionsWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface
};

/** Read one revision's per-file delta onto the FLoreGetRevisionFiles operation. */
class FLoreGetRevisionFilesWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface
};

// The guarded branch write workers. Each precondition (unstage, enumerate, guard) runs on the pool thread in Execute; the game-thread UpdateStates re-runs the lock query (a switch invalidates every cached lock state, locks being branch-scoped) and clears the deferred marker. The package reload is the widget's own completion-callback concern, so the workers stay headlessly testable.

/** The guarded switch: unstage, enumerate the delta, run the rewrite behind the guard, then re-query locks and clear the marker on the game thread. */
class FLoreSwitchBranchWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
	FString RepositoryRoot;
	FString TargetBranch;

	/** Whether the switch rewrote the tree, so the game thread clears the deferred marker and re-queries locks. */
	bool bSwitched = false;

	/** A guarded rewrite that failed left its marker, so the game-thread completion surfaces the recovery offer without waiting for a restart. */
	bool bRewriteFailed = false;
};

/** Create a branch at the current revision and switch to it. Guarded as a rewrite only when a delta exists; creating at the current revision has an empty delta and rewrites no files. */
class FLoreCreateBranchWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	TArray<FLoreSourceControlState> States;
	FString RepositoryRoot;
	bool bCreated = false;
	bool bRewriteFailed = false;
};

/** Archive the named branch. */
class FLoreArchiveBranchWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface
};

/** Set the named branch's protect flag; advisory, since enforcement is server-side. */
class FLoreProtectBranchWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface
};

/** Clear the named branch's protect flag; advisory, since enforcement is server-side. */
class FLoreUnprotectBranchWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface
};

// The changelist workers edit the plugin-owned changelist store, never the Lore server; Lore has no native changelist. Gated with the changelist surface to 5.3.
#if LORE_UE5_3_OR_LATER

/** Create a changelist record and hand its identifier back on the operation. */
class FLoreNewChangelistWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	/** Whether the store was edited, so the provider broadcasts a refresh */
	bool bStoreEdited = false;
};

/** Remove a changelist record; the Default changelist is refused. */
class FLoreDeleteChangelistWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	/** Whether the store was edited, so the provider broadcasts a refresh */
	bool bStoreEdited = false;
};

/** Rewrite a changelist record's description. */
class FLoreEditChangelistWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	/** Whether the store was edited, so the provider broadcasts a refresh */
	bool bStoreEdited = false;
};

/** Move the command's files into the target changelist record. */
class FLoreMoveToChangelistWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface

	/** Whether the store was edited, so the provider broadcasts a refresh */
	bool bStoreEdited = false;
};

/** Refresh the pending changelists; a no-op since GetChangelists and the changelist GetState read the store live. */
class FLoreUpdateChangelistsStatusWorker : public ILoreSourceControlWorker
{
public:
	//~ Begin ILoreSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(FLoreSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;
	//~ End ILoreSourceControlWorker interface
};

#endif // LORE_UE5_3_OR_LATER
