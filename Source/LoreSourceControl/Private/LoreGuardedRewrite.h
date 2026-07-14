// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreOperationMarker.h"
#include "LoreSourceControlUtils.h"

class FLoreSourceControlCommand;
class FLoreSourceControlState;

// The shared crash-protection guard every working-tree rewrite runs behind. Lore has no working-tree transaction and no recovery of its own, so the guard records the operation in a marker, clears the read-only attribute on the delta paths (Lore's rewrite machinery aborts on a read-only file), runs the rewrite, verifies the tree with a scoped status pass, reapplies read-only per the locking policy, and clears the marker. The sync worker adopts it now; the branch switch and the revision move adopt the same function later.
namespace LoreGuardedRewrite
{
	struct FRewriteRequest
	{
		ELoreOperationKind Kind = ELoreOperationKind::None;
		FString SourceBranch;
		FString TargetBranch;              // empty when the rewrite stays on the current branch
		FString SourceRevision;            // anchor before the rewrite, from the status revision header
		FString TargetRevision;            // empty means the branch tip
		TArray<FLoreDeltaEntry> Delta;     // per-file path and action the rewrite will touch
		bool bClearMarkerOnSuccess = true; // the switch sequence defers the clear past the package reload
	};

	// Brackets one working-tree rewrite with the crash protections: write the marker, clear read-only on the delta paths, run the rewrite, verify with a status pass scoped to the delta, reapply read-only per the locking policy, and clear the marker. On rewrite failure the marker stays for recovery, the states are still refreshed so the cache shows the tear, and the function returns false.
	bool Run(const FRewriteRequest& InRequest, FLoreSourceControlCommand& InCommand, TFunctionRef<bool(TArray<FString>& OutErrorMessages)> InRewrite, TArray<FLoreSourceControlState>& OutStates);

	// Repair a tree torn by an interrupted rewrite: scan the marker's changed files and reset the ones the scan flags dirty back to the anchor. Dirty means the file differs from the anchor, which never moved on a torn rewrite, so a reset returns the pre-rewrite content. When the scan flags nothing dirty, the rewrite completed or never started and nothing is reset. Returns true when the repair succeeded (including the nothing-to-do case). bOutAnyReset reports whether any file was reset, so the caller knows whether a re-dispatch of the rewrite is needed.
	bool RepairTornFiles(const FLoreOperationMarker& InMarker, ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, bool& bOutAnyReset, TArray<FString>& OutErrorMessages);
}
