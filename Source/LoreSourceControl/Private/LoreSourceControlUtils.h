// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlSettings.h"

class FLoreSourceControlRevision;

#if LORE_WITH_LIBLORE
namespace LoreClient { struct FStatusResult; struct FLockStatus; struct FHistoryRevision; }
#endif

namespace LoreSourceControlUtils
{
	/** Parse `lore status --json` event lines into a relative-path to working-copy-state map and the branch behind-remote flag. Exposed for tests. */
	void ParseStatusForTest(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates, bool& bOutRemoteAhead);

#if LORE_WITH_LIBLORE
	/** Classify a liblore status result into the same map the CLI JSON parser yields, so the native and command-line paths converge on one classifier. */
	void ParseStatusFromClient(const LoreClient::FStatusResult& InResult, TMap<FString, ELoreWorkingCopyState::Type>& OutStates, bool& bOutRemoteAhead);

	/** Build the relative-path to owner map the CLI lock parser yields from a liblore lock-status result. */
	void ParseLocksFromClient(const TArray<LoreClient::FLockStatus>& InLocks, TMap<FString, FString>& OutLocks);

	/** Build revision records from a liblore file-history result, applying the same action labels and short-id rule the CLI history path uses. bRemoteReachable lets the content-availability probe consult the remote; pass false when the remote is known down to keep the probe local-only and fast. */
	void BuildHistoryFromClient(ELoreBackendMode InMode, const TArray<LoreClient::FHistoryRevision>& InRevisions, const FString& InAbsoluteFile, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TLoreSourceControlHistory& OutHistory, bool bRemoteReachable = true);
#endif

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

	/** Confirm Lore is usable and return its version. Native reads the library version; command line runs `lore --version`. */
	bool CheckLoreAvailability(ELoreBackendMode InMode, const FString& InPathToLoreBinary, FString& OutVersion);

	/** Walk up from a directory to the one holding a `.lore` folder. */
	bool GetRepositoryRoot(const FString& InPathToProjectDir, FString& OutRepositoryRoot);

	/** Read remote_url and identity from `.lore/config.toml`. */
	void GetRepositoryConfig(const FString& InRepositoryRoot, FString& OutRemoteUrl, FString& OutIdentity);

	/** Read the current branch name through the active backend. */
	bool GetBranchName(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FString& OutBranchName);

	/** Probe the remote from the status revision header: whether it is reachable and whether the client is authorized. */
	void GetRemoteAuthState(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, bool& bOutRemoteAvailable, bool& bOutRemoteAuthorized);

	/** Launch `lore auth login` in the background; it opens a browser to authenticate the client. */
	void LaunchLogin(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRemoteUrl);

	/** Authenticate with a bearer token through the active backend. Non-interactive, so it is the path the automated test uses; InTokenType is "lore", "api-key", or "eg1". */
	bool LoginWithToken(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRemoteUrl, const FString& InToken, const FString& InTokenType, FString& OutError);

	/** Authenticate through a browser, through the active backend. The native path blocks until the flow completes, so run it off the game thread; the command-line path launches detached. */
	bool LoginInteractive(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRemoteUrl, FString& OutError);

	/** Remove stored tokens for the working tree's auth URL, through the active backend. */
	bool Logout(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FString& OutError);

	/** The single process chokepoint: build the argument list, run Lore, capture output. */
	bool RunCommand(const FString& InCommand, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages);

	/** Stage the given files through the active backend (native by default; command line or fallback by mode). */
	bool StageFiles(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages);

	/** Remove the given files from the staged set, through the active backend. */
	bool UnstageFiles(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages);

	/** Reset the given files to the current revision, through the active backend. */
	bool ResetFiles(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages);

	/** Acquire locks on the given files, through the active backend. */
	bool AcquireLocks(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages);

	/** Release locks on the given files, through the active backend. */
	bool ReleaseLocks(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages);

	/** Commit the staged set with the given message and identity. Commit never falls back, even in Automatic. */
	bool CommitChanges(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, const FString& InMessage, TArray<FString>& OutErrorMessages);

	/** Push the current branch to the remote, through the active backend. */
	bool PushBranch(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TArray<FString>& OutErrorMessages);

	/** Sync the working tree to the branch tip. Sync never falls back, even in Automatic. */
	bool SyncWorkingTree(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TArray<FString>& OutErrorMessages);

	/** Write a revision's file content to a destination for diff extraction, through the active backend. */
	bool WriteRevisionFile(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRelativeFile, const FString& InRevision, const FString& InOutputPath, TArray<FString>& OutErrorMessages);

	/** Scan the given files and parse their working-copy and lock state. */
	bool RunUpdateStatus(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates);

	/** Read a file's revision history into shared revision records. bRemoteReachable is forwarded to the content-availability probe so a known-down remote is not contacted per revision. */
	bool RunGetHistory(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, TLoreSourceControlHistory& OutHistory, bool bRemoteReachable = true);

	/** Collect reasons a submit must be refused before it mutates the staged set. With bAllowOverLock a foreign lock no longer blocks; an unresolved conflict always does. Returns true when any file blocks the submit. */
	bool CollectCheckInBlockers(const TArray<FLoreSourceControlState>& InStates, bool bAllowOverLock, TArray<FString>& OutMessages);

	/** Push parsed states into the provider's cache. Returns whether anything changed. Game thread only. */
	bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates);

	/** Convert absolute paths to repository-relative for the CLI. */
	TArray<FString> RelativeFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
	FString RelativeFilename(const FString& InFile, const FString& InRepositoryRoot);
	TArray<FString> AbsoluteFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
}
