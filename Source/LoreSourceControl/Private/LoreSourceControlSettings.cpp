// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "SourceControlHelpers.h"

namespace
{
	const TCHAR* SettingsSection = TEXT("LoreSourceControl.LoreSourceControlSettings");
}

FString FLoreSourceControlSettings::GetBinaryPath() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return BinaryPath;
}

bool FLoreSourceControlSettings::SetBinaryPath(const FString& InString)
{
	FScopeLock ScopeLock(&CriticalSection);
	const bool bChanged = BinaryPath != InString;
	BinaryPath = InString;
	return bChanged;
}

bool FLoreSourceControlSettings::IsUsingLocking() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return bUsingLocking;
}

void FLoreSourceControlSettings::SetUsingLocking(bool bInUsingLocking)
{
	FScopeLock ScopeLock(&CriticalSection);
	bUsingLocking = bInUsingLocking;
}

bool FLoreSourceControlSettings::IsPushAfterCommit() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return bPushAfterCommit;
}

void FLoreSourceControlSettings::SetPushAfterCommit(bool bInPushAfterCommit)
{
	FScopeLock ScopeLock(&CriticalSection);
	bPushAfterCommit = bInPushAfterCommit;
}

FString FLoreSourceControlSettings::GetIdentity() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return Identity;
}

bool FLoreSourceControlSettings::SetIdentity(const FString& InString)
{
	FScopeLock ScopeLock(&CriticalSection);
	const bool bChanged = Identity != InString;
	Identity = InString;
	return bChanged;
}

void FLoreSourceControlSettings::LoadSettings()
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->GetString(SettingsSection, TEXT("BinaryPath"), BinaryPath, IniFile);
	GConfig->GetBool(SettingsSection, TEXT("UsingLocking"), bUsingLocking, IniFile);
	GConfig->GetBool(SettingsSection, TEXT("PushAfterCommit"), bPushAfterCommit, IniFile);
	GConfig->GetString(SettingsSection, TEXT("Identity"), Identity, IniFile);
}

void FLoreSourceControlSettings::SaveSettings() const
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->SetString(SettingsSection, TEXT("BinaryPath"), *BinaryPath, IniFile);
	GConfig->SetBool(SettingsSection, TEXT("UsingLocking"), bUsingLocking, IniFile);
	GConfig->SetBool(SettingsSection, TEXT("PushAfterCommit"), bPushAfterCommit, IniFile);
	GConfig->SetString(SettingsSection, TEXT("Identity"), *Identity, IniFile);
}
