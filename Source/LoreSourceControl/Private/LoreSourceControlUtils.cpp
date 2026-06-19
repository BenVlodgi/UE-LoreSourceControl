// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlUtils.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlRevision.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlLog.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"

namespace LoreSourceControlConstants
{
	// Validated client version
	const TCHAR* PinnedVersion = TEXT("0.8.3");
}

namespace
{
	// Single process chokepoint, so launching can change in one place.
	bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors)
	{
		FString FullCommand = InCommand;
		for (const FString& Parameter : InParameters)
		{
			FullCommand += TEXT(" ");
			FullCommand += Parameter;
		}

		for (const FString& File : InFiles)
		{
			FullCommand += TEXT(" \"");
			FullCommand += File;
			FullCommand += TEXT("\"");
		}

		int32 ReturnCode = -1;
		const TCHAR* WorkingDirectory = InRepositoryRoot.IsEmpty() ? nullptr : *InRepositoryRoot;
		const bool bLaunched = FPlatformProcess::ExecProcess(*InPathToLoreBinary, *FullCommand, &ReturnCode, &OutResults, &OutErrors, WorkingDirectory);
		const bool bSucceeded = bLaunched && ReturnCode == 0;
		// Log every command and failure; operations otherwise fail silently behind the notification.
		if (bSucceeded)
		{
			UE_LOG(LogLoreSourceControl, Verbose, TEXT("lore %s"), *FullCommand);
		}
		else
		{
			const FString Trimmed = OutErrors.TrimStartAndEnd();
			UE_LOG(LogLoreSourceControl, Warning, TEXT("lore %s failed (exit %d)%s%s"), *FullCommand, ReturnCode, Trimmed.IsEmpty() ? TEXT("") : TEXT(": "), *Trimmed);
		}
		return bSucceeded;
	}

	void SplitLines(const FString& InText, TArray<FString>& OutLines)
	{
		InText.ParseIntoArrayLines(OutLines, false);
		for (FString& Line : OutLines)
		{
			Line.TrimEndInline();
		}
	}

	// Parse a Lore date like "Thu, 18 Jun 2026 07:27:40 +0000"
	bool ParseLoreDate(const FString& InText, FDateTime& OutDate)
	{
		if (FDateTime::ParseHttpDate(InText, OutDate))
		{
			return true;
		}

		// ParseHttpDate wants a trailing "GMT"; Lore emits a numeric UTC zone, so swap it and retry.
		FString Normalized = InText.TrimEnd();
		int32 LastSpace = INDEX_NONE;
		if (Normalized.FindLastChar(TEXT(' '), LastSpace))
		{
			const FString Zone = Normalized.RightChop(LastSpace + 1);
			// Only a zero offset is safe to call GMT; a real offset would shift the time.
			if (Zone == TEXT("+0000") || Zone == TEXT("-0000"))
			{
				Normalized = Normalized.Left(LastSpace) + TEXT(" GMT");
				return FDateTime::ParseHttpDate(Normalized, OutDate);
			}
		}

		return false;
	}

	// Current section of `lore status` output.
	enum class EStatusSection
	{
		None,
		Untracked,
		NotStaged,
		Staged,
	};

	ELoreWorkingCopyState::Type ActionLetterToState(const TCHAR Letter, EStatusSection Section)
	{
		// Untracked files are not source controlled, though the scan letters them added.
		if (Section == EStatusSection::Untracked)
		{
			return ELoreWorkingCopyState::NotControlled;
		}

		switch (Letter)
		{
		case 'A': return ELoreWorkingCopyState::Added;
		case 'M': return ELoreWorkingCopyState::Modified;
		case 'D': return ELoreWorkingCopyState::Deleted;
		case 'V': return ELoreWorkingCopyState::Moved;
		default:  return ELoreWorkingCopyState::Modified;
		}
	}

	void ParseStatusResults(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates)
	{
		EStatusSection Section = EStatusSection::None;
		for (const FString& Line : InResults)
		{
			if (Line.StartsWith(TEXT("Untracked files")))
			{
				Section = EStatusSection::Untracked;
				continue;
			}

			if (Line.StartsWith(TEXT("Changes not staged")))
			{
				Section = EStatusSection::NotStaged;
				continue;
			}

			if (Line.StartsWith(TEXT("Changes staged")))
			{
				Section = EStatusSection::Staged;
				continue;
			}

			if (Section == EStatusSection::None || Line.IsEmpty() || Line.StartsWith(TEXT("Tracked changes")) || Line.StartsWith(TEXT("No tracked changes")))
			{
				continue;
			}

			// A per file line is "<LETTER> <path>"
			if (Line.Len() >= 3 && Line[1] == TEXT(' '))
			{
				const ELoreWorkingCopyState::Type State = ActionLetterToState(Line[0], Section);
				FString Path = Line.RightChop(2).TrimStartAndEnd();
				// A rename reads "<from> -> <to>", so key on the destination
				int32 ArrowIndex = INDEX_NONE;
				if (Path.FindLastChar(TEXT('>'), ArrowIndex) && ArrowIndex > 0 && Path[ArrowIndex - 1] == TEXT('-'))
				{
					Path = Path.RightChop(ArrowIndex + 1).TrimStartAndEnd();
				}

				Path.ReplaceInline(TEXT("\\"), TEXT("/"));
				OutStates.Add(Path, State);
			}
		}
	}

	void ParseLockResults(const TArray<FString>& InResults, TMap<FString, FString>& OutLocks)
	{
		// Records read "<path> by <owner> on <date>". A path can contain " by ", so strip " on <date>" first, then split on the last " by ".
		const FString ByToken = TEXT(" by ");
		bool bInLockSection = false;
		for (const FString& Line : InResults)
		{
			if (Line.StartsWith(TEXT("Files locked")))
			{
				bInLockSection = true;
				continue;
			}

			if (!bInLockSection || Line.IsEmpty())
			{
				continue;
			}

			FString Record = Line;
			const int32 OnIndex = Record.Find(TEXT(" on "), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (OnIndex != INDEX_NONE)
			{
				Record = Record.Left(OnIndex);
			}

			const int32 ByIndex = Record.Find(ByToken, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (ByIndex == INDEX_NONE)
			{
				continue;
			}

			FString Path = Record.Left(ByIndex).TrimStartAndEnd();
			const FString Owner = Record.RightChop(ByIndex + ByToken.Len()).TrimStartAndEnd();
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			OutLocks.Add(Path, Owner);
		}
	}
}

namespace LoreSourceControlUtils
{

void ParseStatusForTest(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates)
{
	ParseStatusResults(InResults, OutStates);
}

void ParseLocksForTest(const TArray<FString>& InResults, TMap<FString, FString>& OutLocks)
{
	ParseLockResults(InResults, OutLocks);
}

FString RelativeFilename(const FString& InFile, const FString& InRepositoryRoot)
{
	FString Result = FPaths::ConvertRelativePathToFull(InFile);
	FString Root = InRepositoryRoot;
	FPaths::NormalizeDirectoryName(Root);
	if (FPaths::MakePathRelativeTo(Result, *(Root / TEXT(""))))
	{
		return Result;
	}
	return InFile;
}

TArray<FString> RelativeFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot)
{
	TArray<FString> Result;
	Result.Reserve(InFiles.Num());
	for (const FString& File : InFiles)
	{
		Result.Add(RelativeFilename(File, InRepositoryRoot));
	}
	return Result;
}

TArray<FString> AbsoluteFilenames(const TArray<FString>& InFiles, const FString& InRepositoryRoot)
{
	TArray<FString> Result;
	Result.Reserve(InFiles.Num());
	for (const FString& File : InFiles)
	{
		Result.Add(FPaths::ConvertRelativePathToFull(InRepositoryRoot, File));
	}
	return Result;
}

FString GetBundledClientPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LoreSourceControl"));
	if (!Plugin.IsValid())
	{
		return FString();
	}
	const FString Exe = PLATFORM_WINDOWS ? TEXT("lore.exe") : TEXT("lore");
	const FString Platform = PLATFORM_WINDOWS ? TEXT("Win64") : (PLATFORM_MAC ? TEXT("Mac") : TEXT("Linux"));
	return Plugin->GetBaseDir() / TEXT("Binaries") / TEXT("ThirdParty") / TEXT("Lore") / Platform / Exe;
}

bool IsBundledClientPresent()
{
	const FString Bundled = GetBundledClientPath();
	return !Bundled.IsEmpty() && FPaths::FileExists(Bundled);
}

FString FindLoreBinaryPath()
{
	// A bundled copy wins over a system install.
	const FString Bundled = GetBundledClientPath();
	if (!Bundled.IsEmpty() && FPaths::FileExists(Bundled))
	{
		return Bundled;
	}

	const FString Exe = PLATFORM_WINDOWS ? TEXT("lore.exe") : TEXT("lore");
#if PLATFORM_WINDOWS
	const FString UserBin = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE")), TEXT("bin"), Exe);
	if (FPaths::FileExists(UserBin))
	{
		return UserBin;
	}
#endif

	// Otherwise let the operating system resolve it from PATH.
	return Exe;
}

FString MakeBinaryPathRelativeToPlugin(const FString& InAbsolute)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LoreSourceControl"));
	if (!Plugin.IsValid())
	{
		return InAbsolute;
	}
	FString Relative = InAbsolute;
	FString Base = Plugin->GetBaseDir();
	FPaths::NormalizeFilename(Relative);
	FPaths::NormalizeDirectoryName(Base);
	if (FPaths::MakePathRelativeTo(Relative, *(Base / TEXT(""))) && !Relative.StartsWith(TEXT("..")))
	{
		return Relative;
	}
	return InAbsolute;
}

FString ResolveBinaryPath(const FString& InStored)
{
	if (InStored.IsEmpty())
	{
		return InStored;
	}
	// A bare filename resolves from PATH.
	if (!InStored.Contains(TEXT("/")) && !InStored.Contains(TEXT("\\")))
	{
		return InStored;
	}
	if (FPaths::IsRelative(InStored))
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LoreSourceControl"));
		if (Plugin.IsValid())
		{
			return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir(), InStored);
		}
	}
	return InStored;
}

FString GetPinnedVersion()
{
	return LoreSourceControlConstants::PinnedVersion;
}

bool CheckLoreAvailability(const FString& InPathToLoreBinary, FString& OutVersion)
{
	FString Results, Errors;
	const bool bOk = RunCommandInternalRaw(TEXT("--version"), InPathToLoreBinary, FString(), TArray<FString>(), TArray<FString>(), Results, Errors);
	OutVersion = Results.TrimStartAndEnd();
	return bOk && OutVersion.StartsWith(TEXT("lore"));
}

bool GetRepositoryRoot(const FString& InPathToProjectDir, FString& OutRepositoryRoot)
{
	FString Directory = FPaths::ConvertRelativePathToFull(InPathToProjectDir);
	for (int32 Guard = 0; Guard < 64 && !Directory.IsEmpty(); ++Guard)
	{
		if (FPaths::DirectoryExists(Directory / TEXT(".lore")))
		{
			OutRepositoryRoot = Directory;
			return true;
		}

		const FString Parent = FPaths::GetPath(Directory);
		if (Parent == Directory)
		{
			break;
		}

		Directory = Parent;
	}
	return false;
}

void GetRepositoryConfig(const FString& InRepositoryRoot, FString& OutRemoteUrl, FString& OutIdentity)
{
	const FString ConfigPath = InRepositoryRoot / TEXT(".lore") / TEXT("config.toml");
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ConfigPath))
	{
		return;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.StartsWith(TEXT("[")))
		{
			// The remote_url and identity are top-level keys, so stop at the first table.
			break;
		}

		FString Key, Value;
		if (Line.Split(TEXT("="), &Key, &Value))
		{
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			Value.RemoveFromStart(TEXT("\""));
			Value.RemoveFromEnd(TEXT("\""));
			if (Key == TEXT("remote_url"))
			{
				OutRemoteUrl = Value;
			}
			else if (Key == TEXT("identity"))
			{
				OutIdentity = Value;
			}
		}
	}
}

bool GetBranchName(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
	TArray<FString> Results, Errors;
	if (!RunCommand(TEXT("status"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, Errors))
	{
		return false;
	}

	for (const FString& Line : Results)
	{
		if (Line.StartsWith(TEXT("On branch ")))
		{
			FString Rest = Line.RightChop(10);
			int32 SpaceIndex = INDEX_NONE;
			if (Rest.FindChar(TEXT(' '), SpaceIndex))
			{
				Rest = Rest.Left(SpaceIndex);
			}

			OutBranchName = Rest.TrimStartAndEnd();
			return true;
		}
	}
	return false;
}

bool RunCommand(const FString& InCommand, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	FString Results, Errors;
	const bool bOk = RunCommandInternalRaw(InCommand, InPathToLoreBinary, InRepositoryRoot, InParameters, InFiles, Results, Errors);
	SplitLines(Results, OutResults);
	TArray<FString> ErrorLines;
	SplitLines(Errors, ErrorLines);
	OutErrorMessages.Append(ErrorLines);
	if (!bOk && ErrorLines.Num() == 0)
	{
		// On a silent failure, give the user something actionable.
		OutErrorMessages.Add(FString::Printf(TEXT("The lore command '%s' failed with no output. Confirm the Lore CLI path in the Lore settings and that the server is reachable."), *InCommand));
	}

	return bOk;
}

bool RunUpdateStatus(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates)
{
	// The editor queries content outside the working tree; Lore rejects out-of-tree paths, so only in-tree paths reach the client and the rest read as not controlled.
	FString RootFull = FPaths::ConvertRelativePathToFull(InRepositoryRoot);
	RootFull.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!RootFull.EndsWith(TEXT("/")))
	{
		RootFull += TEXT("/");
	}
	auto IsInTree = [&RootFull](const FString& InFile) -> bool
	{
		if (FPaths::IsRelative(InFile))
		{
			return true;
		}
		FString Full = InFile;
		Full.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Full.StartsWith(RootFull, ESearchCase::IgnoreCase);
	};

	TArray<FString> InTreeFiles;
	for (const FString& InFile : InFiles)
	{
		if (IsInTree(InFile))
		{
			InTreeFiles.Add(InFile);
		}
	}
	const TArray<FString> RelativeFiles = RelativeFilenames(InTreeFiles, InRepositoryRoot);

	TMap<FString, ELoreWorkingCopyState::Type> Parsed;
	TMap<FString, FString> Locks;
	bool bBehindRemote = false;
	bool bOk = true;
	if (InTreeFiles.Num() > 0)
	{
		TArray<FString> Parameters;
		Parameters.Add(TEXT("--scan"));
		TArray<FString> Results;
		bOk = RunCommand(TEXT("status"), InPathToLoreBinary, InRepositoryRoot, Parameters, RelativeFiles, Results, OutErrorMessages);
		ParseStatusResults(Results, Parsed);

		// The status header is the only sync signal: behind remote means not at the latest revision.
		for (const FString& Line : Results)
		{
			if (Line.Contains(TEXT("behind remote")))
			{
				bBehindRemote = true;
				break;
			}
		}

		if (bInUsingLocking)
		{
			TArray<FString> LockResults, LockErrors;
			RunCommand(TEXT("lock status"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), RelativeFiles, LockResults, LockErrors);
			ParseLockResults(LockResults, Locks);
		}
	}

	for (int32 Index = 0; Index < InFiles.Num(); ++Index)
	{
		const FString Absolute = FPaths::ConvertRelativePathToFull(InFiles[Index]);

		FLoreSourceControlState State(Absolute);
		State.bUsingLocking = bInUsingLocking;
		State.bNewerVersionOnServer = bBehindRemote;
		State.TimeStamp = FDateTime::Now();

		if (!IsInTree(InFiles[Index]))
		{
			State.WorkingCopyState = ELoreWorkingCopyState::NotControlled;
			OutStates.Add(MoveTemp(State));
			continue;
		}

		FString Relative = RelativeFilename(InFiles[Index], InRepositoryRoot);
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (const ELoreWorkingCopyState::Type* Found = Parsed.Find(Relative))
		{
			State.WorkingCopyState = *Found;
		}
		else if (FPaths::FileExists(Absolute))
		{
			State.WorkingCopyState = ELoreWorkingCopyState::Unchanged;
		}
		else
		{
			State.WorkingCopyState = ELoreWorkingCopyState::Unknown;
		}

		if (bInUsingLocking)
		{
			if (const FString* Owner = Locks.Find(Relative))
			{
				// With authentication off the owner reads <unknown>; treat an unattributable lock as this user's, not a block.
				const bool bMine = Owner->IsEmpty() || Owner->Equals(TEXT("<unknown>"), ESearchCase::IgnoreCase) || (!InIdentity.IsEmpty() && Owner->Equals(InIdentity, ESearchCase::IgnoreCase));
				State.LockState = bMine ? ELoreLockState::Locked : ELoreLockState::LockedOther;
				State.LockUser = *Owner;
			}
			else
			{
				State.LockState = ELoreLockState::NotLocked;
			}
		}

		OutStates.Add(MoveTemp(State));
	}

	return bOk;
}

bool RunGetHistory(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, TLoreSourceControlHistory& OutHistory)
{
	const FString RelativeFile = RelativeFilename(InFile, InRepositoryRoot);
	TArray<FString> Results;
	if (!RunCommand(TEXT("file history"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), { RelativeFile }, Results, OutErrorMessages))
	{
		return false;
	}

	const FString Absolute = FPaths::ConvertRelativePathToFull(InFile);
	TSharedPtr<FLoreSourceControlRevision, ESPMode::ThreadSafe> Current;
	FString PendingAction;

	for (const FString& Line : Results)
	{
		// "<LETTER> <path>" begins a revision block and carries its action.
		if (Line.Len() >= 3 && Line[1] == TEXT(' ') && FChar::IsUpper(Line[0]) && !Line.Contains(TEXT(" : ")))
		{
			PendingAction = FString::Chr(Line[0]);
			continue;
		}

		if (Line.StartsWith(TEXT("Revision")))
		{
			if (Current.IsValid())
			{
				OutHistory.Add(Current.ToSharedRef());
			}

			Current = MakeShared<FLoreSourceControlRevision, ESPMode::ThreadSafe>();
			Current->Filename = Absolute;
			Current->PathToLoreBinary = InPathToLoreBinary;
			Current->PathToRepositoryRoot = InRepositoryRoot;
			Current->Action = PendingAction;
			FString Value;
			if (Line.Split(TEXT(":"), nullptr, &Value))
			{
				Current->RevisionNumber = FCString::Atoi(*Value.TrimStartAndEnd());
			}
			continue;
		}

		if (!Current.IsValid())
		{
			continue;
		}
		FString Value;
		if (Line.StartsWith(TEXT("Signature")) && Line.Split(TEXT(":"), nullptr, &Value))
		{
			Current->CommitId = Value.TrimStartAndEnd();
			Current->ShortCommitId = Current->CommitId.Left(8);
		}
		else if (Line.StartsWith(TEXT("Date")) && Line.Split(TEXT(":"), nullptr, &Value))
		{
			ParseLoreDate(Value.TrimStartAndEnd(), Current->Date);
		}
		else if (Line.StartsWith(TEXT("Creator")) && Line.Split(TEXT(":"), nullptr, &Value))
		{
			Current->UserName = Value.TrimStartAndEnd();
		}
		else if (Line.StartsWith(TEXT(" ")) && !Line.Contains(TEXT(":")))
		{
			Current->Description = Line.TrimStartAndEnd();
		}
	}

	if (Current.IsValid())
	{
		OutHistory.Add(Current.ToSharedRef());
	}
	return true;
}

bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates)
{
	check(IsInGameThread());
	FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
	bool bChanged = false;
	for (const FLoreSourceControlState& InState : InStates)
	{
		TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> Cached = Provider.GetStateInternal(InState.LocalFilename);
		if (Cached->WorkingCopyState != InState.WorkingCopyState || Cached->LockState != InState.LockState || Cached->LockUser != InState.LockUser || Cached->bNewerVersionOnServer != InState.bNewerVersionOnServer)
		{
			bChanged = true;
		}

		Cached->WorkingCopyState = InState.WorkingCopyState;
		Cached->LockState = InState.LockState;
		Cached->LockUser = InState.LockUser;
		Cached->bUsingLocking = InState.bUsingLocking;
		Cached->bNewerVersionOnServer = InState.bNewerVersionOnServer;
#if LORE_UE5_3_OR_LATER
		Cached->PendingResolveInfo = InState.PendingResolveInfo;
#endif
		Cached->TimeStamp = InState.TimeStamp;
		if (InState.History.Num() > 0)
		{
			Cached->History = InState.History;
		}
	}
	return bChanged;
}

} // namespace LoreSourceControlUtils
