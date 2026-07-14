// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

// Synchronous C++ bridge over the liblore native API. Win64 only; the CLI path stays the default.
#if LORE_WITH_LIBLORE

namespace LoreClient
{
	// One file from a liblore status report, carrying the same fields the CLI JSON parser reads so both paths classify through one mapping.
	struct FStatusFile
	{
		FString Path;       // repository-relative, separators as liblore reports them
		FString FromPath;   // move or copy source, empty otherwise
		FString Action;     // canonical Lore action token: keep, add, delete, move, copy
		bool bIsDirectory = false;
		bool bStaged = false;
		bool bDirty = false;
		bool bConflict = false;
	};

	struct FStatusResult
	{
		bool bRemoteAhead = false;
		bool bRemoteAvailable = false;
		bool bRemoteAuthorized = false;
		FString BranchName;             // current branch, from the status revision header
		FString CurrentRevision;        // current revision hash, the anchor a rewrite starts from
		int32 CurrentRevisionNumber = 0;
		FString RemoteRevision;         // remote branch latest hash, empty when unknown or the remote is unreachable
		int32 RemoteRevisionNumber = 0;
		TArray<FStatusFile> Files;
	};

	// One file a sync would rewrite, from a dry-run sync's revisionSyncFile events.
	struct FSyncDeltaFile
	{
		FString Path;    // repository-relative, separators as liblore reports them
		FString Action;  // canonical Lore action token: keep, add, delete
	};

	// One locked path from a liblore lock-status report.
	struct FLockStatus
	{
		FString Path;   // repository-relative, separators as liblore reports them
		FString Owner;  // lock holder identity; unattributable against an unauthenticated server
	};

	// One revision from a liblore file-history report. Date, Description, and UserName arrive only if the library emits metadata events for history.
	struct FHistoryRevision
	{
		FString CommitId;       // hex of the revision hash
		int32 RevisionNumber = 0;
		FString Action;         // canonical Lore action token
		int32 FileSize = 0;
		FDateTime Date = FDateTime::MinValue();
		FString Description;
		FString UserName;
		FString Address;        // content address ("hash-context" hex), for a local-store probe
	};

	// Identity resolved from an auth operation, from a LORE_EVENT_AUTH_USER_INFO event.
	struct FAuthUserInfo
	{
		FString Id;    // user identity (the value lock ownership compares against)
		FString Name;  // display name
	};

	// One branch from a liblore branch listing. Creator is unattributable against an unauthenticated server.
	struct FBranchInfo
	{
		FString Name;
		FString Category;
		FString Creator;
		FString LatestHash;   // hex of the latest revision the branch points at
		FDateTime Created = FDateTime::MinValue();
		bool bIsCurrent = false;
		bool bArchived = false;
		bool bIsRemote = false;  // remote-located branch rather than local
	};

	// One revision from a liblore revision-history listing. Date, Message, and Author fill from the metadata events the library streams after each entry.
	struct FRevisionEntry
	{
		FString Hash;             // hex of the revision hash
		int32 RevisionNumber = 0;
		FString ParentHash;       // direct parent hex, all zeros for the first revision
		FString ParentOtherHash;  // second merge parent hex, all zeros when not a merge
		FDateTime Date = FDateTime::MinValue();
		FString Message;
		FString Author;
	};

	// One changed file from a liblore revision-info delta.
	struct FRevisionDelta
	{
		FString Path;    // repository-relative, separators as liblore reports them
		FString Action;  // canonical Lore action token
		int64 Size = 0;
		bool bModified = false;
		bool bMerged = false;
		bool bIsFile = false;  // false for a directory entry
	};

	// Metadata and per-file delta of one revision from a liblore revision-info call.
	struct FRevisionDetails
	{
		FString Hash;
		int32 RevisionNumber = 0;
		FDateTime Date = FDateTime::MinValue();
		FString Message;
		FString Author;
		TArray<FRevisionDelta> Deltas;
	};

	/** liblore version string, e.g. "0.8.4-nightly". Empty when the library returns nothing. */
	FString GetVersion();

	// Each call runs its liblore operation synchronously and returns false on failure, with OutError set.

	// bScan walks the filesystem and refreshes dirty flags, so it is passed only on an explicit refresh.
	bool GetStatus(const FString& InRepositoryRoot, const FString& InIdentity, const TArray<FString>& InRelativePaths, bool bScan, FStatusResult& OutResult, FString& OutError);

	bool GetLockStatus(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, TArray<FLockStatus>& OutLocks, FString& OutError);

	bool GetHistory(const FString& InRepositoryRoot, const FString& InRelativeFile, TArray<FHistoryRevision>& OutRevisions, FString& OutError);

	// Enumerate the repository's branches, with creator, category, current, and archived flags. bIncludeArchived adds archived local branches.
	bool BranchList(const FString& InRepositoryRoot, bool bIncludeArchived, TArray<FBranchInfo>& OutBranches, FString& OutError);

	// Read a single branch metadata key, returned as text. Branch protection is the boolean key "protect", readable only here and not through branch info (the branch info documentation comment claiming protection status is wrong, verified against lore 0.8.3). bOutFound is false when the branch carries no such key, in which case OutValue is empty; a boolean value returns "true" or "false". Never called on an archived branch, which errors with "Not found".
	bool BranchMetadataGet(const FString& InRepositoryRoot, const FString& InBranch, const FString& InKey, FString& OutValue, bool& bOutFound, FString& OutError);

	// Set the branch protect flag. The FFI metadata-set path cannot express a boolean (lore_metadata_type_t has only binary, numeric, string; a numeric write does not round-trip through get_bool, which needs a single byte), so protect and unprotect go through the dedicated calls that write the boolean blob correctly. Enforcement is server-side: a push to a protected branch is rejected. bProtect true protects, false unprotects.
	bool BranchSetProtect(const FString& InRepositoryRoot, const FString& InBranch, bool bProtect, FString& OutError);

	// Create a branch at the current working-tree revision and switch to it in the same step; 0.8.3 has no create-without-switch. No file content changes, so it needs no read-only clearing. The remote learns of the branch on the first push.
	bool BranchCreate(const FString& InRepositoryRoot, const FString& InName, const FString& InCategory, FString& OutError);

	// Switch the working tree to a branch, optionally to a specific revision on it. Rewrites the file delta between the source and target. Never passes reset or bare, so a conflicting dirty file refuses the switch rather than being overwritten. Read-only files in the delta must be cleared first by the caller, since the rewrite aborts on one.
	bool BranchSwitch(const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, FString& OutError);

	// Archive a branch locally and, unless in local mode, on the remote. 0.8.3 has no unarchive.
	bool BranchArchive(const FString& InRepositoryRoot, const FString& InBranch, FString& OutError);

	// Dry-run switch to a branch, optionally a revision: reports the per-file delta a switch would apply without touching the working tree. Reuses the sync dry-run accumulator, since the switch dry-run emits the identical revisionSyncFile stream.
	bool EnumerateSwitchDelta(const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, TArray<FSyncDeltaFile>& OutDelta, FString& OutError);

	// List a branch's revisions newest first. Empty InBranch reads the current branch; empty InStartRevision starts at the branch tip. The start revision is included in its page, so the next page starts at the oldest loaded entry's parent. InLength of zero returns the whole history.
	bool RevisionList(const FString& InRepositoryRoot, const FString& InBranch, const FString& InStartRevision, int32 InLength, TArray<FRevisionEntry>& OutRevisions, FString& OutError);

	// Read one revision's metadata, and its per-file delta against the parent when bIncludeDelta. Empty InRevision reads the current revision.
	bool RevisionInfo(const FString& InRepositoryRoot, const FString& InRevision, bool bIncludeDelta, FRevisionDetails& OutDetails, FString& OutError);

	// Probe whether a revision's content is recoverable. With bQueryRemote it checks both the local store and the remote; pass false to answer from the local store alone when the remote is known unreachable, which avoids a connection-timeout stall. bOutAvailable is true when any queried store holds the exact address with its payload.
	bool QueryContentAvailable(const FString& InRepositoryRoot, const FString& InAddress, bool& bOutAvailable, FString& OutError, bool bQueryRemote = true);

	bool Stage(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool Unstage(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool Reset(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool LockAcquire(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool LockRelease(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	// liblore refuses to commit without an identity, so the configured InIdentity is passed through.
	bool Commit(const FString& InRepositoryRoot, const FString& InIdentity, const FString& InMessage, FString& OutError);

	bool Push(const FString& InRepositoryRoot, FString& OutError);

	bool Sync(const FString& InRepositoryRoot, FString& OutError);

	// Dry-run sync: reports the per-file delta a sync would apply without touching the working tree. Threads the global dry_run flag so no file, attribute, or anchor changes.
	bool EnumerateSyncDelta(const FString& InRepositoryRoot, TArray<FSyncDeltaFile>& OutDelta, FString& OutError);

	// Writes the binary content directly, so diff extraction has no text to parse.
	bool WriteFile(const FString& InRepositoryRoot, const FString& InRelativeFile, const FString& InRevision, const FString& InOutputPath, FString& OutError);

	// Authenticate with an existing bearer token. Non-interactive, so it is the path the automated test and the settings panel use. InTokenType is one of "lore", "api-key", "eg1". An empty InRemoteUrl resolves from the repository config; InAuthUrl is used directly when set and is required when no remote URL is available.
	bool LoginWithToken(const FString& InRepositoryRoot, const FString& InRemoteUrl, const FString& InAuthUrl, const FString& InToken, const FString& InTokenType, FAuthUserInfo& OutUser, FString& OutError);

	// Authenticate through a browser. The call blocks until the flow completes, so run it off the game thread. With bNoBrowser the login URL is returned in OutAuthUrl instead of a browser opening.
	bool LoginInteractive(const FString& InRepositoryRoot, const FString& InRemoteUrl, bool bNoBrowser, FString& OutAuthUrl, FAuthUserInfo& OutUser, FString& OutError);

	// Resolve the current identity from locally stored tokens without contacting the auth service. OutUser is empty when no token is stored.
	bool GetLocalUserInfo(const FString& InRepositoryRoot, const FString& InAuthEndpoint, FAuthUserInfo& OutUser, FString& OutError);

	// Remove stored tokens for the auth URL. An empty InAuthUrl resolves from the repository; an empty InUserId removes every identity.
	bool Logout(const FString& InRepositoryRoot, const FString& InAuthUrl, const FString& InUserId, FString& OutError);

	/** Stop the liblore runtime and release its worker threads. Call once at module shutdown when no call is in flight. */
	void Shutdown();
}

#endif
