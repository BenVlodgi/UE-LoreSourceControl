// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

// One plugin-authored changelist: a named, described grouping of changed files. Lore has a single staged set per working tree and no native changelist, so this record is the plugin's own model, persisted to the plugin store and never sent to the server as a changelist. The description doubles as the work-in-progress commit message.
struct FLoreChangelistRecord
{
	FString Id;             // stable identifier; the built-in changelist uses LoreChangelists::DefaultId
	FString Title;
	FString Description;    // also the commit message when this changelist is submitted
	TArray<FString> Files;  // repository-relative paths assigned to this changelist
	FDateTime Created = FDateTime::MinValue();
	FDateTime Updated = FDateTime::MinValue();
	FString Author;

	bool IsDefault() const;
};

namespace LoreChangelists
{
	// The mandatory built-in changelist. The editor's Changes view renders only from changelist membership, so a changed file in no changelist is invisible; the Default changelist holds files not assigned elsewhere.
	extern const TCHAR* const DefaultId;

	// Load the changelist records for this working tree's instance. Always returns at least the Default changelist, even when the store file is absent, so the Changes view can render unassigned files.
	TArray<FLoreChangelistRecord> Load(const FString& InRepositoryRoot);

	// Persist the changelist records for this working tree's instance to the plugin store, atomically.
	bool Save(const FString& InRepositoryRoot, const TArray<FLoreChangelistRecord>& InRecords);

	// Load, edit, and save the records as one step under a process-wide lock. Workers edit the store on pool threads while the provider reconciles it on the game thread, so an unguarded load-edit-save loses whichever write lands in between. The edit returns false to abandon the change; nothing is saved then. Returns true when the edit ran and the save succeeded.
	bool Mutate(const FString& InRepositoryRoot, TFunctionRef<bool(TArray<FLoreChangelistRecord>&)> InEdit);

	// Reconcile the records against the currently changed files in place: rewrite a moved file by its new path, drop a file that is no longer changed, and assign any changed file not already in a changelist to Default. Empty non-Default changelists are kept so a reusable description survives, and a file the user moved into another changelist is never pulled back to Default. InMovesFromTo maps a move's source path to its destination.
	void Reconcile(TArray<FLoreChangelistRecord>& Records, const TArray<FString>& InChangedFiles, const TMap<FString, FString>& InMovesFromTo);
}
