// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlState.h"

class FLoreSourceControlRevision;

namespace LoreSourceControlUtils
{
	/** Parse `lore status` output lines into a relative-path to working-copy-state map. Exposed for tests. */
	void ParseStatusForTest(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates);

	/** Parse `lore lock status` output into a relative-path to owner map. Exposed for tests. */
	void ParseLocksForTest(const TArray<FString>& InResults, TMap<FString, FString>& OutLocks);

	/** Find the Lore client on PATH or common install locations. Empty if not found. */
	FString FindLoreBinaryPath();

	/** The Lore client version this plugin is validated against. */
	FString GetPinnedVersion();

	/** Absolute path where a bundled Lore client lives, whether or not it exists yet. */
	FString GetBundledClientPath();

	/** True when the bundled Lore client is present. */
	bool IsBundledClientPresent();

	/** Express an absolute path relative to the plugin when it sits inside it, else return it unchanged. */
	FString MakeBinaryPathRelativeToPlugin(const FString& InAbsolute);

	/** Resolve a stored binary path to an absolute one for running; a bare name is left for PATH. */
	FString ResolveBinaryPath(const FString& InStored);

	/** Run `lore --version` and confirm the binary is usable, returning the version string. */
	bool CheckLoreAvailability(const FString& InPathToLoreBinary, FString& OutVersion);

	/** Walk up from a directory to the one holding a `.lore` folder. */
	bool GetRepositoryRoot(const FString& InPathToProjectDir, FString& OutRepositoryRoot);

	/** Read remote_url and identity from `.lore/config.toml`. */
	void GetRepositoryConfig(const FString& InRepositoryRoot, FString& OutRemoteUrl, FString& OutIdentity);

	/** Read the current branch name. */
	bool GetBranchName(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FString& OutBranchName);

	/** The single process chokepoint: build the argument list, run Lore, capture output. */
	bool RunCommand(const FString& InCommand, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages);

	/** Scan the given files and parse their working-copy and lock state. */
	bool RunUpdateStatus(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates);

	/** Read a file's revision history into shared revision records. */
	bool RunGetHistory(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, TLoreSourceControlHistory& OutHistory);

	/** Push parsed states into the provider's cache. Returns whether anything changed. Game thread only. */
	bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates);

	/** Convert absolute paths to repository-relative for the CLI. */
	TArray<FString> RelativeFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
	FString RelativeFilename(const FString& InFile, const FString& InRepositoryRoot);
	TArray<FString> AbsoluteFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
}
