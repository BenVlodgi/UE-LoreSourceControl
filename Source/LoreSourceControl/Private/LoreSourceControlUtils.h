// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlSettings.h"

class FLoreSourceControlRevision;

#if LORE_WITH_LIBLORE
namespace LoreClient { struct FStatusResult; struct FLockStatus; struct FHistoryRevision; struct FBranchInfo; }
#endif

/** One branch row for the branches window, merging a branch's local and remote listings into one entry keyed by name, shared by the native and command-line paths. An archived branch appears local-only with an all-zeros LatestLocal, so the window renders its Latest column blank and never queries its info or protection. bProtected fills from the branch metadata "protect" key. */
struct FLoreBranchItem
{
	FString Name;
	FString Category;
	FString Creator;
	FString LatestLocal;   // hex of the local latest revision, all zeros when local-only listing is absent or the branch is archived
	FString LatestRemote;  // hex of the remote latest revision, empty when the branch has no remote listing
	FDateTime Created = FDateTime::MinValue();
	bool bIsCurrent = false;
	bool bArchived = false;
	bool bHasLocal = false;   // a local listing entry exists for this branch
	bool bHasRemote = false;  // a remote listing entry exists for this branch
	bool bProtected = false;  // the branch metadata "protect" key reads true; enforcement is the server rejecting a push
};

/** One revision from a repository history listing, shared by the native and command-line paths. Date, Message, and Author fill from the metadata events the client streams after each entry. */
struct FLoreRevisionListEntry
{
	FString Hash;             // hex of revision hash
	int32 RevisionNumber = 0;
	FString ParentHash;       // direct parent hex, all zeros for the first revision
	FString ParentOtherHash;  // second merge parent hex, all zeros when not a merge
	FDateTime Date = FDateTime::MinValue();
	FString Message;
	FString Author;
};

/** One file a working-tree rewrite will touch, produced by the dry-run enumerations and consumed by the guarded rewrite helper. Declared here, not in the guard header, because the utilities produce it and must not depend on the helper. */
struct FLoreDeltaEntry
{
	FString Path;    // repository-relative, separators as Lore reports them
	FString Action;  // action token as the sync events report it: keep, add, delete
};

/** The branch name and revision anchors from the status revision header, the fields a rewrite marker records. RemoteRevision is empty when the remote is unknown or unreachable. */
struct FLoreStatusAnchors
{
	FString BranchName;
	FString CurrentRevision;
	int32 CurrentRevisionNumber = 0;
	FString RemoteRevision;
	int32 RemoteRevisionNumber = 0;
};

/** One changed file from a revision's delta against its parent. */
struct FLoreRevisionDeltaEntry
{
	FString Path;    // repository-relative
	FString Action;  // canonical Lore action token
	int64 Size = 0;
	bool bModified = false;
	bool bMerged = false;
	bool bIsFile = false;  // false for a directory entry
};

/** Metadata and per-file delta of one revision. */
struct FLoreRevisionDetails
{
	FString Hash;
	int32 RevisionNumber = 0;
	FDateTime Date = FDateTime::MinValue();
	FString Message;
	FString Author;
	TArray<FLoreRevisionDeltaEntry> Deltas;
};

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

	/** Read the branch name and revision anchors from the status revision header, through the active backend. A rewrite marker records these. */
	bool GetStatusRevisionAnchors(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FLoreStatusAnchors& OutAnchors, TArray<FString>& OutErrorMessages);

	/** Enumerate the per-file delta a sync to the branch tip would apply, without touching the working tree, through the active backend. The guard cannot protect paths it cannot name, so an enumeration failure refuses the sync before any mutation. Native threads the dry-run global flag; command line runs `lore --dry-run --json sync` and parses revisionSyncFile events. */
	bool EnumerateSyncDelta(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TArray<FLoreDeltaEntry>& OutDelta, TArray<FString>& OutErrorMessages);

	/** Enumerate the per-file delta a switch to InBranch (optionally InRevision) would apply, without touching the working tree, through the active backend. Reuses the sync-delta accumulator and parser, since the switch dry-run emits the identical revisionSyncFile stream. An enumeration failure refuses the switch before any mutation. */
	bool EnumerateSwitchDelta(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, TArray<FLoreDeltaEntry>& OutDelta, TArray<FString>& OutErrorMessages);

	/** Create a branch at the current working-tree revision and switch to it, through the active backend. No files are rewritten, so it needs no read-only clearing; the remote learns of the branch on the first push. Create never falls back, even in Automatic, since a partial create is not safe to retry. */
	bool RunBranchCreate(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InName, const FString& InCategory, TArray<FString>& OutErrorMessages);

	/** Switch the working tree to InBranch, optionally to InRevision on it, through the active backend. Never passes -f, reset, or bare, so a conflicting dirty file refuses the switch. Read-only delta files must be cleared first by the caller. Switch never falls back, even in Automatic, since a partial switch tears the tree. */
	bool RunBranchSwitch(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, TArray<FString>& OutErrorMessages);

	/** Archive InBranch locally and, unless in local mode, on the remote, through the active backend. 0.8.3 has no unarchive. */
	bool RunBranchArchive(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, TArray<FString>& OutErrorMessages);

	/** Set or clear InBranch's protect flag through the active backend. Enforcement is server-side, so this is an advisory client action; the branches widget reads the flag back through RunBranchMetadataGet. */
	bool RunBranchSetProtect(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, bool bProtect, TArray<FString>& OutErrorMessages);

	/** Parse `lore --dry-run --json sync` event lines into delta entries. Exposed for tests. */
	void ParseSyncDeltaForTest(const TArray<FString>& InResults, TArray<FLoreDeltaEntry>& OutDelta);

	/** Write a revision's file content to a destination for diff extraction, through the active backend. */
	bool WriteRevisionFile(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRelativeFile, const FString& InRevision, const FString& InOutputPath, TArray<FString>& OutErrorMessages);

	/** Scan the given files and parse their working-copy and lock state. */
	bool RunUpdateStatus(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates, bool bInRemoteAvailable = true);

	/** Read a file's revision history into shared revision records. bRemoteReachable is forwarded to the content-availability probe so a known-down remote is not contacted per revision. */
	bool RunGetHistory(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, TLoreSourceControlHistory& OutHistory, bool bRemoteReachable = true);

	/** List a branch's revisions newest first, through the active backend. Empty InBranch reads the current branch; empty InStartRevision starts at the branch tip. The start revision is included in its page, so the next page starts at the oldest loaded entry's parent. InLength of zero returns the whole history. */
	bool RunRevisionList(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InStartRevision, int32 InLength, TArray<FLoreRevisionListEntry>& OutRevisions, TArray<FString>& OutErrorMessages);

	/** Read one revision's metadata, and its per-file delta when bIncludeDelta, through the active backend. Empty InRevision reads the current revision. */
	bool RunRevisionInfo(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRevision, bool bIncludeDelta, FLoreRevisionDetails& OutDetails, TArray<FString>& OutErrorMessages);

	/** Merge a branch listing's per-branch local and remote entries into one row per name, so a branch present in both listings folds to a single item carrying both latest hashes. Pure function, testable without a server. bArchived and the all-zeros LatestLocal an archived local entry carries pass through, since an archived branch appears local-only. */
	void MergeBranchListEntries(const TArray<FLoreBranchItem>& InEntries, TArray<FLoreBranchItem>& OutItems);

	/** Enumerate the repository's branches merged into per-branch rows, through the active backend, then read each non-archived row's "protect" key. bIncludeArchived adds archived local branches, rendered local-only with a blank latest and never sent for protection. */
	bool RunGetBranches(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, bool bIncludeArchived, TArray<FLoreBranchItem>& OutBranches, TArray<FString>& OutErrorMessages);

	/** Read a single branch metadata key as text, through the active backend. bOutFound is false when the branch has no such key; a boolean value returns "true" or "false". Never called on an archived branch. */
	bool RunBranchMetadataGet(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InKey, FString& OutValue, bool& bOutFound, TArray<FString>& OutErrorMessages);

	/** Collect reasons a submit must be refused before it mutates the staged set. With bAllowOverLock a foreign lock no longer blocks; an unresolved conflict always does. Returns true when any file blocks the submit. */
	bool CollectCheckInBlockers(const TArray<FLoreSourceControlState>& InStates, bool bAllowOverLock, TArray<FString>& OutMessages);

	/** Push parsed states into the provider's cache. Returns whether anything changed. Game thread only. */
	bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates);

	/** Whether an asset is one the locking policy locks: a binary Unreal asset. Text files merge and stage without a lock. */
	bool IsLockableAsset(const FString& InFile);

	/** Reassert the read-only attribute from the locking policy across the given states: a committed binary asset not locked by this user reads read-only, everything else writable. Lore never sets read-only itself, so a status pass reapplies it. Game or worker thread. */
	void ApplyReadOnlyFromStates(const TArray<FLoreSourceControlState>& InStates);

	/** Set the read-only attribute on the given absolute files, skipping any that do not exist. */
	void SetFilesReadOnly(const TArray<FString>& InFiles, bool bReadOnly);

	/** Convert absolute paths to repository-relative for the CLI. */
	TArray<FString> RelativeFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
	FString RelativeFilename(const FString& InFile, const FString& InRepositoryRoot);
	TArray<FString> AbsoluteFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot);
}
