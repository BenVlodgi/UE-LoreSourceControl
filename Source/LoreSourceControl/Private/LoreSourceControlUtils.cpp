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
#include "HAL/PlatformFileManager.h"
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
		const FString RevisionHistoryEntry = TEXT("revisionHistoryEntry");
		const FString RevisionInfo = TEXT("revisionInfo");
		const FString RevisionInfoDelta = TEXT("revisionInfoDelta");
		const FString RevisionSyncFile = TEXT("revisionSyncFile");
		const FString BranchListEntry = TEXT("branchListEntry");
	}

	// repositoryStatusRevision fields.
	const FString IsRemoteAhead = TEXT("isRemoteAhead");
	const FString RemoteAvailable = TEXT("remoteAvailable");
	const FString RemoteAuthorized = TEXT("remoteAuthorized");
	const FString BranchName = TEXT("branchName");
	const FString RevisionRemote = TEXT("revisionRemote");
	const FString RevisionRemoteNumber = TEXT("revisionRemoteNumber");

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

	// revisionHistoryEntry fields; "revision" and "revisionNumber" are shared with the file history event.
	const FString Parent = TEXT("parent");

	// revisionInfoDelta fields; "path" and "action" are shared with the status file event.
	const FString Size = TEXT("size");
	const FString FlagModify = TEXT("flagModify");
	const FString FlagMerged = TEXT("flagMerged");
	const FString FlagFile = TEXT("flagFile");

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
		const FString Protect = TEXT("protect");
	}

	// branchListEntry fields; "name" is shared with other events. location is 0 for local, 1 for remote.
	const FString Location = TEXT("location");
	const FString Name = TEXT("name");
	const FString Category = TEXT("category");
	const FString Creator = TEXT("creator");
	const FString Latest = TEXT("latest");
	const FString Created = TEXT("created");
	const FString IsCurrent = TEXT("isCurrent");
	const FString Archived = TEXT("archived");
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

	void ParseStatusResults(const TArray<FString>& InResults, TMap<FString, ELoreWorkingCopyState::Type>& OutStates, bool& bOutRemoteAhead, bool& bOutRemoteAvailable)
	{
		bOutRemoteAhead = false;
		// Default to not reachable, so a status that emits no revision event (a failed or empty read) does not then run the server-side lock query.
		bOutRemoteAvailable = false;
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
				int32 RemoteAvail = 0;
				Data->TryGetNumberField(LoreJson::RemoteAvailable, RemoteAvail);
				bOutRemoteAvailable = RemoteAvail != 0;
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

	// Metadata values are tagged objects whose "data" carries the payload. timestamp, message, and committed-by fill the given fields; created-by seeds the author until the committer arrives.
	void ApplyMetadataEvent(const TSharedPtr<FJsonObject>& InData, FDateTime& OutDate, FString& OutMessage, FString& OutAuthor)
	{
		FString Key;
		InData->TryGetStringField(LoreJson::Key, Key);
		const TSharedPtr<FJsonObject>* ValueField = nullptr;
		if (!InData->TryGetObjectField(LoreJson::Value, ValueField) || ValueField == nullptr)
		{
			return;
		}
		if (Key == LoreJson::MetadataKeys::Timestamp)
		{
			double Milliseconds = 0.0;
			(*ValueField)->TryGetNumberField(LoreJson::Data, Milliseconds);
			OutDate = FDateTime::FromUnixTimestamp(static_cast<int64>(Milliseconds / 1000.0));
		}
		else if (Key == LoreJson::MetadataKeys::Message)
		{
			(*ValueField)->TryGetStringField(LoreJson::Data, OutMessage);
		}
		else if (Key == LoreJson::MetadataKeys::CommittedBy || (Key == LoreJson::MetadataKeys::CreatedBy && OutAuthor.IsEmpty()))
		{
			(*ValueField)->TryGetStringField(LoreJson::Data, OutAuthor);
		}
	}

	void ParseRevisionListResults(const TArray<FString>& InResults, TArray<FLoreRevisionListEntry>& OutRevisions)
	{
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid())
			{
				continue;
			}

			if (Tag == LoreJson::Tags::RevisionHistoryEntry)
			{
				FLoreRevisionListEntry Entry;
				Data->TryGetStringField(LoreJson::Revision, Entry.Hash);
				int32 Number = 0;
				Data->TryGetNumberField(LoreJson::RevisionNumber, Number);
				Entry.RevisionNumber = Number;
				const TArray<TSharedPtr<FJsonValue>>* Parents = nullptr;
				if (Data->TryGetArrayField(LoreJson::Parent, Parents) && Parents != nullptr)
				{
					if (Parents->Num() > 0) { (*Parents)[0]->TryGetString(Entry.ParentHash); }
					if (Parents->Num() > 1) { (*Parents)[1]->TryGetString(Entry.ParentOtherHash); }
				}
				OutRevisions.Add(MoveTemp(Entry));
			}
			else if (Tag == LoreJson::Tags::Metadata && OutRevisions.Num() > 0)
			{
				FLoreRevisionListEntry& Current = OutRevisions.Last();
				ApplyMetadataEvent(Data, Current.Date, Current.Message, Current.Author);
			}
		}
	}

	void ParseSyncDeltaResults(const TArray<FString>& InResults, TArray<FLoreDeltaEntry>& OutDelta)
	{
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid() || Tag != LoreJson::Tags::RevisionSyncFile)
			{
				continue;
			}
			// Directory entries carry no content to rewrite; flagFile marks a file.
			bool bIsFile = false;
			Data->TryGetBoolField(LoreJson::FlagFile, bIsFile);
			if (!bIsFile)
			{
				continue;
			}
			FLoreDeltaEntry Entry;
			if (!Data->TryGetStringField(LoreJson::Path, Entry.Path) || Entry.Path.IsEmpty())
			{
				continue;
			}
			Data->TryGetStringField(LoreJson::Action, Entry.Action);
			Entry.Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			OutDelta.Add(MoveTemp(Entry));
		}
	}

	// The reachability flags read for anchors reuse the header parse; the anchor hashes and numbers come off the same event.
	void ParseStatusAnchorsResults(const TArray<FString>& InResults, FLoreStatusAnchors& OutAnchors)
	{
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid() || Tag != LoreJson::Tags::RepositoryStatusRevision)
			{
				continue;
			}
			Data->TryGetStringField(LoreJson::BranchName, OutAnchors.BranchName);
			Data->TryGetStringField(LoreJson::Revision, OutAnchors.CurrentRevision);
			int32 Number = 0;
			Data->TryGetNumberField(LoreJson::RevisionNumber, Number);
			OutAnchors.CurrentRevisionNumber = Number;
			Data->TryGetStringField(LoreJson::RevisionRemote, OutAnchors.RemoteRevision);
			int32 RemoteNumber = 0;
			Data->TryGetNumberField(LoreJson::RevisionRemoteNumber, RemoteNumber);
			OutAnchors.RemoteRevisionNumber = RemoteNumber;
			return;
		}
	}

	// The all-zeros hash means no remote revision; treat it as empty so a marker records no target when the remote is unknown.
	bool IsZeroHash(const FString& InHex)
	{
		for (const TCHAR Character : InHex)
		{
			if (Character != TEXT('0'))
			{
				return false;
			}
		}
		return !InHex.IsEmpty();
	}

	void ParseRevisionInfoResults(const TArray<FString>& InResults, FLoreRevisionDetails& OutDetails)
	{
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid())
			{
				continue;
			}

			if (Tag == LoreJson::Tags::RevisionInfo)
			{
				Data->TryGetStringField(LoreJson::Revision, OutDetails.Hash);
				int32 Number = 0;
				Data->TryGetNumberField(LoreJson::RevisionNumber, Number);
				OutDetails.RevisionNumber = Number;
			}
			else if (Tag == LoreJson::Tags::Metadata)
			{
				ApplyMetadataEvent(Data, OutDetails.Date, OutDetails.Message, OutDetails.Author);
			}
			else if (Tag == LoreJson::Tags::RevisionInfoDelta)
			{
				FLoreRevisionDeltaEntry Delta;
				Data->TryGetStringField(LoreJson::Path, Delta.Path);
				Data->TryGetStringField(LoreJson::Action, Delta.Action);
				Data->TryGetNumberField(LoreJson::Size, Delta.Size);
				Data->TryGetBoolField(LoreJson::FlagModify, Delta.bModified);
				Data->TryGetBoolField(LoreJson::FlagMerged, Delta.bMerged);
				Data->TryGetBoolField(LoreJson::FlagFile, Delta.bIsFile);
				OutDetails.Deltas.Add(MoveTemp(Delta));
			}
		}
	}

	// The all-zeros hex latest a branch reports when it has no revision (an archived branch) reads as blank in the window, so treat it as empty. Reuses the IsZeroHash rule.
	FString NonZeroHashOrEmpty(const FString& InHex)
	{
		return IsZeroHash(InHex) ? FString() : InHex;
	}

	// Read each branchListEntry into one FLoreBranchItem per listing entry (local and remote arrive as separate entries); MergeBranchListEntries folds them by name afterward. bProtected is filled later per branch, so it stays false here.
	void ParseBranchListResults(const TArray<FString>& InResults, TArray<FLoreBranchItem>& OutEntries)
	{
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid() || Tag != LoreJson::Tags::BranchListEntry)
			{
				continue;
			}
			FLoreBranchItem Entry;
			Data->TryGetStringField(LoreJson::Name, Entry.Name);
			if (Entry.Name.IsEmpty())
			{
				continue;
			}
			Data->TryGetStringField(LoreJson::Category, Entry.Category);
			Data->TryGetStringField(LoreJson::Creator, Entry.Creator);
			int32 Location = 0;
			Data->TryGetNumberField(LoreJson::Location, Location);
			const bool bRemote = Location != 0;
			Entry.bHasLocal = !bRemote;
			Entry.bHasRemote = bRemote;
			FString Latest;
			Data->TryGetStringField(LoreJson::Latest, Latest);
			if (bRemote)
			{
				Entry.LatestRemote = NonZeroHashOrEmpty(Latest);
			}
			else
			{
				Entry.LatestLocal = NonZeroHashOrEmpty(Latest);
			}
			double CreatedMs = 0.0;
			Data->TryGetNumberField(LoreJson::Created, CreatedMs);
			Entry.Created = FDateTime::FromUnixTimestamp(static_cast<int64>(CreatedMs / 1000.0));
			Data->TryGetBoolField(LoreJson::IsCurrent, Entry.bIsCurrent);
			Data->TryGetBoolField(LoreJson::Archived, Entry.bArchived);
			OutEntries.Add(MoveTemp(Entry));
		}
	}

	// One metadata event's value is a tagged object whose "data" is the payload. Read the requested key's value as text (boolean as "true"/"false", number decimal, string verbatim), so the CLI mirror matches the native wrapper. bFound stays false until a matching key arrives.
	void ParseBranchMetadataResults(const TArray<FString>& InResults, const FString& InKey, FString& OutValue, bool& bOutFound)
	{
		for (const FString& Line : InResults)
		{
			FString Tag;
			TSharedPtr<FJsonObject> Data;
			if (!ParseJsonEvent(Line, Tag, Data) || !Data.IsValid() || Tag != LoreJson::Tags::Metadata)
			{
				continue;
			}
			FString Key;
			Data->TryGetStringField(LoreJson::Key, Key);
			if (Key != InKey)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* ValueField = nullptr;
			if (!Data->TryGetObjectField(LoreJson::Value, ValueField) || ValueField == nullptr)
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Payload = (*ValueField)->TryGetField(LoreJson::Data);
			if (Payload.IsValid())
			{
				bool bBool = false;
				FString AsString;
				double AsNumber = 0.0;
				if (Payload->Type == EJson::Boolean)
				{
					OutValue = Payload->AsBool() ? TEXT("true") : TEXT("false");
				}
				else if (Payload->TryGetString(AsString))
				{
					OutValue = AsString;
				}
				else if (Payload->TryGetNumber(AsNumber))
				{
					OutValue = FString::Printf(TEXT("%lld"), static_cast<int64>(AsNumber));
				}
			}
			bOutFound = true;
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
	bool bRemoteAvailable = false;
	ParseStatusResults(InResults, OutStates, bOutRemoteAhead, bRemoteAvailable);
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

bool RunUpdateStatus(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InIdentity, bool bInUsingLocking, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages, TArray<FLoreSourceControlState>& OutStates, bool bInRemoteAvailable)
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
	// Reachability is read fresh from the status probe, which reports remoteAvailable, and falls back to the caller's hint only when the probe does not run. The lock query and check-out availability follow this, so a server that went down mid-session is caught this pass rather than hanging the lock query on a long timeout.
	bool bRemoteReachable = bInRemoteAvailable;
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
				bRemoteReachable = ClientResult.bRemoteAvailable;
				bStatusRead = true;
			}
			else if (!FallsBackToCommandLine(InMode))
			{
				// Native mode does not retry on the command line; report the failure and stop. A failed status read means the server could not be reached, so do not then run the lock query.
				OutErrorMessages.Add(ClientError);
				bOk = false;
				bStatusRead = true;
				bRemoteReachable = false;
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
			ParseStatusResults(Results, Parsed, bBehindRemote, bRemoteReachable);
		}

		// The lock query is purely server-side and blocks for minutes against an unreachable server, so skip it when the status probe just reported the remote down. Lock owners then read as unknown for this pass, which is what offline editing expects.
		if (bInUsingLocking && bRemoteReachable)
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
		State.bRemoteAvailable = bRemoteReachable;
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

bool RunRevisionList(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InStartRevision, int32 InLength, TArray<FLoreRevisionListEntry>& OutRevisions, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		TArray<LoreClient::FRevisionEntry> ClientRevisions;
		FString ClientError;
		if (LoreClient::RevisionList(InRepositoryRoot, InBranch, InStartRevision, InLength, ClientRevisions, ClientError))
		{
			for (const LoreClient::FRevisionEntry& In : ClientRevisions)
			{
				FLoreRevisionListEntry Out;
				Out.Hash = In.Hash;
				Out.RevisionNumber = In.RevisionNumber;
				Out.ParentHash = In.ParentHash;
				Out.ParentOtherHash = In.ParentOtherHash;
				Out.Date = In.Date;
				Out.Message = In.Message;
				Out.Author = In.Author;
				OutRevisions.Add(MoveTemp(Out));
			}
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore revision history failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--json"));
	if (!InStartRevision.IsEmpty())
	{
		Parameters.Add(FString::Printf(TEXT("--revision %s"), *InStartRevision));
	}
	if (!InBranch.IsEmpty())
	{
		Parameters.Add(FString::Printf(TEXT("--branch \"%s\""), *InBranch));
	}
	if (InLength > 0)
	{
		Parameters.Add(FString::FromInt(InLength));
	}
	TArray<FString> Results;
	if (!RunCommand(TEXT("revision history"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages))
	{
		return false;
	}
	ParseRevisionListResults(Results, OutRevisions);
	return true;
}

bool RunRevisionInfo(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InRevision, bool bIncludeDelta, FLoreRevisionDetails& OutDetails, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		LoreClient::FRevisionDetails ClientDetails;
		FString ClientError;
		if (LoreClient::RevisionInfo(InRepositoryRoot, InRevision, bIncludeDelta, ClientDetails, ClientError))
		{
			OutDetails.Hash = ClientDetails.Hash;
			OutDetails.RevisionNumber = ClientDetails.RevisionNumber;
			OutDetails.Date = ClientDetails.Date;
			OutDetails.Message = ClientDetails.Message;
			OutDetails.Author = ClientDetails.Author;
			for (const LoreClient::FRevisionDelta& In : ClientDetails.Deltas)
			{
				FLoreRevisionDeltaEntry Out;
				Out.Path = In.Path;
				Out.Action = In.Action;
				Out.Size = In.Size;
				Out.bModified = In.bModified;
				Out.bMerged = In.bMerged;
				Out.bIsFile = In.bIsFile;
				OutDetails.Deltas.Add(MoveTemp(Out));
			}
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore revision info failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--json"));
	Parameters.Add(TEXT("--metadata"));
	if (bIncludeDelta)
	{
		Parameters.Add(TEXT("--delta"));
	}
	if (!InRevision.IsEmpty())
	{
		Parameters.Add(InRevision);
	}
	TArray<FString> Results;
	if (!RunCommand(TEXT("revision info"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages))
	{
		return false;
	}
	ParseRevisionInfoResults(Results, OutDetails);
	return true;
}

void MergeBranchListEntries(const TArray<FLoreBranchItem>& InEntries, TArray<FLoreBranchItem>& OutItems)
{
	// One row per name, keyed by name so a branch listed both locally and remotely folds to a single item. A later entry for the same name merges its listing side and its latest.
	TMap<FString, int32> IndexByName;
	for (const FLoreBranchItem& In : InEntries)
	{
		int32* Existing = IndexByName.Find(In.Name);
		if (!Existing)
		{
			IndexByName.Add(In.Name, OutItems.Add(In));
			continue;
		}
		FLoreBranchItem& Row = OutItems[*Existing];
		Row.bHasLocal |= In.bHasLocal;
		Row.bHasRemote |= In.bHasRemote;
		if (In.bHasLocal)
		{
			Row.LatestLocal = In.LatestLocal;
			Row.bIsCurrent = In.bIsCurrent;
			Row.bArchived = In.bArchived;
			// The local entry carries the branch's own metadata; keep its category, creator, and created over a remote entry's.
			Row.Category = In.Category;
			Row.Creator = In.Creator;
			Row.Created = In.Created;
		}
		if (In.bHasRemote)
		{
			Row.LatestRemote = In.LatestRemote;
		}
	}
}

bool RunGetBranches(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, bool bIncludeArchived, TArray<FLoreBranchItem>& OutBranches, TArray<FString>& OutErrorMessages)
{
	TArray<FLoreBranchItem> Entries;

#if LORE_WITH_LIBLORE
	bool bListed = false;
	if (UseNativeBackend(InMode))
	{
		TArray<LoreClient::FBranchInfo> ClientBranches;
		FString ClientError;
		if (LoreClient::BranchList(InRepositoryRoot, bIncludeArchived, ClientBranches, ClientError))
		{
			for (const LoreClient::FBranchInfo& In : ClientBranches)
			{
				FLoreBranchItem Entry;
				Entry.Name = In.Name;
				Entry.Category = In.Category;
				Entry.Creator = In.Creator;
				Entry.Created = In.Created;
				Entry.bIsCurrent = In.bIsCurrent;
				Entry.bArchived = In.bArchived;
				Entry.bHasLocal = !In.bIsRemote;
				Entry.bHasRemote = In.bIsRemote;
				const FString Latest = IsZeroHash(In.LatestHash) ? FString() : In.LatestHash;
				if (In.bIsRemote) { Entry.LatestRemote = Latest; } else { Entry.LatestLocal = Latest; }
				Entries.Add(MoveTemp(Entry));
			}
			bListed = true;
		}
		else if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		else
		{
			UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore branch list failed, falling back to the CLI: %s"), *ClientError);
		}
	}
	if (!bListed)
#endif
	{
		TArray<FString> Parameters;
		Parameters.Add(TEXT("--json"));
		if (bIncludeArchived)
		{
			Parameters.Add(TEXT("--archived"));
		}
		TArray<FString> Results;
		if (!RunCommand(TEXT("branch list"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages))
		{
			return false;
		}
		ParseBranchListResults(Results, Entries);
	}

	MergeBranchListEntries(Entries, OutBranches);

	// An archived branch errors on info and metadata, so its protect stays false; a non-archived branch reads its "protect" key. A protection read that fails does not fail the listing.
	for (FLoreBranchItem& Row : OutBranches)
	{
		if (Row.bArchived || !Row.bHasLocal)
		{
			continue;
		}
		FString Value;
		bool bFound = false;
		TArray<FString> ProtectErrors;
		if (RunBranchMetadataGet(InMode, InPathToLoreBinary, InRepositoryRoot, Row.Name, LoreJson::MetadataKeys::Protect, Value, bFound, ProtectErrors))
		{
			Row.bProtected = bFound && Value == TEXT("true");
		}
	}

	return true;
}

bool RunBranchMetadataGet(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InKey, FString& OutValue, bool& bOutFound, TArray<FString>& OutErrorMessages)
{
	OutValue.Empty();
	bOutFound = false;

#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::BranchMetadataGet(InRepositoryRoot, InBranch, InKey, OutValue, bOutFound, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore branch metadata get failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--json"));
	Parameters.Add(FString::Printf(TEXT("--branch \"%s\""), *InBranch));
	Parameters.Add(InKey);
	TArray<FString> Results;
	if (!RunCommand(TEXT("branch metadata get"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages))
	{
		return false;
	}
	ParseBranchMetadataResults(Results, InKey, OutValue, bOutFound);
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

bool GetStatusRevisionAnchors(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, FLoreStatusAnchors& OutAnchors, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		LoreClient::FStatusResult Result;
		FString Error;
		if (LoreClient::GetStatus(InRepositoryRoot, FString(), TArray<FString>(), false, Result, Error))
		{
			OutAnchors.BranchName = Result.BranchName;
			OutAnchors.CurrentRevision = Result.CurrentRevision;
			OutAnchors.CurrentRevisionNumber = Result.CurrentRevisionNumber;
			OutAnchors.RemoteRevision = IsZeroHash(Result.RemoteRevision) ? FString() : Result.RemoteRevision;
			OutAnchors.RemoteRevisionNumber = Result.RemoteRevisionNumber;
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(Error);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore status (anchors) failed, falling back to the CLI: %s"), *Error);
	}
#endif
	// Plain status, no scan: a light read of the revision header, not a tree walk that persists dirty state.
	TArray<FString> Results;
	if (!RunCommand(TEXT("status"), InPathToLoreBinary, InRepositoryRoot, { TEXT("--json") }, TArray<FString>(), Results, OutErrorMessages))
	{
		return false;
	}
	ParseStatusAnchorsResults(Results, OutAnchors);
	if (IsZeroHash(OutAnchors.RemoteRevision))
	{
		OutAnchors.RemoteRevision.Empty();
	}
	return !OutAnchors.CurrentRevision.IsEmpty();
}

bool EnumerateSyncDelta(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, TArray<FLoreDeltaEntry>& OutDelta, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		TArray<LoreClient::FSyncDeltaFile> ClientDelta;
		FString Error;
		if (LoreClient::EnumerateSyncDelta(InRepositoryRoot, ClientDelta, Error))
		{
			for (const LoreClient::FSyncDeltaFile& File : ClientDelta)
			{
				if (File.Path.IsEmpty())
				{
					continue;
				}
				FLoreDeltaEntry Entry;
				Entry.Path = File.Path;
				Entry.Path.ReplaceInline(TEXT("\\"), TEXT("/"));
				Entry.Action = File.Action;
				OutDelta.Add(MoveTemp(Entry));
			}
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(Error);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore dry-run sync failed, falling back to the CLI: %s"), *Error);
	}
#endif
	// The dry-run global flag reports the delta and writes nothing, so parse the same revisionSyncFile stream the switch dry-run emits. Globals precede the subcommand: `lore --dry-run --json sync`.
	TArray<FString> Results;
	if (!RunCommand(TEXT("--dry-run --json sync"), InPathToLoreBinary, InRepositoryRoot, TArray<FString>(), TArray<FString>(), Results, OutErrorMessages))
	{
		return false;
	}
	ParseSyncDeltaResults(Results, OutDelta);
	return true;
}

bool EnumerateSwitchDelta(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, TArray<FLoreDeltaEntry>& OutDelta, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		TArray<LoreClient::FSyncDeltaFile> ClientDelta;
		FString Error;
		if (LoreClient::EnumerateSwitchDelta(InRepositoryRoot, InBranch, InRevision, ClientDelta, Error))
		{
			for (const LoreClient::FSyncDeltaFile& File : ClientDelta)
			{
				if (File.Path.IsEmpty())
				{
					continue;
				}
				FLoreDeltaEntry Entry;
				Entry.Path = File.Path;
				Entry.Path.ReplaceInline(TEXT("\\"), TEXT("/"));
				Entry.Action = File.Action;
				OutDelta.Add(MoveTemp(Entry));
			}
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(Error);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore dry-run switch failed, falling back to the CLI: %s"), *Error);
	}
#endif
	// The switch dry-run emits the same revisionSyncFile stream the sync dry-run does. A dry-run also emits branchSwitchEnd, which the parser ignores, so completion is never inferred from it.
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--json"));
	Parameters.Add(FString::Printf(TEXT("\"%s\""), *InBranch));
	if (!InRevision.IsEmpty())
	{
		Parameters.Add(InRevision);
	}
	Parameters.Add(TEXT("--dry-run"));
	TArray<FString> Results;
	if (!RunCommand(TEXT("branch switch"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages))
	{
		return false;
	}
	ParseSyncDeltaResults(Results, OutDelta);
	return true;
}

bool RunBranchCreate(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InName, const FString& InCategory, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		// A partial create is not safe to retry, so never fall back even in Automatic; surface the error instead.
		FString ClientError;
		if (!LoreClient::BranchCreate(InRepositoryRoot, InName, InCategory, ClientError))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		return true;
	}
#endif
	TArray<FString> Parameters;
	Parameters.Add(FString::Printf(TEXT("\"%s\""), *InName));
	TArray<FString> Results;
	return RunCommand(TEXT("branch create"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages);
}

bool RunBranchSwitch(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, const FString& InRevision, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		// A partial switch tears the working tree, so never fall back even in Automatic; surface the error instead.
		FString ClientError;
		if (!LoreClient::BranchSwitch(InRepositoryRoot, InBranch, InRevision, ClientError))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		return true;
	}
#endif
	// Never -f: it silently overwrites a conflicting local edit.
	TArray<FString> Parameters;
	Parameters.Add(FString::Printf(TEXT("\"%s\""), *InBranch));
	if (!InRevision.IsEmpty())
	{
		Parameters.Add(InRevision);
	}
	TArray<FString> Results;
	return RunCommand(TEXT("branch switch"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages);
}

bool RunBranchArchive(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::BranchArchive(InRepositoryRoot, InBranch, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore branch archive failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Parameters;
	Parameters.Add(FString::Printf(TEXT("\"%s\""), *InBranch));
	TArray<FString> Results;
	return RunCommand(TEXT("branch archive"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages);
}

bool RunBranchSetProtect(ELoreBackendMode InMode, const FString& InPathToLoreBinary, const FString& InRepositoryRoot, const FString& InBranch, bool bProtect, TArray<FString>& OutErrorMessages)
{
#if LORE_WITH_LIBLORE
	if (UseNativeBackend(InMode))
	{
		FString ClientError;
		if (LoreClient::BranchSetProtect(InRepositoryRoot, InBranch, bProtect, ClientError))
		{
			return true;
		}
		if (!FallsBackToCommandLine(InMode))
		{
			OutErrorMessages.Add(ClientError);
			return false;
		}
		UE_LOG(LogLoreSourceControl, Verbose, TEXT("liblore branch protect failed, falling back to the CLI: %s"), *ClientError);
	}
#endif
	TArray<FString> Parameters;
	Parameters.Add(FString::Printf(TEXT("\"%s\""), *InBranch));
	TArray<FString> Results;
	return RunCommand(bProtect ? TEXT("branch protect") : TEXT("branch unprotect"), InPathToLoreBinary, InRepositoryRoot, Parameters, TArray<FString>(), Results, OutErrorMessages);
}

void ParseSyncDeltaForTest(const TArray<FString>& InResults, TArray<FLoreDeltaEntry>& OutDelta)
{
	ParseSyncDeltaResults(InResults, OutDelta);
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

bool IsLockableAsset(const FString& InFile)
{
	const FString Extension = FPaths::GetExtension(InFile);
	return Extension.Equals(TEXT("uasset"), ESearchCase::IgnoreCase) || Extension.Equals(TEXT("umap"), ESearchCase::IgnoreCase);
}

void SetFilesReadOnly(const TArray<FString>& InFiles, bool bReadOnly)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	for (const FString& File : InFiles)
	{
		if (PlatformFile.FileExists(*File))
		{
			PlatformFile.SetReadOnly(*File, bReadOnly);
		}
	}
}

void ApplyReadOnlyFromStates(const TArray<FLoreSourceControlState>& InStates)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	for (const FLoreSourceControlState& State : InStates)
	{
		if (!IsLockableAsset(State.LocalFilename) || !PlatformFile.FileExists(*State.LocalFilename))
		{
			continue;
		}
		const bool bCommitted = State.WorkingCopyState == ELoreWorkingCopyState::Unchanged
			|| State.WorkingCopyState == ELoreWorkingCopyState::Modified
			|| State.WorkingCopyState == ELoreWorkingCopyState::Conflicted;
		const bool bReadOnly = bCommitted && State.LockState != ELoreLockState::Locked;
		PlatformFile.SetReadOnly(*State.LocalFilename, bReadOnly);
	}
}

bool UpdateCachedStates(const TArray<FLoreSourceControlState>& InStates)
{
	check(IsInGameThread());
	FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
	bool bChanged = false;
	for (const FLoreSourceControlState& InState : InStates)
	{
		TSharedRef<FLoreSourceControlState, ESPMode::ThreadSafe> Cached = Provider.GetStateInternal(InState.LocalFilename);
		if (Cached->WorkingCopyState != InState.WorkingCopyState || Cached->LockState != InState.LockState || Cached->LockUser != InState.LockUser || Cached->bNewerVersionOnServer != InState.bNewerVersionOnServer || Cached->bRemoteAvailable != InState.bRemoteAvailable)
		{
			bChanged = true;
		}

		Cached->WorkingCopyState = InState.WorkingCopyState;
		Cached->LockState = InState.LockState;
		Cached->LockUser = InState.LockUser;
		Cached->bUsingLocking = InState.bUsingLocking;
		Cached->bRemoteAvailable = InState.bRemoteAvailable;
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
