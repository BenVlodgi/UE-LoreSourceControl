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

	// Restore read-only on tracked binary assets whose lock released.
	void RestoreReadOnlyFromStates(const TArray<FLoreSourceControlState>& InStates)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (const FLoreSourceControlState& State : InStates)
		{
			if (State.LockState == ELoreLockState::NotLocked && State.IsSourceControlled() && IsLockableAsset(State.LocalFilename) && PlatformFile.FileExists(*State.LocalFilename))
			{
				PlatformFile.SetReadOnly(*State.LocalFilename, true);
			}
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
	FLoreSourceControlModule::Get().GetProvider().SetRepositoryInfo(bAvailable, bRepositoryFound, RepositoryRoot, RemoteUrl, Identity, BranchName, RepositoryId, LoreVersion);
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

	if (InCommand.bCommandSuccessful && InCommand.bUsingLocking)
	{
		LoreSourceControlUtils::RunCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	}

	RefreshStates(InCommand, States);
	if (InCommand.bCommandSuccessful && InCommand.bUsingLocking)
	{
		RestoreReadOnlyFromStates(States);
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

FName FLoreCheckInWorker::GetName() const { return "CheckIn"; }

bool FLoreCheckInWorker::Execute(FLoreSourceControlCommand& InCommand)
{
	const TArray<FString> RelativeFiles = LoreSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);
	TArray<FString> Results;

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
	if (InCommand.bCommandSuccessful && InCommand.bUsingLocking && !bKeepCheckedOut)
	{
		LoreSourceControlUtils::RunCommand(TEXT("lock release"), InCommand.PathToLoreBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), RelativeFiles, Results, InCommand.ErrorMessages);
	}

	// Make the success message specific: count, description, and file names.
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
		RestoreReadOnlyFromStates(States);
	}
	return InCommand.bCommandSuccessful;
}

bool FLoreCheckInWorker::UpdateStates() const
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
