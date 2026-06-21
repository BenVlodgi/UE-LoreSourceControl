// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlState.h"

class FLoreSourceControlRevision;

namespace LoreSourceControlUtils
{
	/** Parse `lore status --json` event lines into a relative-path to working-copy-state map and the branch behind-remote flag. Exposed for tests. */
	void ParseStatusForTest(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates, bool& bOutRemoteAhead);

	/** Parse `lore lock status --json` event lines into a relative-path to owner map. Exposed for tests. */
	void ParseLocksForTest(const TArray<FString>& InResults, TMap<FString, FString>& OutLocks);

	/** Parse the remote reachability and authorization flags from the status revision header. Exposed for tests. */
	void ParseRemoteAuthForTest(const TArray<FString>& InResults, bool& bOutRemoteAvailable, bool& bOutRemoteAuthorized);

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

	/** Probe the remote from the status revision header: whether it is reachable and whether the client is authorized. */
	void GetRemoteAuthState(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, bool& bOutRemoteAvailable, bool& bOutRemoteAuthorized);

	/** Launch `lore auth login` in the background; it opens a browser to authenticate the client. */
	void LaunchLogin(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRemoteUrl);

	/** The single process chokepoint: build the argument list, run Lore, capture output. */
	bool RunCommand(const FString& InCommand, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages);

	/** Scan the given files and parse their working-copy and lock state. */
	bool RunUpdateStatus(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates);

	/** Read a file's revision history into shared revision records. */
	bool RunGetHistory(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, TLoreSourceControlHistory& OutHistory);

	/** Collect reasons a submit must be refused before it mutates the staged set. With bAllowOverLock a foreign lock no longer blocks; an unresolved conflict always does. Returns true when any file blocks the submit. */
	bool CollectCheckInBlockers(const TArray<FLoreSourceControlState>& InStates, bool bAllowOverLock, TArray<FString>& OutMessages);

	/** Push parsed states into the provider's cache. Returns whether anything changed. Game thread only. */
	bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates);

	/** Convert absolute paths to repository-relative for the CLI. */
	TArray<FString> RelativeFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
	FString RelativeFilename(const FString& InFile, const FString& InRepositoryRoot);
	TArray<FString> AbsoluteFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
}
