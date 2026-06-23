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
		FString BranchName;   // current branch, from the status revision header
		TArray<FStatusFile> Files;
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
	};

	// Identity resolved from an auth operation, from a LORE_EVENT_AUTH_USER_INFO event.
	struct FAuthUserInfo
	{
		FString Id;    // user identity (the value lock ownership compares against)
		FString Name;  // display name
	};

	/** liblore version string, e.g. "0.8.4-nightly". Empty when the library returns nothing. */
	FString GetVersion();

	// Each call runs its liblore operation synchronously and returns false on failure, with OutError set.

	// bScan walks the filesystem and refreshes dirty flags, so it is passed only on an explicit refresh.
	bool GetStatus(const FString& InRepositoryRoot, const FString& InIdentity, const TArray<FString>& InRelativePaths, bool bScan, FStatusResult& OutResult, FString& OutError);

	bool GetLockStatus(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, TArray<FLockStatus>& OutLocks, FString& OutError);

	bool GetHistory(const FString& InRepositoryRoot, const FString& InRelativeFile, TArray<FHistoryRevision>& OutRevisions, FString& OutError);

	bool Stage(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool Unstage(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool Reset(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool LockAcquire(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	bool LockRelease(const FString& InRepositoryRoot, const TArray<FString>& InRelativePaths, FString& OutError);

	// liblore refuses to commit without an identity, so the configured InIdentity is passed through.
	bool Commit(const FString& InRepositoryRoot, const FString& InIdentity, const FString& InMessage, FString& OutError);

	bool Push(const FString& InRepositoryRoot, FString& OutError);

	bool Sync(const FString& InRepositoryRoot, FString& OutError);

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
