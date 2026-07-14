// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreGuardedRewrite.h"
#include "LoreSourceControlCommand.h"
#include "LoreSourceControlState.h"
#include "LorePluginStore.h"
#include "Misc/Paths.h"

namespace LoreGuardedRewrite
{
	namespace
	{
		// Delta paths are repository-relative; the read-only calls and the verify pass work in absolute paths.
		TArray<FString> AbsoluteDeltaPaths(const TArray<FLoreDeltaEntry>& InDelta, const FString& InRepositoryRoot)
		{
			TArray<FString> Out;
			Out.Reserve(InDelta.Num());
			for (const FLoreDeltaEntry& Entry : InDelta)
			{
				Out.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(InRepositoryRoot, Entry.Path)));
			}
			return Out;
		}
	}

	bool Run(const FRewriteRequest& InRequest, FLoreSourceControlCommand& InCommand, TFunctionRef<bool(TArray<FString>& OutErrorMessages)> InRewrite, TArray<FLoreSourceControlState>& OutStates)
	{
		const FString& Root = InCommand.PathToRepositoryRoot;
		const TArray<FString> DeltaAbsolute = AbsoluteDeltaPaths(InRequest.Delta, Root);

		// Record the operation before the first mutation the guard performs, so recovery can reapply read-only per the locking policy no matter where a crash falls. Clearing read-only counts as that first mutation, so the marker precedes it.
		FLoreOperationMarker Marker;
		Marker.Kind = InRequest.Kind;
		Marker.Instance = LorePluginStore::ReadInstanceId(Root);
		Marker.SourceBranch = InRequest.SourceBranch;
		Marker.TargetBranch = InRequest.TargetBranch;
		Marker.SourceRevision = InRequest.SourceRevision;
		Marker.TargetRevision = InRequest.TargetRevision;
		Marker.Started = FDateTime::UtcNow();
		for (const FLoreDeltaEntry& Entry : InRequest.Delta)
		{
			Marker.ChangedFiles.Add(Entry.Path);
		}
		LoreOperationMarker::Write(Root, Marker);

		// Lore's rewrite machinery aborts on a read-only file, so clear read-only on the delta paths regardless of the locking setting: the abort depends on the attribute being set, not on why it is set.
		LoreSourceControlUtils::SetFilesReadOnly(DeltaAbsolute, false);

		const bool bRewrote = InRewrite(InCommand.ErrorMessages);

		OutStates.Reset();
		if (!bRewrote)
		{
			// A worker-reported failure that is not a crash leaves the marker in place for recovery, but the cache still shows the tear. The command's game-thread completion routes into the same detection path connect uses.
			LoreSourceControlUtils::RunUpdateStatus(InCommand.BackendMode, InCommand.PathToLoreBinary, Root, InCommand.Identity, InCommand.bUsingLocking, DeltaAbsolute, InCommand.ErrorMessages, OutStates, InCommand.bRemoteAvailable);
			return false;
		}

		// The verify pass scans, scoped to the delta, because a crash means Lore's persisted dirty flags for the half-written files cannot be trusted. The scan reconciles them against the anchor.
		LoreSourceControlUtils::RunUpdateStatus(InCommand.BackendMode, InCommand.PathToLoreBinary, Root, InCommand.Identity, InCommand.bUsingLocking, DeltaAbsolute, InCommand.ErrorMessages, OutStates, InCommand.bRemoteAvailable);

		// Reapply read-only per the locking policy. A delete entry's file is gone, and ApplyReadOnlyFromStates skips a file that does not exist, so delete entries need no special case. Runs only when locking is on.
		if (InCommand.bUsingLocking)
		{
			LoreSourceControlUtils::ApplyReadOnlyFromStates(OutStates);
		}

		if (InRequest.bClearMarkerOnSuccess)
		{
			LoreOperationMarker::Clear(Root);
		}
		return true;
	}

	bool RepairTornFiles(const FLoreOperationMarker& InMarker, ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, bool& bOutAnyReset, TArray<FString>& OutErrorMessages)
	{
		bOutAnyReset = false;
		if (InMarker.ChangedFiles.Num() == 0)
		{
			return true;
		}

		// Scan only the marker's changed files, since only a delta file can be torn. The scan reconciles Lore's persisted dirty flags, which a crash left untrustworthy.
		TArray<FString> AbsoluteFiles;
		AbsoluteFiles.Reserve(InMarker.ChangedFiles.Num());
		for (const FString& Relative : InMarker.ChangedFiles)
		{
			AbsoluteFiles.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(InRepositoryRoot, Relative)));
		}

		TArray<FLoreSourceControlState> States;
		if (!LoreSourceControlUtils::RunUpdateStatus(InMode, InPathToLoreBinary, InRepositoryRoot, InIdentity, bInUsingLocking, AbsoluteFiles, OutErrorMessages, States))
		{
			return false;
		}

		// A file that differs from the anchor reads as changed; reset those back to the anchor. An unchanged file completed its rewrite or never started, so it is left alone.
		TArray<FString> ToReset;
		for (const FLoreSourceControlState& State : States)
		{
			const bool bChanged = State.WorkingCopyState != ELoreWorkingCopyState::Unchanged
				&& State.WorkingCopyState != ELoreWorkingCopyState::Unknown
				&& State.WorkingCopyState != ELoreWorkingCopyState::NotControlled;
			if (bChanged)
			{
				ToReset.Add(LoreSourceControlUtils::RelativeFilename(State.LocalFilename, InRepositoryRoot));
			}
		}

		if (ToReset.Num() == 0)
		{
			return true;
		}

		// A dirty file inside a delta refuses a re-run, so the reset must precede the re-dispatch. Clearing read-only first, since a not-checked-out binary asset is read-only and reset rewrites it.
		LoreSourceControlUtils::SetFilesReadOnly(AbsoluteFiles, false);
		if (!LoreSourceControlUtils::ResetFiles(InMode, InPathToLoreBinary, InRepositoryRoot, ToReset, OutErrorMessages))
		{
			return false;
		}
		bOutAnyReset = true;
		return true;
	}
}
