// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

/** Which backend the plugin drives Lore through. */
enum class ELoreBackendMode : uint8
{
	Native,       // liblore in process; the command line is never invoked
	CommandLine,  // the lore subprocess only; liblore is never called
	Automatic,    // native first, command line when a native call returns an error
};

/** Machine-local settings, persisted to the source control settings ini. */
class FLoreSourceControlSettings
{
public:
	/** Path to the Lore executable. Empty means resolve from PATH. */
	FString GetBinaryPath() const;
	bool SetBinaryPath(const FString& InString);

	/** Which backend operations run through. Native by default, so a default install never needs the command-line client. */
	ELoreBackendMode GetBackendMode() const;
	void SetBackendMode(ELoreBackendMode InMode);

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

	/** Which backend operations run through */
	ELoreBackendMode BackendMode = ELoreBackendMode::Native;

	/** True when check out takes a lock and binary assets are read-only until checked out */
	bool bUsingLocking = true;

	/** True when check in pushes the new revision to the server */
	bool bPushAfterCommit = true;

	/** Commit identity (email) recorded on commits, overriding the working tree configuration when set */
	FString Identity;
};
