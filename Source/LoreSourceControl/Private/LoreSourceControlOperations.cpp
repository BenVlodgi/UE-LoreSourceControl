// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlOperations.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "LoreGuardedRewrite.h"
#include "LoreChangelistStore.h"
#include "LoreSourceControlChangelist.h"
#include "SourceControlOperations.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	void RefreshStates(FLoreSourceControlCommand& InCommand, TArray<FLoreSourceControlState>& OutStates)
	{
		OutStates.Reset();
		LoreSourceControlUtils::RunUpdateStatus(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, InCommand.ErrorMessages, OutStates, InCommand.bRemoteAvailable);
	}

	// The read-only policy helpers live in the utilities now, shared with the guarded rewrite helper; alias them for the terse call sites here.
	using LoreSourceControlUtils::SetFilesReadOnly;
	using LoreSourceControlUtils::ApplyReadOnlyFromStates;
}

// Connect ---------------------------------------------------------------------

FName FLoreConnectWorker::GetName() const { return "Connect"; }

bool FLoreConnectWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	bAvailable = LoreSourceControlUtils::CheckLoreAvailability(InCommand.BackendMode, InCommand.PathToLoreBinary, LoreVersion);
	bRepositoryFound = LoreSourceControlUtils::GetRepositoryRoot(FPaths::ProjectDir(), RepositoryRoot);
	if (bRepositoryFound)
	{
		LoreSourceControlUtils::GetRepositoryConfig(RepositoryRoot, RemoteUrl, Identity);
		LoreSourceControlUtils::GetBranchName(InCommand.BackendMode, InCommand.PathToLoreBinary, RepositoryRoot, BranchName);
		LoreSourceControlUtils::GetRemoteAuthState(InCommand.BackendMode, InCommand.PathToLoreBinary, RepositoryRoot, bRemoteAvailable, bRemoteAuthorized);
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
	// A connect that finds a repository Init did not have is the other place a marker from a crash surfaces.
	if (bRepositoryFound)
	{
		Provider.DetectInterruptedRewrite();
	}
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

	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunUpdateStatus(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, InCommand.ErrorMessages, States, InCommand.bRemoteAvailable);

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
			LoreSourceControlUtils::RunGetHistory(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, State.LocalFilename, InCommand.ErrorMessages, History, State.bRemoteAvailable);
			State.History = History;
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FLoreUpdateStatusWorker::UpdateStates() const
{
	// A status pass reads reachability fresh, so feed it back to the provider. Only connect set reachability before, so this is how a mid-session server drop or recovery reaches later commands and grays out or restores check out.
	if (States.Num() > 0)
	{
		FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
		Provider.SetRemoteAuthState(States[0].bRemoteAvailable, Provider.IsRemoteAuthorized());
	}
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// CheckOut --------------------------------------------------------------------

FName FLoreCheckOutWorker::GetName() const { return "CheckOut"; }

bool FLoreCheckOutWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// A lock cannot be acquired offline, and the acquire call blocks for minutes against an unreachable server. Make the files writable so editing continues, leaving them unlocked, rather than hang on a lock that cannot be taken.
	if (!InCommand.bRemoteAvailable)
	{
		SetFilesReadOnly(InCommand.Files, false);
		InCommand.InfoMessages.Add(TEXT("Offline: made writable without acquiring a lock."));
		InCommand.bCommandSuccessful = true;
		RefreshStates(InCommand, States);
		return InCommand.bCommandSuccessful;
	}

	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::AcquireLocks(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, RelativeFiles, InCommand.ErrorMessages);
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
	InCommand.bCommandSuccessful = LoreSourceControlUtils::StageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, RelativeFiles, InCommand.ErrorMessages);
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
	// Stage records a removal only once the file is gone on disk, so delete first.
	IFileManager& FileManager = IFileManager::Get();
	for (const FString& File : InCommand.Files)
	{
		if (FileManager.FileExists(*File))
		{
			FileManager.Delete(*File, false, true);
		}
	}

	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::StageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, RelativeFiles, InCommand.ErrorMessages);
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

	// Unstage, then reset on-disk content to the committed revision. Unstage failures are cleanup (an unstaged path has nothing to unstage), so keep them out of the user-facing list.
	TArray<FString> UnstageCleanupErrors;
	LoreSourceControlUtils::UnstageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, RelativeFiles, UnstageCleanupErrors);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::ResetFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, RelativeFiles, InCommand.ErrorMessages);

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
			LoreSourceControlUtils::ReleaseLocks(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, LockedRelative, InCommand.ErrorMessages);
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
	// Enumerate the delta first: the guard cannot protect paths it cannot name, and against an unreachable server this fails fast where the unguarded sync would stall. A committed binary asset this user has not locked is read-only on disk, and Lore's rewrite machinery aborts on a read-only file inside the delta, so the sync runs behind the guard that clears read-only on the delta paths.
	TArray<FLoreDeltaEntry> Delta;
	if (!LoreSourceControlUtils::EnumerateSyncDelta(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Delta, InCommand.ErrorMessages))
	{
		InCommand.bCommandSuccessful = false;
		RefreshStates(InCommand, States);
		return false;
	}

	// The marker records the branch and the anchor a recovery rolls back to; the header already carries both.
	FLoreStatusAnchors Anchors;
	TArray<FString> AnchorErrors;
	LoreSourceControlUtils::GetStatusRevisionAnchors(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Anchors, AnchorErrors);

	LoreGuardedRewrite::FRewriteRequest Request;
	Request.Kind = ELoreOperationKind::RevisionSync;
	Request.SourceBranch = Anchors.BranchName;
	Request.SourceRevision = Anchors.CurrentRevision;
	Request.Delta = MoveTemp(Delta);

	InCommand.bCommandSuccessful = LoreGuardedRewrite::Run(Request, InCommand, [&InCommand](TArray<FString>& OutErrorMessages)
	{
		return LoreSourceControlUtils::SyncWorkingTree(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, OutErrorMessages);
	}, States);
	bRewriteFailed = !InCommand.bCommandSuccessful;
	return InCommand.bCommandSuccessful;
}

bool FLoreSyncWorker::UpdateStates() const
{
	const bool bChanged = LoreSourceControlUtils::UpdateCachedStates(States);
	// A failed guarded sync retained its marker, the crash-that-does-not-restart case, so surface the recovery offer now rather than waiting for the next connect.
	if (bRewriteFailed)
	{
		FLoreSourceControlModule::Get().GetProvider().DetectInterruptedRewrite();
	}
	return bChanged;
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
#if LORE_UE5_3_OR_LATER
	// A changelist-keyed submit was expanded to its member files at issue time on the game thread, so the committed set is exactly what the Changes view rendered. A changelist that expanded to nothing refuses rather than committing the whole staged set.
	if (InCommand.Files.Num() == 0 && !InCommand.ChangelistId.IsEmpty())
	{
		InCommand.ErrorMessages.Add(NSLOCTEXT("LoreSourceControl", "CheckInEmptyChangelist", "The changelist has no files to submit.").ToString());
		InCommand.bCommandSuccessful = false;
		return false;
	}
#endif

	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);

	// Guard on a fresh status: refuse before staging if a submitted file is locked by another (unless forcing over it) or conflicted. A behind-the-remote file is allowed; its commit is local and a rejected push is reported.
	TArray<FLoreSourceControlState> GuardStates;
	TArray<FString> GuardErrors;
	LoreSourceControlUtils::RunUpdateStatus(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, GuardErrors, GuardStates, InCommand.bRemoteAvailable);
	if (LoreSourceControlUtils::CollectCheckInBlockers(GuardStates, bForceOverLock, InCommand.ErrorMessages))
	{
		InCommand.bCommandSuccessful = false;
		RefreshStates(InCommand, States);
		return false;
	}

	// Lore commits the whole staged set, so unstage everything, then stage only our files. Unstage errors are cleanup, keep them out of the user-facing list.
	TArray<FString> UnstageErrors;
	LoreSourceControlUtils::UnstageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, { TEXT(".") }, UnstageErrors);

	if (!LoreSourceControlUtils::StageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, RelativeFiles, InCommand.ErrorMessages))
	{
		InCommand.bCommandSuccessful = false;
		RefreshStates(InCommand, States);
		return false;
	}

	// Commit only records when a staged file actually changed; otherwise nothing to submit.
	TArray<FLoreSourceControlState> SelectedStates;
	TArray<FString> SelectedStatusErrors;
	LoreSourceControlUtils::RunUpdateStatus(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, InCommand.bUsingLocking, InCommand.Files, SelectedStatusErrors, SelectedStates, InCommand.bRemoteAvailable);
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
		StaticCastSharedRef<FCheckIn>(InCommand.Operation)->SetSuccessMessage(NSLOCTEXT("LoreSourceControl", "CheckInNoChanges", "Nothing to commit: the selected files have no changes, so no revision was created. They stay checked out; revert to release the lock."));
		InCommand.bCommandSuccessful = true;
		RefreshStates(InCommand, States);
		return true;
	}

	const FString Message = StaticCastSharedRef<FCheckIn>(InCommand.Operation)->GetDescription().ToString();

	// Lore will not commit without an identity, so the configured one is passed through. CommitChanges escapes the message only for the CLI path; the native path records it verbatim.
	InCommand.bCommandSuccessful = LoreSourceControlUtils::CommitChanges(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.Identity, Message, InCommand.ErrorMessages);

	if (InCommand.bCommandSuccessful && InCommand.bPushAfterCommit)
	{
		const bool bPushed = LoreSourceControlUtils::PushBranch(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, InCommand.ErrorMessages);
		if (!bPushed)
		{
			InCommand.ErrorMessages.Add(NSLOCTEXT("LoreSourceControl", "PushFailedAfterCommit", "Commit succeeded locally but push failed. Sync, resolve, and submit again.").ToString());
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
			FText::AsNumber(RelativeFiles.Num()), FText::FromString(Message), FText::FromString(FString::Join(Names, TEXT(", "))));
		StaticCastSharedRef<FCheckIn>(InCommand.Operation)->SetSuccessMessage(SuccessMessage);
	}

#if LORE_UE5_3_OR_LATER
	// Drop the submitted files from their changelist record. A named record emptied by its submit goes away, the Plastic provider's implicit delete on submit; the Default record only empties.
	if (InCommand.bCommandSuccessful && !InCommand.ChangelistId.IsEmpty())
	{
		LoreChangelists::Mutate(InCommand.PathToRepositoryRoot, [&InCommand, &RelativeFiles](TArray<FLoreChangelistRecord>& Records)
		{
			FLoreChangelistRecord* Record = Records.FindByPredicate([&InCommand](const FLoreChangelistRecord& R) { return R.Id == InCommand.ChangelistId; });
			if (!Record)
			{
				return false;
			}
			Record->Files.RemoveAll([&RelativeFiles](const FString& File) { return RelativeFiles.Contains(File); });
			if (!Record->IsDefault() && Record->Files.Num() == 0)
			{
				Records.RemoveAll([&InCommand](const FLoreChangelistRecord& R) { return R.Id == InCommand.ChangelistId; });
			}
			return true;
		});
	}
#endif

	RefreshStates(InCommand, States);
	if (InCommand.bCommandSuccessful && InCommand.bUsingLocking)
	{
		// Release after refreshing, then assert NotLocked rather than trusting the re-read: against an auth-off server a just-released lock reads back as still held, which would leave the file checked out and its read-only bit wrong on the next edit. Mirrors the revert worker. Keep Files Checked Out and a forced foreign lock skip the release.
		if (!bKeepCheckedOut && !bForceOverLock)
		{
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
				LoreSourceControlUtils::ReleaseLocks(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, LockedRelative, InCommand.ErrorMessages);
			}
		}
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
	TArray<FString> Targets;
	Targets.Add(RelativeDestination);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::StageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Targets, InCommand.ErrorMessages);

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
	InCommand.bCommandSuccessful = LoreSourceControlUtils::StageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, RelativeFiles, InCommand.ErrorMessages);
	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreResolveWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// Read operations for the branches window and the history browser ---------------
// Each fills its operation object and touches no state cache, so UpdateStates returns false.

FName FLoreGetBranchesWorker::GetName() const { return "GetBranches"; }

bool FLoreGetBranchesWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreGetBranches, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreGetBranches>(InCommand.Operation);
	Operation->Branches.Reset();
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunGetBranches(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Operation->bIncludeArchived, Operation->Branches, InCommand.ErrorMessages);
	return InCommand.bCommandSuccessful;
}

bool FLoreGetBranchesWorker::UpdateStates() const
{
	return false;
}

FName FLoreGetRevisionsWorker::GetName() const { return "GetRevisions"; }

bool FLoreGetRevisionsWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreGetRevisions, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreGetRevisions>(InCommand.Operation);
	Operation->Revisions.Reset();
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunRevisionList(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Operation->Branch, Operation->StartRevision, Operation->PageLength, Operation->Revisions, InCommand.ErrorMessages);
	return InCommand.bCommandSuccessful;
}

bool FLoreGetRevisionsWorker::UpdateStates() const
{
	return false;
}

FName FLoreGetRevisionFilesWorker::GetName() const { return "GetRevisionFiles"; }

bool FLoreGetRevisionFilesWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreGetRevisionFiles, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreGetRevisionFiles>(InCommand.Operation);
	Operation->Details = FLoreRevisionDetails();
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunRevisionInfo(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Operation->Revision, true, Operation->Details, InCommand.ErrorMessages);
	return InCommand.bCommandSuccessful;
}

bool FLoreGetRevisionFilesWorker::UpdateStates() const
{
	return false;
}

// Guarded branch write operations -----------------------------------------------

FName FLoreSwitchBranchWorker::GetName() const { return "SwitchBranch"; }

bool FLoreSwitchBranchWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreSwitchBranch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreSwitchBranch>(InCommand.Operation);
	TargetBranch = Operation->BranchName;
	RepositoryRoot = InCommand.PathToRepositoryRoot;

	// Unstage first: a staged state refuses the switch outright before any file work, and both refusals (staged, or a dirty file in the delta) leave the tree untouched. Unstage errors are cleanup, so keep them out of the user-facing list.
	TArray<FString> UnstageErrors;
	LoreSourceControlUtils::UnstageFiles(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, { TEXT(".") }, UnstageErrors);

	// Enumerate the delta with a dry-run switch: the guard cannot protect paths it cannot name, and a read-only file inside the delta aborts the switch midway. An enumeration failure refuses the switch before any mutation.
	TArray<FLoreDeltaEntry> Delta;
	if (!LoreSourceControlUtils::EnumerateSwitchDelta(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TargetBranch, FString(), Delta, InCommand.ErrorMessages))
	{
		InCommand.bCommandSuccessful = false;
		RefreshStates(InCommand, States);
		return false;
	}

	// The marker records the source branch and anchor a recovery rolls back to, and the target branch a recovery re-dispatches the switch to.
	FLoreStatusAnchors Anchors;
	TArray<FString> AnchorErrors;
	LoreSourceControlUtils::GetStatusRevisionAnchors(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Anchors, AnchorErrors);

	for (const FLoreDeltaEntry& Entry : Delta)
	{
		Operation->UpdatedFiles.Add(Entry.Path);
	}

	LoreGuardedRewrite::FRewriteRequest Request;
	Request.Kind = ELoreOperationKind::BranchSwitch;
	Request.SourceBranch = Anchors.BranchName;
	Request.TargetBranch = TargetBranch;
	Request.SourceRevision = Anchors.CurrentRevision;
	Request.Delta = MoveTemp(Delta);
	// The switch clears its marker on the game thread after the package reload, so the guard must not clear it on success.
	Request.bClearMarkerOnSuccess = false;

	InCommand.bCommandSuccessful = LoreGuardedRewrite::Run(Request, InCommand, [&InCommand, this](TArray<FString>& OutErrorMessages)
	{
		// Never -f, reset, or bare: a conflicting dirty file must refuse the switch, not be overwritten.
		return LoreSourceControlUtils::RunBranchSwitch(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TargetBranch, FString(), OutErrorMessages);
	}, States);

	bSwitched = InCommand.bCommandSuccessful;
	bRewriteFailed = !InCommand.bCommandSuccessful;

	// Live recompile hook: hand the delta to the recompile trigger once it exists. Seam only; the recompile is not built here.

	return InCommand.bCommandSuccessful;
}

bool FLoreSwitchBranchWorker::UpdateStates() const
{
	const bool bChanged = LoreSourceControlUtils::UpdateCachedStates(States);

	if (bSwitched)
	{
		// Locks are branch-scoped, so every cached lock state is invalid after a switch. The guard's verify pass re-read lock ownership for the delta files on the new branch; the editor's ongoing status polling corrects the rest. Clear the deferred marker now that the switch verified consistent; the widget reloads the affected packages next, and a death during that reload does not tear the tree.
		LoreOperationMarker::Clear(RepositoryRoot);
	}
	else if (bRewriteFailed)
	{
		// A failed guarded switch retained its marker, the crash-that-does-not-restart case, so surface the recovery offer now rather than waiting for the next connect.
		FLoreSourceControlModule::Get().GetProvider().DetectInterruptedRewrite();
	}

	return bChanged;
}

// CreateBranch ------------------------------------------------------------------

FName FLoreCreateBranchWorker::GetName() const { return "CreateBranch"; }

bool FLoreCreateBranchWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreCreateBranch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreCreateBranch>(InCommand.Operation);
	RepositoryRoot = InCommand.PathToRepositoryRoot;

	// Create always switches to the new branch in the same step, but the new branch starts at the current working-tree revision, so its delta against the tree is empty and no files are rewritten. It therefore needs no read-only clearing and no guarded rewrite; the guard only brackets a delta.
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunBranchCreate(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Operation->BranchName, FString(), InCommand.ErrorMessages);
	bCreated = InCommand.bCommandSuccessful;

	RefreshStates(InCommand, States);
	return InCommand.bCommandSuccessful;
}

bool FLoreCreateBranchWorker::UpdateStates() const
{
	return LoreSourceControlUtils::UpdateCachedStates(States);
}

// ArchiveBranch -----------------------------------------------------------------

FName FLoreArchiveBranchWorker::GetName() const { return "ArchiveBranch"; }

bool FLoreArchiveBranchWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreArchiveBranch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreArchiveBranch>(InCommand.Operation);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunBranchArchive(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Operation->BranchName, InCommand.ErrorMessages);
	return InCommand.bCommandSuccessful;
}

bool FLoreArchiveBranchWorker::UpdateStates() const
{
	return false;
}

// ProtectBranch and UnprotectBranch ---------------------------------------------

FName FLoreProtectBranchWorker::GetName() const { return "ProtectBranch"; }

bool FLoreProtectBranchWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreProtectBranch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreProtectBranch>(InCommand.Operation);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunBranchSetProtect(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Operation->BranchName, true, InCommand.ErrorMessages);
	return InCommand.bCommandSuccessful;
}

bool FLoreProtectBranchWorker::UpdateStates() const
{
	return false;
}

FName FLoreUnprotectBranchWorker::GetName() const { return "UnprotectBranch"; }

bool FLoreUnprotectBranchWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FLoreUnprotectBranch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreUnprotectBranch>(InCommand.Operation);
	InCommand.bCommandSuccessful = LoreSourceControlUtils::RunBranchSetProtect(InCommand.BackendMode, InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, Operation->BranchName, false, InCommand.ErrorMessages);
	return InCommand.bCommandSuccessful;
}

bool FLoreUnprotectBranchWorker::UpdateStates() const
{
	return false;
}

// Changelists -------------------------------------------------------------------
// Pure edits of the plugin-owned store; the Changes view reads the records live, so an edited store plus a refresh broadcast is the whole update path.

#if LORE_UE5_3_OR_LATER

namespace
{
	// The Changes view labels a changelist by its title, so the description's first line doubles as one.
	FString TitleFromDescription(const FString& InDescription)
	{
		int32 NewlineIndex = INDEX_NONE;
		if (InDescription.FindChar(TEXT('\n'), NewlineIndex))
		{
			return InDescription.Left(NewlineIndex).TrimEnd();
		}
		return InDescription;
	}
}

FName FLoreNewChangelistWorker::GetName() const { return "NewChangelist"; }

bool FLoreNewChangelistWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TSharedRef<FNewChangelist, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FNewChangelist>(InCommand.Operation);
	const FString Description = Operation->GetDescription().ToString();

	FLoreChangelistRecord Record;
	Record.Id = FString::Printf(TEXT("cl-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	Record.Title = TitleFromDescription(Description);
	Record.Description = Description;
	Record.Author = InCommand.Identity;
	Record.Created = FDateTime::UtcNow();
	Record.Updated = Record.Created;

	// The editor's move-to-new-changelist action passes the files to carry into the new changelist.
	if (InCommand.Files.Num() > 0)
	{
		Record.Files = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	}

	InCommand.bCommandSuccessful = LoreChangelists::Mutate(InCommand.PathToRepositoryRoot, [&Record](TArray<FLoreChangelistRecord>& Records)
	{
		for (FLoreChangelistRecord& Existing : Records)
		{
			Existing.Files.RemoveAll([&Record](const FString& File) { return Record.Files.Contains(File); });
		}
		Records.Add(Record);
		return true;
	});
	if (InCommand.bCommandSuccessful)
	{
		Operation->SetNewChangelist(MakeShared<FLoreSourceControlChangelist, ESPMode::ThreadSafe>(Record.Id));
		bStoreEdited = true;
	}
	return InCommand.bCommandSuccessful;
}

bool FLoreNewChangelistWorker::UpdateStates() const
{
	return bStoreEdited;
}

FName FLoreDeleteChangelistWorker::GetName() const { return "DeleteChangelist"; }

bool FLoreDeleteChangelistWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// The Default changelist is mandatory; the editor grays the action out through CanDelete, so this guards a direct caller.
	if (InCommand.ChangelistId == LoreChangelists::DefaultId)
	{
		InCommand.ErrorMessages.Add(NSLOCTEXT("LoreSourceControl", "DeleteDefaultChangelist", "The Default changelist cannot be deleted.").ToString());
		InCommand.bCommandSuccessful = false;
		return false;
	}

	bool bFound = false;
	InCommand.bCommandSuccessful = LoreChangelists::Mutate(InCommand.PathToRepositoryRoot, [&InCommand, &bFound](TArray<FLoreChangelistRecord>& Records)
	{
		bFound = Records.RemoveAll([&InCommand](const FLoreChangelistRecord& Record) { return Record.Id == InCommand.ChangelistId; }) > 0;
		return bFound;
	});
	if (!bFound)
	{
		InCommand.ErrorMessages.Add(FString::Printf(TEXT("Changelist %s was not found."), *InCommand.ChangelistId));
	}
	bStoreEdited = InCommand.bCommandSuccessful;
	return InCommand.bCommandSuccessful;
}

bool FLoreDeleteChangelistWorker::UpdateStates() const
{
	return bStoreEdited;
}

FName FLoreEditChangelistWorker::GetName() const { return "EditChangelist"; }

bool FLoreEditChangelistWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const FString Description = StaticCastSharedRef<FEditChangelist>(InCommand.Operation)->GetDescription().ToString();

	bool bFound = false;
	InCommand.bCommandSuccessful = LoreChangelists::Mutate(InCommand.PathToRepositoryRoot, [&InCommand, &Description, &bFound](TArray<FLoreChangelistRecord>& Records)
	{
		FLoreChangelistRecord* Record = Records.FindByPredicate([&InCommand](const FLoreChangelistRecord& R) { return R.Id == InCommand.ChangelistId; });
		if (!Record)
		{
			return false;
		}
		bFound = true;
		Record->Description = Description;
		Record->Title = TitleFromDescription(Description);
		Record->Updated = FDateTime::UtcNow();
		return true;
	});
	if (!bFound)
	{
		InCommand.ErrorMessages.Add(FString::Printf(TEXT("Changelist %s was not found."), *InCommand.ChangelistId));
	}
	bStoreEdited = InCommand.bCommandSuccessful;
	return InCommand.bCommandSuccessful;
}

bool FLoreEditChangelistWorker::UpdateStates() const
{
	return bStoreEdited;
}

FName FLoreMoveToChangelistWorker::GetName() const { return "MoveToChangelist"; }

bool FLoreMoveToChangelistWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	bool bFound = false;
	InCommand.bCommandSuccessful = LoreChangelists::Mutate(InCommand.PathToRepositoryRoot, [&InCommand, &RelativeFiles, &bFound](TArray<FLoreChangelistRecord>& Records)
	{
		FLoreChangelistRecord* Destination = Records.FindByPredicate([&InCommand](const FLoreChangelistRecord& R) { return R.Id == InCommand.ChangelistId; });
		if (!Destination)
		{
			return false;
		}
		bFound = true;

		// Membership is exclusive, so a file leaves its previous changelist as it joins the destination.
		for (FLoreChangelistRecord& Record : Records)
		{
			Record.Files.RemoveAll([&RelativeFiles](const FString& File) { return RelativeFiles.Contains(File); });
		}
		Destination->Files.Append(RelativeFiles);
		Destination->Updated = FDateTime::UtcNow();
		return true;
	});
	if (!bFound)
	{
		InCommand.ErrorMessages.Add(FString::Printf(TEXT("Changelist %s was not found."), *InCommand.ChangelistId));
	}
	bStoreEdited = InCommand.bCommandSuccessful;
	return InCommand.bCommandSuccessful;
}

bool FLoreMoveToChangelistWorker::UpdateStates() const
{
	return bStoreEdited;
}

FName FLoreUpdateChangelistsStatusWorker::GetName() const { return "UpdateChangelistsStatus"; }

bool FLoreUpdateChangelistsStatusWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	// The records are read live from the store and membership reconciliation runs in the provider's changelist GetState on the game thread, so there is nothing to refresh here.
	InCommand.bCommandSuccessful = true;
	return true;
}

bool FLoreUpdateChangelistsStatusWorker::UpdateStates() const
{
	return false;
}

#endif // LORE_UE5_3_OR_LATER
