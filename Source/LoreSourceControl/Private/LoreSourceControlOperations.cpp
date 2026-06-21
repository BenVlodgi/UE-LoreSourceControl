// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlOperations.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "SourceControlOperations.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	void RefreshStates(FLoreSourceControlCommand& InCommand, TArray<FLoreSourceControlState>& OutStates)
	{
		OutStates.Reset();
		LoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, InCommand.ErrorMessages, OutStates);
	}

	void SetFilesReadOnly(const TArray<FString>& InFiles, bool bReadOnly)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& File : InFiles)
		{
			if (PlatformFile.FileExists(*File))
			{
				PlatformFile.SetReadOnly(*File, bReadOnly);
			}
		}
	}

	// Only binary assets get locked; text files merge and stage without one.
	bool IsLockableAsset(const FString& InFile)
	{
		const FString Extension = FPaths::GetExtension(InFile);
		return Extension.Equals(TEXT("uasset"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("umap"), ESearchCase::IgnoreCase);
	}

	// Keep binary assets read-only unless user holds the lock; Lore doesn't set read-only itself.
	void ApplyReadOnlyFromStates(const TArray<FLoreSourceControlState>& InStates)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (const FLoreSourceControlState& State : InStates)
		{
			if (!IsLockableAsset(State.LocalFilename) || !PlatformFile.FileExists(*State.LocalFilename))
			{
				continue;
			}
			const bool bCommitted = State.WorkingCopyState == ELoreWorkingCopyState::Unchanged
				|| State.WorkingCopyState == ELoreWorkingCopyState::Modified
				|| State.WorkingCopyState == ELoreWorkingCopyState::Conflicted;
			const bool bReadOnly = bCommitted && State.LockState != ELoreLockState::Locked;
			PlatformFile.SetReadOnly(*State.LocalFilename, bReadOnly);
		}
	}
}

// Connect ---------------------------------------------------------------------

FName FLoreConnectWorker::GetName() const { return "Connect"; }

bool FLoreConnectWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	bAvailable = LoreSourceControlUtils::CheckLoreAvailability(InCommand.PathToLoreBinary, LoreVersion);
	bRepositoryFound = LoreSourceControlUtils::GetRepositoryRoot(FPaths::ProjectDir(), RepositoryRoot);
	if (bRepositoryFound)
	{
		LoreSourceControlUtils::GetRepositoryConfig(RepositoryRoot, RemoteUrl, Identity);
		LoreSourceControlUtils::GetBranchName(InCommand.PathToLoreBinary, RepositoryRoot, BranchName);
		LoreSourceControlUtils::GetRemoteAuthState(InCommand.PathToLoreBinary, RepositoryRoot, bRemoteAvailable, bRemoteAuthorized);
	}

	InCommand.bCommandSuccessful = bAvailable && bRepositoryFound;
	if (!InCommand.bCommandSuccessful)
	{
		const FText Error = !bAvailable
			? NSLOCTEXT("LoreSourceControl", "ClientMissing", "The Lore CLI could not be found. Set its path in the Lore settings.")
			: NSLOCTEXT("LoreSourceControl", "RepoMissing", "No Lore repository was found at or above the project directory.");
		InCommand.ErrorMessages.Add(Error.ToString());
		StaticCastSharedRef<FConnect>(InCommand.Operation)->SetErrorText(Error);
	}

	return InCommand.bCommandSuccessful;
}

bool FLoreConnectWorker::UpdateStates() const
{
	FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
	Provider.SetRepositoryInfo(bAvailable, bRepositoryFound, RepositoryRoot, RemoteUrl, Identity, BranchName, RepositoryId, LoreVersion);
	Provider.SetRemoteAuthState(bRemoteAvailable, bRemoteAuthorized);
	return false;
}

// UpdateStatus ----------------------------------------------------------------

FName FLoreUpdateStatusWorker::GetName() const { return "UpdateStatus"; }

bool FLoreUpdateStatusWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	if (InCommand.Files.Num() == 0)
	{
		InCommand.bCommandSuccessful = true;
		return true;
	}

	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, InCommand.ErrorMessages, States);

	// Re-assert read-only each status pass, since Lore never sets it itself.
	if (InCommand.bUsingLocking)
	{
		ApplyReadOnlyFromStates(States);
	}

	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);
	if (Operation->ShouldUpdateHistory())
	{
		for (FLoreSourceControlState& State : States)
		{
			TLoreSourceControlHistory History;
			LoreSourceControlUtils::RunGetHistory(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, State.LocalFilename, InCommand.ErrorMessages, History);
			State.History = History;
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FLoreUpdateStatusWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// CheckOut --------------------------------------------------------------------

FName FLoreCheckOutWorker::GetName() const { return "CheckOut"; }

bool FLoreCheckOutWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("lock acquire"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	if (InCommand.bCommandSuccessful)
	{
		// A lock does not change writability, so clear read-only ourselves.
		SetFilesReadOnly(InCommand.Files, false);
	}

	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreCheckOutWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// MarkForAdd ------------------------------------------------------------------

FName FLoreMarkForAddWorker::GetName() const { return "MarkForAdd"; }

bool FLoreMarkForAddWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreMarkForAddWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// Delete ----------------------------------------------------------------------

FName FLoreDeleteWorker::GetName() const { return "Delete"; }

bool FLoreDeleteWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// Delete on disk, then stage the removal.
	IFileManager& FileManager = IFileManager::Get();
	for (const FString& File : InCommand.Files)
	{
		if (FileManager.FileExists(*File))
		{
			FileManager.Delete(*File, false, true);
		}
	}

	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreDeleteWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// Revert ----------------------------------------------------------------------

FName FLoreRevertWorker::GetName() const { return "Revert"; }

bool FLoreRevertWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;

	// Unstage, then reset on-disk content to the committed revision.
	LoreSourceControlUtils::RunCommand(TEXT("unstage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);

	RefreshStates(InCommand, States);
	if (InCommand.bCommandSuccessful && InCommand.bUsingLocking)
	{
		// Release only locks this user holds; reverting an added file leaves an untracked path that was never locked, which release would error on.
		TArray<FString> LockedRelative;
		for (FLoreSourceControlState& State : States)
		{
			if (State.LockState == ELoreLockState::Locked)
			{
				LockedRelative.Add(LoreSourceControlUtils::RelativeFilename(State.LocalFilename, InCommand.PathToRepositoryRoot));
				State.LockState = ELoreLockState::NotLocked;
			}
		}
		if (LockedRelative.Num() > 0)
		{
			LoreSourceControlUtils::RunCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), LockedRelative, Results, InCommand.ErrorMessages);
		}
		ApplyReadOnlyFromStates(States);
	}
	return InCommand.bCommandSuccessful;
}

bool FLoreRevertWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// Sync ------------------------------------------------------------------------

FName FLoreSyncWorker::GetName() const { return "Sync"; }

bool FLoreSyncWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	TArray<FString> Results;
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("sync"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, InCommand.ErrorMessages);
	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreSyncWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// CheckIn ---------------------------------------------------------------------

/**
 * Shared check-in body for the normal and force-over-lock submit paths.
 *
 * @param InCommand       Command carrying the files, identity, and settings to submit.
 * @param States          Receives the file states refreshed after the submit.
 * @param bForceOverLock  When true, the submit proceeds over a foreign lock and does not release a lock this user never held.
 * @return                True when the commit, and any push, succeeded.
 */
static bool RunCheckIn(FLoreSourceControlCommand& InCommand, TArray<FLoreSourceControlState>& States, bool bForceOverLock)
{
	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;

	// Guard on a fresh status: refuse before staging if a submitted file is locked by another (unless forcing over it) or conflicted. A behind-the-remote file is allowed; its commit is local and a rejected push is reported.
	TArray<FLoreSourceControlState> GuardStates;
	TArray<FString> GuardErrors;
	LoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, GuardErrors, GuardStates);
	if (LoreSourceControlUtils::CollectCheckInBlockers(GuardStates, bForceOverLock, InCommand.ErrorMessages))
	{
		InCommand.bCommandSuccessful = false;
		RefreshStates(InCommand, States);
		return false;
	}

	// Lore commits the whole staged set, so unstage everything, then stage only our files. Unstage errors are cleanup, keep them out of the user-facing list.
	TArray<FString> UnstageErrors;
	LoreSourceControlUtils::RunCommand(TEXT("unstage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, { TEXT(".") }, TArray<FString>(), Results, UnstageErrors);

	if (!LoreSourceControlUtils::RunCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages))
	{
		InCommand.bCommandSuccessful = false;
		RefreshStates(InCommand, States);
		return false;
	}

	// Commit only records when a staged file actually changed; otherwise nothing to submit.
	TArray<FLoreSourceControlState> SelectedStates;
	TArray<FString> SelectedStatusErrors;
	LoreSourceControlUtils::RunUpdateStatus(InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, SelectedStatusErrors, SelectedStates);
	const bool bHasChangeToSubmit = SelectedStates.ContainsByPredicate([](const FLoreSourceControlState& State)
	{
		return State.WorkingCopyState == ELoreWorkingCopyState::Added
			|| State.WorkingCopyState == ELoreWorkingCopyState::Modified
			|| State.WorkingCopyState == ELoreWorkingCopyState::Deleted
			|| State.WorkingCopyState == ELoreWorkingCopyState::Moved
			|| State.WorkingCopyState == ELoreWorkingCopyState::Copied;
	});
	if (!bHasChangeToSubmit)
	{
		StaticCastSharedRef<FCheckIn>(InCommand.Operation)->SetSuccessMessage(NSLOCTEXT("LoreSourceControl", "CheckInNoChanges", "No changes to submit."));
		InCommand.bCommandSuccessful = true;
		RefreshStates(InCommand, States);
		return true;
	}

	const FString Description = StaticCastSharedRef<FCheckIn>(InCommand.Operation)->GetDescription().ToString();
	FString Message = Description;
	Message.ReplaceInline(TEXT("\""), TEXT("'"));
	Message.ReplaceInline(TEXT("\r"), TEXT(" "));
	Message.ReplaceInline(TEXT("\n"), TEXT(" "));

	TArray<FString> CommitParameters;
	CommitParameters.Add(FString::Printf(TEXT("\"%s\""), *Message));
	// Lore will not commit without an identity, so pass the configured one.
	if (!InCommand.Identity.IsEmpty())
	{
		CommitParameters.Add(TEXT("--identity"));
		CommitParameters.Add(FString::Printf(TEXT("\"%s\""), *InCommand.Identity));
	}
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("commit"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, CommitParameters, TArray<FString>(), Results, InCommand.ErrorMessages);

	if (InCommand.bCommandSuccessful && InCommand.bPushAfterCommit)
	{
		const bool bPushed = LoreSourceControlUtils::RunCommand(TEXT("push"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, InCommand.ErrorMessages);
		if (!bPushed)
		{
			InCommand.ErrorMessages.Add(TEXT("Commit succeeded locally but push failed. Sync, resolve, and submit again."));
			InCommand.bCommandSuccessful = false;
		}
	}

	// The editor relies on the provider for Keep Files Checked Out, so release the lock only when the option is off.
#if LORE_UE5_1_OR_LATER
	const bool bKeepCheckedOut = StaticCastSharedRef<FCheckIn>(InCommand.Operation)->GetKeepCheckedOut();
#else
	// FCheckIn gained the flag in 5.1; earlier editors always release on submit.
	const bool bKeepCheckedOut = false;
#endif
	// Do not release a lock held by someone else when forcing over it; we never held it.
	if (InCommand.bCommandSuccessful && InCommand.bUsingLocking && !bKeepCheckedOut && !bForceOverLock)
	{
		LoreSourceControlUtils::RunCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	}

	if (InCommand.bCommandSuccessful)
	{
		TArray<FString> Names;
		for (const FString& RelativeFile : RelativeFiles)
		{
			Names.Add(FPaths::GetCleanFilename(RelativeFile));
		}
		const FText SuccessMessage = FText::Format(
			InCommand.bPushAfterCommit
				? NSLOCTEXT("LoreSourceControl", "CheckInPushed", "Submitted {0} {0}|plural(one=file,other=files) to Lore: \"{1}\" ({2})")
				: NSLOCTEXT("LoreSourceControl", "CheckInCommitted", "Committed {0} {0}|plural(one=file,other=files) locally: \"{1}\" ({2})"),
			FText::AsNumber(RelativeFiles.Num()), FText::FromString(Description), FText::FromString(FString::Join(Names, TEXT(", "))));
		StaticCastSharedRef<FCheckIn>(InCommand.Operation)->SetSuccessMessage(SuccessMessage);
	}

	RefreshStates(InCommand, States);
	if (InCommand.bCommandSuccessful && InCommand.bUsingLocking)
	{
		ApplyReadOnlyFromStates(States);
	}
	return InCommand.bCommandSuccessful;
}

FName FLoreCheckInWorker::GetName() const { return "CheckIn"; }

bool FLoreCheckInWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	return RunCheckIn(InCommand, States, /*bForceOverLock*/ false);
}

bool FLoreCheckInWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// CheckInOverLock -------------------------------------------------------------

FName FLoreCheckInOverLockWorker::GetName() const { return "CheckInOverLock"; }

bool FLoreCheckInOverLockWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	return RunCheckIn(InCommand, States, /*bForceOverLock*/ true);
}

bool FLoreCheckInOverLockWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// Copy ------------------------------------------------------------------------

FName FLoreCopyWorker::GetName() const { return "Copy"; }

bool FLoreCopyWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const FString Destination = StaticCastSharedRef<FCopy>(InCommand.Operation)->GetDestination();
	const FString RelativeDestination = LoreSourceControlUtils::RelativeFilename(Destination, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;
	TArray<FString> Targets;
	Targets.Add(RelativeDestination);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), Targets, Results, InCommand.ErrorMessages);

	InCommand.Files.Add(Destination);
	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreCopyWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// Resolve ---------------------------------------------------------------------

FName FLoreResolveWorker::GetName() const { return "Resolve"; }

bool FLoreResolveWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunCommand(TEXT("stage"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreResolveWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}
