// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlUtils.h"
#include "LoreSourceControlState.h"
#include "LoreSourceControlRevision.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "LoreSourceControlLog.h"
#include "LoreSourceControlSettings.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if LORE_WITH_LIBLORE
#include "LoreClient.h"
#endif

namespace LoreSourceControlConstants
{
	// Validated client version
	const TCHAR* PinnedVersion = TEXT("0.8.3");
}

// The Lore --json event vocabulary in one place, so a mistyped key fails the build instead of parsing to nothing at runtime.
namespace LoreJson
{
	// Event envelope: every line is { "tagName": <event>, "data": { ... } }.
	const FString TagName = TEXT("tagName");
	const FString Data = TEXT("data");

	// Values the envelope "tagName" takes.
	namespace Tags
	{
		const FString RepositoryStatusRevision = TEXT("repositoryStatusRevision");
		const FString RepositoryStatusFile = TEXT("repositoryStatusFile");
		const FString LockFileStatus = TEXT("lockFileStatus");
		const FString FileHistory = TEXT("fileHistory");
		const FString Metadata = TEXT("metadata");
	}

	// repositoryStatusRevision fields.
	const FString IsRemoteAhead = TEXT("isRemoteAhead");
	const FString RemoteAvailable = TEXT("remoteAvailable");
	const FString RemoteAuthorized = TEXT("remoteAuthorized");

	// repositoryStatusFile fields.
	const FString Type = TEXT("type");
	const FString TypeDirectory = TEXT("directory");
	const FString Path = TEXT("path");
	const FString Action = TEXT("action");
	const FString FlagStaged = TEXT("flagStaged");
	const FString FlagDirty = TEXT("flagDirty");
	const FString FlagConflict = TEXT("flagConflict");
	const FString FlagConflictUnresolved = TEXT("flagConflictUnresolved");

	// Values the "action" field takes.
	namespace ActionValues
	{
		const FString Add = TEXT("add");
		const FString Keep = TEXT("keep");
		const FString Delete = TEXT("delete");
		const FString Move = TEXT("move");
		const FString Copy = TEXT("copy");
	}

	// lockFileStatus fields; "path" is shared with the status file event.
	const FString Owner = TEXT("owner");

	// fileHistory fields; "action" is shared with the status file event.
	const FString Revision = TEXT("revision");
	const FString RevisionNumber = TEXT("revisionNumber");

	// metadata fields; each value is itself a tagged object whose "data" carries the payload.
	const FString Key = TEXT("key");
	const FString Value = TEXT("value");

	// Values the metadata "key" takes.
	namespace MetadataKeys
	{
		const FString Timestamp = TEXT("timestamp");
		const FString Message = TEXT("message");
		const FString CommittedBy = TEXT("committed-by");
		const FString CreatedBy = TEXT("created-by");
	}
}

// The .lore/config.toml keys the connect path reads.
namespace LoreConfig
{
	const FString RemoteUrl = TEXT("remote_url");
	const FString Identity = TEXT("identity");
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

	// Whether this mode attempts the native library. Only possible when liblore is compiled in.
	bool UseNativeBackend(ELoreBackendMode InMode)
	{
#if LORE_WITH_LIBLORE
		return InMode != ELoreBackendMode::CommandLine;
#else
		return false;
#endif
	}

	// Whether a failed native call retries on the command line. Only Automatic does.
	bool FallsBackToCommandLine(ELoreBackendMode InMode)
	{
		return InMode == ELoreBackendMode::Automatic;
	}

	void SplitLines(const FString& InText, TArray<FString>& OutLines)
	{
		InText.ParseIntoArrayLines(OutLines, false);
		for (FString& Line : OutLines)
		{
			Line.TrimEndInline();
		}
	}

	// Lore date shape: "Thu, 18 Jun 2026 07:27:40 +0000"
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

	bool ParseJsonEvent(const FString& InLine, FString& OutTag, TSharedPtr<FJsonObject>& OutData)
	{
		const FString Trimmed = InLine.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed[0] != TEXT('{'))
		{
			return false;
		}
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return false;
		}
		if (!Object->TryGetStringField(LoreJson::TagName, OutTag))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* DataField = nullptr;
		if (Object->TryGetObjectField(LoreJson::Data, DataField) && DataField != nullptr)
		{
			OutData = *DataField;
		}
		return true;
	}

	// "keep" means tracked, so dirtiness decides modified versus unchanged.
	ELoreWorkingCopyState::Type StatusStateFromEvent(const FString& InAction, bool bStaged, bool bDirty, bool bConflict)
	{
		if (bConflict)
		{
			return ELoreWorkingCopyState::Conflicted;
		}
		if (InAction == LoreJson::ActionValues::Add)
		{
			// A staged add is marked for add; an unstaged one is still untracked.
			return bStaged ? ELoreWorkingCopyState::Added : ELoreWorkingCopyState::NotControlled;
		}
		if (InAction == LoreJson::ActionValues::Delete)
		{
			return ELoreWorkingCopyState::Deleted;
		}
		if (InAction == LoreJson::ActionValues::Move)
		{
			return ELoreWorkingCopyState::Moved;
		}
		if (InAction == LoreJson::ActionValues::Copy)
		{
			return ELoreWorkingCopyState::Copied;
		}
		return bDirty ? ELoreWorkingCopyState::Modified : ELoreWorkingCopyState::Unchanged;
	}

	// Skip build output and tool folders that should never be source controlled, so a repository missing a .loreignore does not flood the submit dialog.
	bool IsTransientRepoPath(const FString& InPath)
	{
		static const TSet<FString> Transient = { TEXT("Binaries"), TEXT("Intermediate"), TEXT("Saved"), TEXT("DerivedDataCache"), TEXT(".git"), TEXT(".lore"), TEXT(".vs"), TEXT(".idea") };
		TArray<FString> Segments;
		InPath.ParseIntoArray(Segments, TEXT("/"), true);
		for (const FString& Segment : Segments)
		{
			if (Transient.Contains(Segment))
			{
				return true;
			}
		}
		return false;
	}

	void ParseStatusResults(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates, bool& bOutRemoteAhead)
	{
		bOutRemoteAhead = false;
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid())
			{
				continue;
			}

			if (Tag == LoreJson::Tags::RepositoryStatusRevision)
			{
				int32 RemoteAhead = 0;
				Data->TryGetNumberField(LoreJson::IsRemoteAhead, RemoteAhead);
				bOutRemoteAhead = RemoteAhead != 0;
				continue;
			}

			if (Tag != LoreJson::Tags::RepositoryStatusFile)
			{
				continue;
			}

			FString Type;
			Data->TryGetStringField(LoreJson::Type, Type);
			if (Type == LoreJson::TypeDirectory)
			{
				continue;
			}

			FString Path;
			if (!Data->TryGetStringField(LoreJson::Path, Path) || Path.IsEmpty())
			{
				continue;
			}
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (IsTransientRepoPath(Path))
			{
				continue;
			}

			FString Action;
			Data->TryGetStringField(LoreJson::Action, Action);
			bool bStaged = false, bDirty = false, bConflict = false, bConflictUnresolved = false;
			Data->TryGetBoolField(LoreJson::FlagStaged, bStaged);
			Data->TryGetBoolField(LoreJson::FlagDirty, bDirty);
			Data->TryGetBoolField(LoreJson::FlagConflict, bConflict);
			Data->TryGetBoolField(LoreJson::FlagConflictUnresolved, bConflictUnresolved);

			// A later event for the same path takes precedence, matching the duplication the scan emits.
			OutStates.Add(Path, StatusStateFromEvent(Action, bStaged, bDirty, bConflict || bConflictUnresolved));
		}
	}

	void ParseLockResults(const TArray<FString>& InResults, TMap<FString, FString>& OutLocks)
	{
		// `lockFileStatus` events carry the local cache before any remote-authentication error, so owners parse even offline.
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid())
			{
				continue;
			}
			if (Tag != LoreJson::Tags::LockFileStatus)
			{
				continue;
			}
			FString Path;
			if (!Data->TryGetStringField(LoreJson::Path, Path))
			{
				continue;
			}
			FString Owner;
			Data->TryGetStringField(LoreJson::Owner, Owner);
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			OutLocks.Add(Path, Owner);
		}
	}

	// The reachability and authorization flags live only on the status revision header, not the per-file events.
	void ParseRemoteAuthResults(const TArray<FString>& InResults, bool& bOutRemoteAvailable, bool& bOutRemoteAuthorized)
	{
		bOutRemoteAvailable = false;
		bOutRemoteAuthorized = false;
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid() || Tag != LoreJson::Tags::RepositoryStatusRevision)
			{
				continue;
			}

			int32 Available = 0, Authorized = 0;
			Data->TryGetNumberField(LoreJson::RemoteAvailable, Available);
			Data->TryGetNumberField(LoreJson::RemoteAuthorized, Authorized);
			bOutRemoteAvailable = Available != 0;
			bOutRemoteAuthorized = Authorized != 0;
			return;
		}
	}
}

namespace LoreSourceControlUtils
{

// Defined further down; forward-declared for the liblore history converter that appears earlier in the file.
FString LoreActionToString(const FString& InAction);

void ParseStatusForTest(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates, bool& bOutRemoteAhead)
{
	ParseStatusResults(InResults, OutStates, bOutRemoteAhead);
}

#if LORE_WITH_LIBLORE
void ParseStatusFromClient(const LoreClient::FStatusResult& InResult, TMap<FString, ELoreWorkingCopyState::Type>& OutStates, bool& bOutRemoteAhead)
{
	bOutRemoteAhead = InResult.bRemoteAhead;
	for (const LoreClient::FStatusFile& File : InResult.Files)
	{
		if (File.bIsDirectory || File.Path.IsEmpty())
		{
			continue;
		}
		FString Path = File.Path;
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (IsTransientRepoPath(Path))
		{
			continue;
		}
		OutStates.Add(Path, StatusStateFromEvent(File.Action, File.bStaged, File.bDirty, File.bConflict));
	}
}

void ParseLocksFromClient(const TArray<LoreClient::FLockStatus>& InLocks, TMap<FString, FString>& OutLocks)
{
	for (const LoreClient::FLockStatus& Lock : InLocks)
	{
		if (Lock.Path.IsEmpty())
		{
			continue;
		}
		FString Path = Lock.Path;
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		OutLocks.Add(Path, Lock.Owner);
	}
}

void BuildHistoryFromClient(ELoreBackendMode InMode, const TArray<LoreClient::FHistoryRevision>& InRevisions, const FString& InAbsoluteFile, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TLoreSourceControlHistory& OutHistory, bool bRemoteReachable)
{
	for (const LoreClient::FHistoryRevision& In : InRevisions)
	{
		TSharedRef<FLoreSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeShared<FLoreSourceControlRevision, ESPMode::ThreadSafe>();
		Revision->Filename = InAbsoluteFile;
		Revision->PathToLoreBinary = InPathToLoreBinary;
		Revision->PathToRepositoryRoot = InRepositoryRoot;
		Revision->BackendMode = InMode;
		Revision->CommitId = In.CommitId;
		Revision->ShortCommitId = In.CommitId.Left(8);
		Revision->RevisionNumber = In.RevisionNumber;
		Revision->Action = LoreActionToString(In.Action);
		Revision->Description = In.Description;
		Revision->UserName = In.UserName;
		Revision->Date = In.Date;
		Revision->FileSize = In.FileSize;
		Revision->ContentAddress = In.Address;

		// Mark a revision whose content is in neither the local store nor the reachable remote, since its diff cannot extract it. After a push the bytes live only on the remote, so the probe must consult it; with the remote down the probe is local-only and pushed content reads unavailable, which is exactly what an offline diff would find.
		if (!In.Address.IsEmpty())
		{
			bool bAvailable = false;
			FString ProbeError;
			if (LoreClient::QueryContentAvailable(InRepositoryRoot, In.Address, bAvailable, ProbeError, bRemoteReachable) && !bAvailable)
			{
				Revision->Description = NSLOCTEXT("LoreSourceControl", "RevisionContentUnavailable", "[Unavailable] ").ToString() + Revision->Description;
			}
		}

		OutHistory.Add(Revision);
	}
}
#endif

void ParseLocksForTest(const TArray<FString>& InResults, TMap<FString, FString>& OutLocks)
{
	ParseLockResults(InResults, OutLocks);
}

void ParseRemoteAuthForTest(const TArray<FString>& InResults, bool& bOutRemoteAvailable, bool& bOutRemoteAuthorized)
{
	ParseRemoteAuthResults(InResults, bOutRemoteAvailable, bOutRemoteAuthorized);
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
	// A bundled copy takes precedence over a system install.
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

bool CheckLoreAvailability(ELoreBackendMode InMode, const FString& InPathToLoreBinary, FString& OutVersion)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		// The library reports its own version; a non-empty answer means it loaded and runs.
		OutVersion = LoreClient::GetVersion();
		return !OutVersion.IsEmpty();
	}
#endif
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
			if (Key == LoreConfig::RemoteUrl)
			{
				OutRemoteUrl = Value;
			}
			else if (Key == LoreConfig::Identity)
			{
				OutIdentity = Value;
			}
		}
	}
}

bool GetBranchName(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		// The status revision header carries the branch, so read it with no tree scan.
		LoreClient::FStatusResult Result;
		FString Error;
		if (LoreClient::GetStatus(InRepositoryRoot, FString(), TArray<FString>(), false, Result, Error))
		{
			OutBranchName = Result.BranchName;
			return !OutBranchName.IsEmpty();
		}
		return false;
	}
#endif
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

void LaunchLogin(const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRemoteUrl)
{
	// Login is interactive and opens a browser, so launch it detached and do not wait on it.
	const FString Params = InRemoteUrl.IsEmpty() ? TEXT("auth login") : FString::Printf(TEXT("auth login %s"), *InRemoteUrl);
	const TCHAR* WorkingDirectory = InRepositoryRoot.IsEmpty() ? nullptr : *InRepositoryRoot;
	FProcHandle Handle = FPlatformProcess::CreateProc(*InPathToLoreBinary, *Params, true, false, false, nullptr, 0, WorkingDirectory, nullptr);
	if (Handle.IsValid())
	{
		FPlatformProcess::CloseProc(Handle);
	}
}

bool LoginWithToken(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRemoteUrl, const FString& InToken, const FString& InTokenType, FString& OutError)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		LoreClient::FAuthUserInfo User;
		if (LoreClient::LoginWithToken(InRepositoryRoot, InRemoteUrl, FString(), InToken, InTokenType, User, OutError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore token login failed, falling back to the CLI: %s"), *OutError);
	}
#endif
	TArray<FString> Results, Errors;
	const TArray<FString> Parameters = { TEXT("--token-type"), InTokenType, TEXT("--token"), InToken };
	const bool bOk = RunCommand(TEXT("auth login"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, Errors);
	if (!bOk)
	{
		OutError = Errors.Num() ? FString::Join(Errors, TEXT("; ")) : TEXT("token login failed");
	}
	return bOk;
}

bool LoginInteractive(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRemoteUrl, FString& OutError)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString AuthUrl;
		LoreClient::FAuthUserInfo User;
		if (LoreClient::LoginInteractive(InRepositoryRoot, InRemoteUrl, false, AuthUrl, User, OutError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore interactive login failed, falling back to the CLI: %s"), *OutError);
	}
#endif
	// The command-line browser login is detached and gives no synchronous result, so report it as launched.
	LaunchLogin(InPathToLoreBinary, InRepositoryRoot, InRemoteUrl);
	return true;
}

bool Logout(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FString& OutError)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		if (LoreClient::Logout(InRepositoryRoot, FString(), FString(), OutError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore logout failed, falling back to the CLI: %s"), *OutError);
	}
#endif
	TArray<FString> Results, Errors;
	const bool bOk = RunCommand(TEXT("auth logout"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, Errors);
	if (!bOk)
	{
		OutError = Errors.Num() ? FString::Join(Errors, TEXT("; ")) : TEXT("logout failed");
	}
	return bOk;
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
		OutErrorMessages.Add(FText::Format(NSLOCTEXT("LoreSourceControl", "CommandNoOutput", "The lore command '{0}' failed with no output. Confirm the Lore CLI path in the Lore settings and that the server is reachable."), FText::FromString(InCommand)).ToString());
	}

	return bOk;
}

void GetRemoteAuthState(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, bool& bOutRemoteAvailable, bool& bOutRemoteAuthorized)
{
	bOutRemoteAvailable = false;
	bOutRemoteAuthorized = false;
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		LoreClient::FStatusResult Result;
		FString Error;
		if (LoreClient::GetStatus(InRepositoryRoot, FString(), TArray<FString>(), false, Result, Error))
		{
			bOutRemoteAvailable = Result.bRemoteAvailable;
			bOutRemoteAuthorized = Result.bRemoteAuthorized;
		}
		return;
	}
#endif
	// Plain status, no --scan: a light read of the revision header flags, not a tree walk that persists dirty state.
	TArray<FString> Results, Errors;
	if (!RunCommand(TEXT("status"), InPathToLoreBinary, InRepositoryRoot, { TEXT("--json") }, TArray<FString>(), Results, Errors))
	{
		return;
	}

	ParseRemoteAuthResults(Results, bOutRemoteAvailable, bOutRemoteAuthorized);
}

bool RunUpdateStatus(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates)
{
	// The editor queries content outside the working tree; Lore rejects out-of-tree paths, so only in-tree paths reach the client and the rest read as Unknown, never offered for add.
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
		bool bStatusRead = false;
#if LORE_WITH_LIBLORE
		if (UseNativeBackend(InMode))
		{
			LoreClient::FStatusResult ClientResult;
			FString ClientError;
			if (LoreClient::GetStatus(InRepositoryRoot, InIdentity, RelativeFiles, true, ClientResult, ClientError))
			{
				ParseStatusFromClient(ClientResult, Parsed, bBehindRemote);
				bStatusRead = true;
			}
			else if (!FallsBackToCommandLine(InMode))
			{
				// Native mode does not retry on the command line; report the failure and stop.
				OutErrorMessages.Add(ClientError);
				bOk = false;
				bStatusRead = true;
			}
			else
			{
				UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore status failed, falling back to the CLI: %s"), *ClientError);
			}
		}
#endif
		if (!bStatusRead)
		{
			TArray<FString> Parameters;
			Parameters.Add(TEXT("--scan"));
			Parameters.Add(TEXT("--json"));
			TArray<FString> Results;
			bOk = RunCommand(TEXT("status"), InPathToLoreBinary, InRepositoryRoot, Parameters, RelativeFiles, Results, OutErrorMessages);
			ParseStatusResults(Results, Parsed, bBehindRemote);
		}

		if (bInUsingLocking)
		{
			bool bLocksRead = false;
#if LORE_WITH_LIBLORE
			if (UseNativeBackend(InMode))
			{
				TArray<LoreClient::FLockStatus> ClientLocks;
				FString LockError;
				if (LoreClient::GetLockStatus(InRepositoryRoot, RelativeFiles, ClientLocks, LockError))
				{
					ParseLocksFromClient(ClientLocks, Locks);
					bLocksRead = true;
				}
				else if (!FallsBackToCommandLine(InMode))
				{
					// Native mode does not retry on the command line; lock owners stay unknown for this pass.
					UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore lock status failed in native mode: %s"), *LockError);
					bLocksRead = true;
				}
				else
				{
					UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore lock status failed, falling back to the CLI: %s"), *LockError);
				}
			}
#endif
			if (!bLocksRead)
			{
				TArray<FString> LockResults, LockErrors;
				RunCommand(TEXT("lock status"), InPathToLoreBinary, InRepositoryRoot, { TEXT("--json") }, RelativeFiles, LockResults, LockErrors);
				ParseLockResults(LockResults, Locks);
			}
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
			// An out-of-tree file is not part of this repository. The submit dialog offers any not-controlled file for add, so engine and plugin content must read Unknown to stay out of that list.
			State.WorkingCopyState = ELoreWorkingCopyState::Unknown;
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

bool CollectCheckInBlockers(const TArray<FLoreSourceControlState>& InStates, bool bAllowOverLock, TArray<FString>& OutMessages)
{
	// A behind-the-remote file is allowed (the commit is local; a rejected push is reported). A foreign lock blocks unless the caller forces over it. An unresolved conflict always blocks.
	for (const FLoreSourceControlState& State : InStates)
	{
		if (!bAllowOverLock && State.LockState == ELoreLockState::LockedOther)
		{
			OutMessages.Add(FText::Format(NSLOCTEXT("LoreSourceControl", "BlockerLocked", "Cannot submit {0}: locked by {1}."), FText::FromString(FPaths::GetCleanFilename(State.GetFilename())), FText::FromString(State.LockUser)).ToString());
		}
		else if (State.IsConflicted())
		{
			OutMessages.Add(FText::Format(NSLOCTEXT("LoreSourceControl", "BlockerConflict", "Cannot submit {0}: unresolved conflict. Resolve, then submit."), FText::FromString(FPaths::GetCleanFilename(State.GetFilename()))).ToString());
		}
	}

	return OutMessages.Num() > 0;
}

FString LoreActionToString(const FString& InAction)
{
	// Labels are localizable text; they convert to a string at the ISourceControlRevision::GetAction boundary, which returns a string reference.
	static const TMap<FString, FText> Labels = {
		{ LoreJson::ActionValues::Add,    NSLOCTEXT("LoreSourceControl", "HistoryActionAdd", "Add") },
		{ LoreJson::ActionValues::Keep,   NSLOCTEXT("LoreSourceControl", "HistoryActionKeep", "Keep (Edit)") }, // Lore keeps an edited-but-present file as "keep"
		{ LoreJson::ActionValues::Delete, NSLOCTEXT("LoreSourceControl", "HistoryActionDelete", "Delete") },
		{ LoreJson::ActionValues::Move,   NSLOCTEXT("LoreSourceControl", "HistoryActionMove", "Move") },
		{ LoreJson::ActionValues::Copy,   NSLOCTEXT("LoreSourceControl", "HistoryActionCopy", "Copy") },
	};
	const FText* Label = Labels.Find(InAction);
	return Label ? Label->ToString() : InAction;
}

bool RunGetHistory(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InFile, TArray<FString>& OutErrorMessages, TLoreSourceControlHistory& OutHistory, bool bRemoteReachable)
{
	const FString RelativeFile = RelativeFilename(InFile, InRepositoryRoot);

#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		TArray<LoreClient::FHistoryRevision> ClientRevisions;
		FString ClientError;
		if (LoreClient::GetHistory(InRepositoryRoot, RelativeFile, ClientRevisions, ClientError))
		{
			BuildHistoryFromClient(InMode, ClientRevisions, FPaths::ConvertRelativePathToFull(InFile), InPathToLoreBinary, InRepositoryRoot, OutHistory, bRemoteReachable);
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore file history failed, falling back to the CLI: %s"), *ClientError);
	}
#endif

	TArray<FString> Results;
	if (!RunCommand(TEXT("file history"), InPathToLoreBinary, InRepositoryRoot, { TEXT("--json") }, { RelativeFile }, Results, OutErrorMessages))
	{
		return false;
	}

	const FString Absolute = FPaths::ConvertRelativePathToFull(InFile);
	TSharedPtr<FLoreSourceControlRevision, ESPMode::ThreadSafe> Current;

	for (const FString& Line : Results)
	{
		FString Tag;
		TSharedPtr<FJsonObject> Data;
		if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid())
		{
			continue;
		}

		if (Tag == LoreJson::Tags::FileHistory)
		{
			// A new revision begins; the preceding one is complete.
			if (Current.IsValid())
			{
				OutHistory.Add(Current.ToSharedRef());
			}

			Current = MakeShared<FLoreSourceControlRevision, ESPMode::ThreadSafe>();
			Current->Filename = Absolute;
			Current->PathToLoreBinary = InPathToLoreBinary;
			Current->PathToRepositoryRoot = InRepositoryRoot;
			Current->BackendMode = InMode;
			FString Revision;
			Data->TryGetStringField(LoreJson::Revision, Revision);
			Current->CommitId = Revision;
			Current->ShortCommitId = Revision.Left(8);
			int32 RevisionNumber = 0;
			Data->TryGetNumberField(LoreJson::RevisionNumber, RevisionNumber);
			Current->RevisionNumber = RevisionNumber;
			FString Action;
			Data->TryGetStringField(LoreJson::Action, Action);
			Current->Action = LoreActionToString(Action);
			continue;
		}

		if (Tag != LoreJson::Tags::Metadata || !Current.IsValid())
		{
			continue;
		}

		// Each metadata value is itself a tagged object whose "data" carries the payload.
		FString Key;
		Data->TryGetStringField(LoreJson::Key, Key);
		const TSharedPtr<FJsonObject>* ValueField = nullptr;
		if (!Data->TryGetObjectField(LoreJson::Value, ValueField) || ValueField == nullptr)
		{
			continue;
		}
		if (Key == LoreJson::MetadataKeys::Timestamp)
		{
			double Milliseconds = 0.0;
			(*ValueField)->TryGetNumberField(LoreJson::Data, Milliseconds);
			Current->Date = FDateTime::FromUnixTimestamp(static_cast<int64>(Milliseconds / 1000.0));
		}
		else if (Key == LoreJson::MetadataKeys::Message)
		{
			(*ValueField)->TryGetStringField(LoreJson::Data, Current->Description);
		}
		else if (Key == LoreJson::MetadataKeys::CommittedBy || (Key == LoreJson::MetadataKeys::CreatedBy && Current->UserName.IsEmpty()))
		{
			// Prefer the committer; the creator arrives first and seeds the field.
			(*ValueField)->TryGetStringField(LoreJson::Data, Current->UserName);
		}
	}

	if (Current.IsValid())
	{
		OutHistory.Add(Current.ToSharedRef());
	}
	return true;
}

bool StageFiles(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::Stage(InRepositoryRoot, InRelativeFiles, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore stage failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Results;
	return RunCommand(TEXT("stage"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), InRelativeFiles, Results, OutErrorMessages);
}

bool UnstageFiles(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::Unstage(InRepositoryRoot, InRelativeFiles, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore unstage failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Results;
	return RunCommand(TEXT("unstage"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), InRelativeFiles, Results, OutErrorMessages);
}

bool ResetFiles(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::Reset(InRepositoryRoot, InRelativeFiles, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore reset failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Results;
	return RunCommand(TEXT("reset"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), InRelativeFiles, Results, OutErrorMessages);
}

bool AcquireLocks(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::LockAcquire(InRepositoryRoot, InRelativeFiles, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore lock acquire failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Results;
	return RunCommand(TEXT("lock acquire"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), InRelativeFiles, Results, OutErrorMessages);
}

bool ReleaseLocks(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const TArray<FString>& InRelativeFiles, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::LockRelease(InRepositoryRoot, InRelativeFiles, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore lock release failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Results;
	return RunCommand(TEXT("lock release"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), InRelativeFiles, Results, OutErrorMessages);
}

bool CommitChanges(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, const FString& InMessage, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		// Commit is not idempotent: a partial liblore commit can record the revision before reporting failure, so a retry would mint a duplicate. Never fall back, even in Automatic; surface the error instead.
		FString ClientError;
		if (!LoreClient::Commit(InRepositoryRoot, InIdentity, InMessage, ClientError))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		return true;
	}
#endif
	// Keep the message safe inside the CLI's quoted argument; the native path takes it verbatim.
	FString Message = InMessage;
	Message.ReplaceInline(TEXT("\""), TEXT("'"));
	Message.ReplaceInline(TEXT("\r"), TEXT(" "));
	Message.ReplaceInline(TEXT("\n"), TEXT(" "));
	TArray<FString> CommitParameters;
	CommitParameters.Add(FString::Printf(TEXT("\"%s\""), *Message));
	if (!InIdentity.IsEmpty())
	{
		CommitParameters.Add(TEXT("--identity"));
		CommitParameters.Add(FString::Printf(TEXT("\"%s\""), *InIdentity));
	}
	TArray<FString> Results;
	return RunCommand(TEXT("commit"), InPathToLoreBinary, InRepositoryRoot, CommitParameters, TArray<FString>(), Results, OutErrorMessages);
}

bool PushBranch(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::Push(InRepositoryRoot, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore push failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Results;
	return RunCommand(TEXT("push"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, OutErrorMessages);
}

bool SyncWorkingTree(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		// Sync can auto-merge and auto-commit, so a partial liblore sync must not be retried against an already-advanced tree. Never fall back, even in Automatic; surface the error instead.
		FString ClientError;
		if (!LoreClient::Sync(InRepositoryRoot, ClientError))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		return true;
	}
#endif
	TArray<FString> Results;
	return RunCommand(TEXT("sync"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, OutErrorMessages);
}

bool WriteRevisionFile(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRelativeFile, const FString& InRevision, const FString& InOutputPath, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::WriteFile(InRepositoryRoot, InRelativeFile, InRevision, InOutputPath, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore file write failed, falling back to the CLI: %s"), *ClientError);
		// The client refuses to overwrite, so clear any partial output liblore left before the CLI retry.
		IFileManager::Get().Delete(*InOutputPath, false, true);
	}
#endif
	TArray<FString> Parameters;
	Parameters.Add(FString::Printf(TEXT("--revision %s"), *InRevision));
	Parameters.Add(FString::Printf(TEXT("--path \"%s\""), *InRelativeFile));
	Parameters.Add(FString::Printf(TEXT("--output \"%s\""), *InOutputPath));
	TArray<FString> Results;
	return RunCommand(TEXT("file write"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages);
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
