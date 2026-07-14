// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

// The working-tree rewrite a marker guards. Lore has no working-tree transaction and no recovery of its own, so the plugin records the operation before it touches the tree and clears it only after the rewrite and the following status pass both complete. A marker found at connect means a rewrite was interrupted.
enum class ELoreOperationKind : uint8
{
	None,
	BranchSwitch,
	RevisionSync,
	ReturnToLatest,
};

struct FLoreOperationMarker
{
	ELoreOperationKind Kind = ELoreOperationKind::None;
	FString Instance;              // per-checkout instance identifier, so a shared store never reinvents the lost-update problem
	FString SourceBranch;          // branch the rewrite started on
	FString TargetBranch;          // switch target; empty otherwise
	FString SourceRevision;        // revision before the rewrite, to roll back to on recovery
	FString TargetRevision;        // intended revision after the rewrite; empty means the branch tip
	TArray<FString> ChangedFiles;  // files the rewrite touches, from the delta enumerated before it
	FDateTime Started = FDateTime::MinValue();

	bool IsValid() const { return Kind != ELoreOperationKind::None; }
};

namespace LoreOperationMarker
{
	// Write the marker before a working-tree rewrite. The atomic store write flushes it to disk, so a crash mid-rewrite leaves the marker for recovery on the next connect. The marker is keyed by the instance recorded on it, falling back to the unkeyed file when it carries none.
	bool Write(const FString& InRepositoryRoot, const FLoreOperationMarker& InMarker);

	// Read the marker for this working tree's instance. Returns a marker with Kind None when none is present.
	FLoreOperationMarker Read(const FString& InRepositoryRoot);

	// Clear the marker for this working tree's instance once the rewrite and the post-rewrite status pass have both completed.
	bool Clear(const FString& InRepositoryRoot);
}
