// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

/** Machine-local settings, persisted to the source control settings ini. */
class FLoreSourceControlSettings
{
public:
	/** Path to the Lore executable. Empty means resolve from PATH. */
	FString GetBinaryPath() const;
	bool SetBinaryPath(const FString& InString);

	/** Whether check out acquires a Lore lock and binary assets are read-only until checked out. */
	bool IsUsingLocking() const;
	void SetUsingLocking(bool bInUsingLocking);

	/** Whether submit pushes the new revision to the server. */
	bool IsPushAfterCommit() const;
	void SetPushAfterCommit(bool bInPushAfterCommit);

	/** Commit identity (email) the editor records on commits. Empty falls back to the working tree configuration. */
	FString GetIdentity() const;
	bool SetIdentity(const FString& InString);

	/** Load and save from the source control settings ini. */
	void LoadSettings();
	void SaveSettings() const;

private:
	/** Critical section for settings access */
	mutable FCriticalSection CriticalSection;

	/** Path to the Lore binary */
	FString BinaryPath;

	/** True when check out takes a lock and binary assets are read-only until checked out */
	bool bUsingLocking = true;

	/** True when check in pushes the new revision to the server */
	bool bPushAfterCommit = true;

	/** Commit identity (email) recorded on commits, overriding the working tree configuration when set */
	FString Identity;
};
