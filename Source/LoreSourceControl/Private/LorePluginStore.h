// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Plugin-owned on-disk state co-located with a Lore working tree, under .lore/.ueplugin. Lore writes only immutable/ and mutable/ under .lore and never deletes an unknown subfolder there, so this folder is safe for the plugin's changelist model, operation marker, and stash index. Every write is atomic (temporary file then rename) so a reader never sees a half-written file.
namespace LorePluginStore
{
	// Absolute path to the plugin store root for a working tree: <repositoryRoot>/.lore/.ueplugin.
	FString GetStoreRoot(const FString& InRepositoryRoot);

	// Absolute path to a named file under the store root. Creates no directories.
	FString GetStorePath(const FString& InRepositoryRoot, const FString& InRelativePath);

	// Per-checkout instance identifier, hex of the bytes in .lore/instance. Empty when the file is absent or unreadable. Changelists are keyed by this so two working trees against one store never overwrite each other's lists.
	FString ReadInstanceId(const FString& InRepositoryRoot);

	// Read a JSON object from a store file. Returns false when the file is absent or does not parse as an object.
	bool ReadJsonObject(const FString& InAbsolutePath, TSharedPtr<FJsonObject>& OutObject);

	// Write a JSON object to a store file atomically: serialize to a sibling temporary file, then rename over the destination so a concurrent reader sees either the old file or the new one, never a partial write. Creates parent directories. Returns false on a serialize or filesystem error.
	bool WriteJsonObjectAtomic(const FString& InAbsolutePath, const TSharedRef<FJsonObject>& InObject);
}
